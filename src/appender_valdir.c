/*-------------------------------------------------------------------------
 *
 * appender_valdir.c
 *    Value directories of appender segments and out-of-line value fetch.
 *
 * External values are stored in dedicated value blocks; a chained directory
 * block records every (valueid, offset) pair a segment holds.  The directory
 * is parsed lazily into a per-backend cache, and a TOAST slice request is
 * served by looking the value up in the directory first, falling back to a
 * scan of the requesting command's own uncommitted appends.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/fd.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "appender_datafile.h"
#include "appender_metadata.h"
#include "appender_valdir.h"
#include "appender_writer.h"

int			appender_valdir_cache_mb = 64;

typedef struct AppenderValDirCacheKey
{
	RelFileLocator rlocator;
	int			segno;
} AppenderValDirCacheKey;

/* Newest cached directory of a segment: a sorted array of all its entries. */
typedef struct AppenderValDirCache
{
	AppenderValDirCacheKey key;
	int64		valdir_off;		/* chain tip the payload was built from */
	int			nentries;
	char	   *payload;		/* AppenderValDirEntry array, sorted */
	Size		payload_bytes;
	uint64		last_used;
} AppenderValDirCache;

static HTAB *appender_valdir_cache = NULL;
static Size appender_valdir_cache_bytes = 0;
static uint64 appender_valdir_cache_tick = 0;

static void
appender_valdir_cache_evict(AppenderValDirCache *protect)
{
	Size		limit = (Size) appender_valdir_cache_mb * 1024 * 1024;

	if (appender_valdir_cache == NULL)
		return;

	while (appender_valdir_cache_bytes > limit)
	{
		HASH_SEQ_STATUS status;
		AppenderValDirCache *entry;
		AppenderValDirCache *oldest = NULL;

		hash_seq_init(&status, appender_valdir_cache);
		while ((entry = hash_seq_search(&status)) != NULL)
		{
			if (entry == protect)
				continue;
			if (oldest == NULL || entry->last_used < oldest->last_used)
				oldest = entry;
		}

		if (oldest == NULL)
			break;

		if (oldest->payload)
		{
			appender_valdir_cache_bytes -= oldest->payload_bytes;
			pfree(oldest->payload);
		}
		hash_search(appender_valdir_cache, &oldest->key, HASH_REMOVE, NULL);
	}
}

void
appender_valdir_cache_reset(void)
{
	HASH_SEQ_STATUS status;
	AppenderValDirCache *e;

	if (appender_valdir_cache == NULL)
		return;

	hash_seq_init(&status, appender_valdir_cache);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		if (e->payload)
			pfree(e->payload);
	}
	hash_destroy(appender_valdir_cache);
	appender_valdir_cache = NULL;
	appender_valdir_cache_bytes = 0;
	appender_valdir_cache_tick = 0;
}

int
appender_valdir_entry_cmp(const void *a, const void *b)
{
	const AppenderValDirEntry *ea = (const AppenderValDirEntry *) a;
	const AppenderValDirEntry *eb = (const AppenderValDirEntry *) b;

	if (ea->valueid < eb->valueid)
		return -1;
	if (ea->valueid > eb->valueid)
		return 1;
	return 0;
}

/*
 * Build (or fetch from the per-backend cache) the sorted directory of a
 * segment: every (valueid, offset) pair reachable by walking the chained
 * directory blocks.  The chain is walked once per (segment, chain tip).
 */
static AppenderValDirCache *
appender_valdir_load(const RelFileLocator *rlocator, int segno, int64 valdir_off)
{
	AppenderValDirCacheKey key = {*rlocator, segno};
	AppenderValDirCache *entry;
	bool		found;
	AppenderValDirEntry *all;
	char	   *cached;
	int			total = 0;
	int			cap = 256;
	int64		doff = valdir_off;
	volatile File file = -1;
	Size		bytes;

	if (appender_valdir_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(key);
		ctl.entrysize = sizeof(AppenderValDirCache);
		ctl.hcxt = TopMemoryContext;
		appender_valdir_cache = hash_create("appender valdir", 16, &ctl,
											HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	entry = hash_search(appender_valdir_cache, &key, HASH_FIND, NULL);
	if (entry != NULL && entry->valdir_off == valdir_off)
	{
		entry->last_used = ++appender_valdir_cache_tick;
		return entry;
	}

	all = palloc(cap * sizeof(AppenderValDirEntry));
	PG_TRY();
	{
		if (doff > 0)
			file = appender_seg_open(rlocator, segno);

		while (doff > 0)
		{
			int64		cur = doff;
			AppenderBlockHeader hdr;
			char	   *payload;
			int			n;
			int64		expected;

			appender_seg_read_header(file, doff, &hdr);
			if (hdr.kind != APPENDER_BLOCK_KIND_VALDIR)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid value directory at offset " INT64_FORMAT " of segment %d of relation %u/%u/%u",
								(int64) doff, segno,
								rlocator->spcOid, rlocator->dbOid,
								rlocator->relNumber)));
			payload = palloc(hdr.datalen);
			appender_seg_read_payload(file, doff, &hdr, payload);

			n = (int) hdr.rowcount;
			expected = sizeof(int64) + (int64) n * sizeof(AppenderValDirEntry);
			if (expected != hdr.datalen)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid value directory payload at offset " INT64_FORMAT " of segment %d of relation %u/%u/%u",
								(int64) doff, segno,
								rlocator->spcOid, rlocator->dbOid,
								rlocator->relNumber)));

			if (total + n > cap)
			{
				while (total + n > cap)
					cap *= 2;
				all = repalloc(all, cap * sizeof(AppenderValDirEntry));
			}
			memcpy(all + total, payload + sizeof(int64),
				   n * sizeof(AppenderValDirEntry));
			total += n;
			doff = *(int64 *) payload;
			if (doff != 0 && (doff < APPENDER_SEG_DATA_START || doff >= cur))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid value directory chain link in segment %d of relation %u/%u/%u",
								segno, rlocator->spcOid, rlocator->dbOid,
								rlocator->relNumber)));
			pfree(payload);
		}
	}
	PG_CATCH();
	{
		if (file >= 0)
			FileClose(file);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (file >= 0)
		FileClose(file);

	qsort(all, total, sizeof(AppenderValDirEntry), appender_valdir_entry_cmp);

	bytes = total * sizeof(AppenderValDirEntry);
	cached = MemoryContextAlloc(TopMemoryContext, bytes);
	memcpy(cached, all, bytes);

	/* nothing below can fail, so the cache never holds a half-built entry */
	entry = hash_search(appender_valdir_cache, &key, HASH_ENTER, &found);
	if (found)
	{
		appender_valdir_cache_bytes -= entry->payload_bytes;
		pfree(entry->payload);
	}
	entry->payload = cached;
	entry->nentries = total;
	entry->valdir_off = valdir_off;
	entry->payload_bytes = bytes;
	entry->last_used = ++appender_valdir_cache_tick;
	appender_valdir_cache_bytes += bytes;
	appender_valdir_cache_evict(entry);
	pfree(all);
	return entry;
}

/* Binary search a sorted run of directory entries. */
static bool
appender_valdir_search(const AppenderValDirEntry *entries, int n, Oid valueid,
					   int64 *off)
{
	int			lo = 0;
	int			hi = n - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (entries[mid].valueid == valueid)
		{
			*off = entries[mid].offset;
			return true;
		}
		if (entries[mid].valueid < valueid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/*
 * Find a value block in one segment: the committed directory first, then a
 * scan of [scan_lo, scan_hi), the caller's own uncommitted appends that no
 * directory published yet.  A hit by scan leaves the segment open in
 * *found_file for the caller's read.
 */
static bool
appender_find_toast_value_in_segment(const RelFileLocator *rlocator, int segno,
									 int64 valdir_off, int64 scan_lo,
									 int64 scan_hi, Oid valueid, int64 *off,
									 File *found_file)
{
	AppenderBlockHeader hdr;
	File		file;

	if (valdir_off > 0)
	{
		AppenderValDirCache *dir =
			appender_valdir_load(rlocator, segno, valdir_off);

		if (appender_valdir_search((AppenderValDirEntry *) dir->payload,
								   dir->nentries, valueid, off))
			return true;
	}

	if (scan_hi <= scan_lo)
		return false;

	file = appender_seg_open(rlocator, segno);
	for (int64 p = scan_lo; p < scan_hi;)
	{
		appender_seg_read_header(file, p, &hdr);
		if (p + appender_block_size(&hdr) > scan_hi)
			break;
		if (hdr.kind == APPENDER_BLOCK_KIND_VALUE &&
			hdr.first_rownum == valueid)
		{
			*off = p;
			*found_file = file;
			return true;
		}
		p += appender_block_size(&hdr);
	}
	FileClose(file);
	return false;
}

/*
 * Fetch a slice of an external value stored in value blocks.  The stored
 * bytes are copied verbatim, so a compressed value is returned in its stored
 * form and decompressed by the caller.  valueid is a global OID; it is found
 * through the segment's directory, with a scan fallback for values that are
 * still uncommitted.
 */
void
appender_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
						   int32 sliceoffset, int32 slicelength,
						   struct varlena *result)
{
	RelFileLocator rlocator = toastrel->rd_locator;
	AppenderSegment *meta;
	AppenderScanSeg *own = NULL;
	int			nmeta;
	int			nown;
	int			segno = -1;
	int64		off = -1;
	AppenderBlockHeader hdr;
	File		file = -1;

	if (slicelength <= 0)
		return;

	nmeta = appender_meta_segments(&rlocator, GetLatestSnapshot(), &meta);

	/*
	 * a row can be detoasted by the command that wrote it (RETURNING,
	 * triggers)
	 */
	nown = appender_own_segments(&rlocator, InvalidCommandId, &own);

	for (int i = 0; i < nmeta && segno < 0; i++)
	{
		int64		scan_lo = 0;
		int64		scan_hi = 0;

		for (int j = 0; j < nown; j++)
		{
			if (own[j].segno == meta[i].segno)
			{
				scan_lo = own[j].own_off;
				scan_hi = own[j].own_eof;
			}
		}

		if (appender_find_toast_value_in_segment(&rlocator, meta[i].segno,
												 meta[i].valdir_off,
												 scan_lo, scan_hi,
												 valueid, &off, &file))
			segno = meta[i].segno;
	}

	/* Also search own-only segments that have no manifest row yet. */
	for (int j = 0; j < nown && segno < 0; j++)
	{
		bool		in_meta = false;

		for (int i = 0; i < nmeta; i++)
		{
			if (meta[i].segno == own[j].segno)
			{
				in_meta = true;
				break;
			}
		}
		if (in_meta)
			continue;

		if (appender_find_toast_value_in_segment(&rlocator, own[j].segno, 0,
												 own[j].own_off, own[j].own_eof,
												 valueid, &off, &file))
			segno = own[j].segno;
	}

	if (own)
		pfree(own);
	pfree(meta);

	if (segno < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("external value %u is missing from \"%s\"",
						valueid, RelationGetRelationName(toastrel))));

	if (file < 0)
		file = appender_seg_open(&rlocator, segno);
	appender_seg_read_header(file, off, &hdr);
	if (hdr.kind != APPENDER_BLOCK_KIND_VALUE ||
		hdr.first_rownum != valueid ||
		attrsize != (int32) hdr.datalen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("external value %u does not match its block in segment %d of \"%s\"",
						valueid, segno, RelationGetRelationName(toastrel))));

	if (sliceoffset < 0 || slicelength < 0 ||
		(int64) sliceoffset + slicelength > hdr.datalen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("external value slice out of range for segment %d of \"%s\"",
						segno, RelationGetRelationName(toastrel))));

	appender_seg_read_slice(file, off, &hdr, sliceoffset, slicelength,
							VARDATA(result));
	FileClose(file);
}
