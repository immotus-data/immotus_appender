/*-------------------------------------------------------------------------
 *
 * appender_writer.c
 *    Insert path, per-transaction writer state and own-row visibility.
 *
 * A writer exists per (transaction, relation storage).  It owns the
 * segments it claimed, the block buffer and, for every segment, the byte and
 * row positions reached at the end of each command, so a snapshot taken
 * inside the transaction sees exactly the rows of earlier commands.
 *
 * Subtransactions are handled lazily: the first time a writer is modified at
 * a deeper nesting level, its buffer is flushed and its position saved, so
 * a rollback only has to restore positions; blocks written past the restored
 * position are overwritten by later inserts and are never visible.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "appender_datafile.h"
#include "appender_lifecycle.h"
#include "appender_metadata.h"
#include "appender_tid.h"
#include "appender_writer.h"
#include "appender_xlog.h"
#include "common/int.h"
#include "common/relpath.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#define APPENDER_BLOCK_TARGET		(32 * 1024)
#define APPENDER_BLOCK_MAX_ROWS		4096
#define APPENDER_TOAST_THRESHOLD	2048

int			appender_segment_size_mb = 1024;

typedef struct AppenderCidMark
{
	CommandId	cid;
	int64		eof;
	uint32		rows;
} AppenderCidMark;

typedef struct AppenderWSeg
{
	int32		segno;
	bool		is_new;			/* created by this transaction */
	bool		sealed;
	bool		sealed_here;	/* sealed by this transaction */
	bool		dirty;			/* blocks written, needs fsync */
	int64		start_eof;		/* committed position when claimed */
	uint32		start_rows;
	int64		eof;			/* current position, buffered rows included */
	int64		horizon;		/* zero-filled extent, -1 when unknown */
	uint32		rows;
	int64		valdir_off;		/* offset of the newest directory block */
	AppenderValDirEntry *pending; /* directory entries not yet flushed */
	int			npending;
	int			maxpending;
	AppenderCidMark *marks;
	int			nmarks;
	int			maxmarks;
} AppenderWSeg;

typedef struct AppenderWMark
{
	int			level;
	int			nsegs;
	int64		eof;
	uint32		rows;
	int64		valdir_off;
	bool		sealed;
	bool		sealed_here;
	int			nmarks;
	AppenderCidMark lastmark;
} AppenderWMark;

typedef struct AppenderWriter
{
	RelFileLocator rlocator;	/* hash key */
	int			create_level;
	AppenderWSeg *segs;
	int			nsegs;
	int			maxsegs;
	File		file;
	int			file_segno;
	char	   *buf;
	Size		buflen;
	Size		bufcap;
	uint32		bufrows;
	int			bufnatts;
	int64		bufoff;
	uint32		buffirst;
	AppenderWMark *stack;
	int			nstack;
	int			maxstack;
} AppenderWriter;

static HTAB *appender_writers = NULL;
static Oid appender_last_valueid = InvalidOid;
static bool appender_valueid_wrapped = false;

static void *
appender_grow(void *array, int *max, Size elemsize)
{
	if (array == NULL)
	{
		*max = 4;
		return MemoryContextAlloc(TopTransactionContext, *max * elemsize);
	}
	*max *= 2;
	return repalloc(array, *max * elemsize);
}

static AppenderWriter *
appender_get_writer(const RelFileLocator *rlocator, bool create)
{
	AppenderWriter *w;
	bool		found;

	if (appender_writers == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(AppenderWriter);
		ctl.hcxt = TopTransactionContext;
		appender_writers = hash_create("appender writers", 16, &ctl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	w = hash_search(appender_writers, rlocator, create ? HASH_ENTER : HASH_FIND,
					&found);
	if (w && !found)
	{
		memset((char *) w + sizeof(RelFileLocator), 0,
			   sizeof(AppenderWriter) - sizeof(RelFileLocator));
		w->create_level = GetCurrentTransactionNestLevel();
		w->file = -1;
	}
	return w;
}

static inline AppenderWSeg *
appender_cur_seg(AppenderWriter *w)
{
	return w->nsegs > 0 ? &w->segs[w->nsegs - 1] : NULL;
}

static bool
appender_seg_full(const AppenderWSeg *seg)
{
	int64		limit = (int64) appender_segment_size_mb * 1024 * 1024;

	return seg->eof >= limit ||
		seg->rows + APPENDER_BLOCK_MAX_ROWS > APPENDER_ROWS_PER_SEGMENT;
}

static void
appender_close_file(AppenderWriter *w)
{
	if (w->file >= 0)
		FileClose(w->file);
	w->file = -1;
	w->file_segno = 0;
}

static File
appender_writer_file(AppenderWriter *w, int segno)
{
	if (w->file < 0 || w->file_segno != segno)
	{
		appender_close_file(w);
		w->file = appender_seg_open(&w->rlocator, segno);
		w->file_segno = segno;
	}
	return w->file;
}

/*
 * Segment locks are transaction-scoped object locks on (appender_meta,
 * relfilenode, segno).  They are only ever taken without waiting.
 */
static void
appender_seg_locktag(LOCKTAG *tag, const RelFileLocator *rlocator, int segno)
{
	SET_LOCKTAG_OBJECT(*tag, rlocator->dbOid, appender_meta_relid(false),
					   rlocator->relNumber, (uint16) segno);
}

static bool
appender_seg_trylock(const RelFileLocator *rlocator, int segno)
{
	LOCKTAG		tag;

	appender_seg_locktag(&tag, rlocator, segno);
	return LockAcquire(&tag, ExclusiveLock, false, true) != LOCKACQUIRE_NOT_AVAIL;
}

static void
appender_seg_unlock(const RelFileLocator *rlocator, int segno)
{
	LOCKTAG		tag;

	appender_seg_locktag(&tag, rlocator, segno);
	LockRelease(&tag, ExclusiveLock, false);
}

static bool
appender_writer_has_seg(AppenderWriter *w, int segno)
{
	for (int i = 0; i < w->nsegs; i++)
		if (w->segs[i].segno == segno)
			return true;
	return false;
}

static AppenderWSeg *
appender_add_seg(AppenderWriter *w, int segno, bool is_new, int64 eof,
				 uint32 rows, int64 valdir_off)
{
	AppenderWSeg *seg;

	if (w->nsegs == w->maxsegs)
		w->segs = appender_grow(w->segs, &w->maxsegs, sizeof(AppenderWSeg));
	seg = &w->segs[w->nsegs++];
	memset(seg, 0, sizeof(*seg));
	seg->segno = segno;
	seg->is_new = is_new;
	seg->start_eof = seg->eof = eof;
	seg->horizon = -1;
	seg->start_rows = seg->rows = rows;
	seg->valdir_off = valdir_off;
	seg->pending = NULL;
	seg->npending = 0;
	seg->maxpending = 0;
	return seg;
}

/*
 * Claim a segment to append to: an open segment no other transaction holds,
 * or else a new one.  Positions are re-read after the lock is taken, because
 * the previous holder may have committed in between.
 */
static void appender_flush_valdir(AppenderWriter *w, AppenderWSeg *seg);

static void
appender_claim_segment(AppenderWriter *w)
{
	AppenderSegment *segs;
	int			nsegs = appender_meta_segments(&w->rlocator, GetLatestSnapshot(),
											   &segs);
	int			maxsegno = APPENDER_FIRST_SEGNO - 1;
	AppenderSegment cur;

	for (int i = 0; i < nsegs; i++)
		maxsegno = Max(maxsegno, segs[i].segno);
	for (int i = 0; i < w->nsegs; i++)
		maxsegno = Max(maxsegno, w->segs[i].segno);

	for (int i = 0; i < nsegs; i++)
	{
		int			segno = segs[i].segno;

		if (segs[i].state != APPENDER_SEG_OPEN ||
			appender_writer_has_seg(w, segno) ||
			!appender_seg_trylock(&w->rlocator, segno))
			continue;

		if (appender_meta_get(&w->rlocator, segno, GetLatestSnapshot(), &cur) &&
			cur.state == APPENDER_SEG_OPEN)
		{
			AppenderWSeg *seg = appender_add_seg(w, segno, false, cur.eof,
												 (uint32) cur.rowcount,
												 cur.valdir_off);

			if (!appender_seg_full(seg))
			{
				pfree(segs);
				return;
			}
			/* left full by its last writer: seal it on our way past */
			seg->sealed = seg->sealed_here = true;
			continue;
		}
		appender_seg_unlock(&w->rlocator, segno);
	}
	pfree(segs);

	for (int segno = maxsegno + 1;; segno++)
	{
		if (segno > APPENDER_MAX_SEGNO)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("appender relation has too many segments")));

		if (!appender_seg_trylock(&w->rlocator, segno))
			continue;

		if (appender_meta_get(&w->rlocator, segno, GetLatestSnapshot(), &cur))
		{
			if (cur.state == APPENDER_SEG_OPEN)
			{
				AppenderWSeg *seg = appender_add_seg(w, segno, false, cur.eof,
													 (uint32) cur.rowcount,
													 cur.valdir_off);

				if (!appender_seg_full(seg))
					return;
				seg->sealed = seg->sealed_here = true;
				continue;
			}
			appender_seg_unlock(&w->rlocator, segno);
			continue;
		}

		appender_close_file(w);
		w->file = appender_seg_create(&w->rlocator, segno);
		w->file_segno = segno;
		appender_xlog_create(&w->rlocator, segno);
		appender_add_seg(w, segno, true, APPENDER_SEG_DATA_START, 0, 0);
		return;
	}
}

static void
appender_buf_flush(AppenderWriter *w)
{
	AppenderWSeg *seg = appender_cur_seg(w);
	AppenderBlockHeader hdr;
	File		file;

	if (w->bufrows == 0)
		return;

	memset(&hdr, 0, sizeof(hdr));
	hdr.version = APPENDER_FORMAT_VERSION;
	hdr.kind = APPENDER_BLOCK_KIND_DATA;
	hdr.rowcount = w->bufrows;
	hdr.first_rownum = w->buffirst;
	hdr.datalen = hdr.rawlen = (uint32) w->buflen;
	hdr.natts = (uint16) w->bufnatts;
	hdr.codec = APPENDER_CODEC_NONE;

	file = appender_writer_file(w, seg->segno);
	appender_seg_ensure_room(file, w->bufoff + appender_block_size(&hdr),
							 &seg->horizon);
	appender_seg_write_block(file, w->bufoff, &hdr, w->buf);
	appender_xlog_block(&w->rlocator, seg->segno, w->bufoff, &hdr, w->buf);
	FileWriteback(file, w->bufoff, appender_block_size(&hdr),
				  WAIT_EVENT_DATA_FILE_FLUSH);

	seg->dirty = true;
	w->buflen = 0;
	w->bufrows = 0;
}

/* Save the writer's position the first time it changes at a new level. */
static void
appender_writer_savepoint(AppenderWriter *w)
{
	int			level = GetCurrentTransactionNestLevel();
	int			top = w->nstack > 0 ? w->stack[w->nstack - 1].level : w->create_level;
	AppenderWSeg *seg;
	AppenderWMark *m;

	if (level <= top)
		return;

	appender_buf_flush(w);
	if (appender_cur_seg(w) != NULL)
		appender_flush_valdir(w, appender_cur_seg(w));

	if (w->nstack == w->maxstack)
		w->stack = appender_grow(w->stack, &w->maxstack, sizeof(AppenderWMark));
	m = &w->stack[w->nstack++];
	memset(m, 0, sizeof(*m));
	m->level = level;
	m->nsegs = w->nsegs;
	seg = appender_cur_seg(w);
	if (seg)
	{
		m->eof = seg->eof;
		m->rows = seg->rows;
		m->valdir_off = seg->valdir_off;
		m->sealed = seg->sealed;
		m->sealed_here = seg->sealed_here;
		m->nmarks = seg->nmarks;
		if (seg->nmarks > 0)
			m->lastmark = seg->marks[seg->nmarks - 1];
	}
}

static void
appender_mark_cid(AppenderWSeg *seg, CommandId cid)
{
	AppenderCidMark *mark;

	if (seg->nmarks > 0 && seg->marks[seg->nmarks - 1].cid == cid)
		mark = &seg->marks[seg->nmarks - 1];
	else
	{
		if (seg->nmarks == seg->maxmarks)
			seg->marks = appender_grow(seg->marks, &seg->maxmarks,
									   sizeof(AppenderCidMark));
		mark = &seg->marks[seg->nmarks++];
		mark->cid = cid;
	}
	mark->eof = seg->eof;
	mark->rows = seg->rows;
}

/* Begin a new block, moving to another segment if the current one is full. */
static void
appender_buf_start(AppenderWriter *w, int natts)
{
	AppenderWSeg *seg = appender_cur_seg(w);

	if (seg != NULL && !seg->sealed && appender_seg_full(seg))
		seg->sealed = seg->sealed_here = true;
	if (seg == NULL || seg->sealed)
	{
		appender_claim_segment(w);
		seg = appender_cur_seg(w);
	}

	w->bufoff = seg->eof;
	w->buffirst = seg->rows;
	w->bufnatts = natts;
}

static AppenderWSeg *
appender_find_writer_seg(AppenderWriter *w, int segno)
{
	for (int i = 0; i < w->nsegs; i++)
	{
		if (w->segs[i].segno == segno)
			return &w->segs[i];
	}
	return NULL;
}

static bool
appender_segment_has_valueid(const RelFileLocator *rlocator, int segno,
						 int64 hi, Oid valueid)
{
	AppenderBlockHeader hdr;
	File		file;

	if (hi <= APPENDER_SEG_DATA_START)
		return false;

	file = appender_seg_open(rlocator, segno);
	for (int64 p = APPENDER_SEG_DATA_START; p < hi;)
	{
		appender_seg_read_header(file, p, &hdr);
		if (p + appender_block_size(&hdr) > hi)
			break;
		if (hdr.kind == APPENDER_BLOCK_KIND_VALUE &&
			hdr.first_rownum == valueid)
		{
			FileClose(file);
			return true;
		}
		p += appender_block_size(&hdr);
	}
	FileClose(file);
	return false;
}

static bool
appender_valueid_exists(AppenderWriter *w, Oid valueid)
{
	AppenderSegment *meta;
	int			nmeta;
	bool		exists = false;

	nmeta = appender_meta_segments(&w->rlocator, GetLatestSnapshot(), &meta);

	for (int i = 0; i < nmeta && !exists; i++)
	{
		AppenderWSeg *own = appender_find_writer_seg(w, meta[i].segno);
		int64		hi = meta[i].eof;

		if (own)
			hi = Max(hi, own->eof);
		exists = appender_segment_has_valueid(&w->rlocator, meta[i].segno,
										hi, valueid);
	}

	for (int i = 0; i < w->nsegs && !exists; i++)
	{
		bool		in_meta = false;

		for (int j = 0; j < nmeta; j++)
		{
			if (meta[j].segno == w->segs[i].segno)
			{
				in_meta = true;
				break;
			}
		}
		if (in_meta)
			continue;

		exists = appender_segment_has_valueid(&w->rlocator, w->segs[i].segno,
										w->segs[i].eof, valueid);
	}

	pfree(meta);
	return exists;
}

/*
 * Get a value identifier.  After this backend observes OID wrap-around,
 * verify every candidate against existing value blocks to avoid collisions
 * with very old pointers that still exist on disk.
 */
static Oid
appender_allocate_valueid(AppenderWriter *w)
{
	for (int tries = 0; tries < 1024; tries++)
	{
		Oid			valueid = GetNewObjectId();

		if (OidIsValid(appender_last_valueid) &&
			valueid <= appender_last_valueid)
			appender_valueid_wrapped = true;
		appender_last_valueid = valueid;

		if (!appender_valueid_wrapped || !appender_valueid_exists(w, valueid))
			return valueid;
	}

	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("could not allocate a unique external value identifier"),
			 errhint("Retry the statement after reducing old data or restarting the session.")));
}

/*
 * Store a varlena value in its own block and return the on-disk TOAST
 * pointer that references it.  The pointer names our relation as the toast
 * relation, so detoast dispatches to appender_fetch_toast_slice.
 */
static struct varlena *
appender_externalize_value(AppenderWriter *w, AppenderWSeg *seg, Oid relid,
						   struct varlena *value, char attstorage,
						   char attcompression)
{
	struct varatt_external toast_pointer;
	struct varlena *compressed = NULL;
	struct varlena *stored = value;
	struct varlena *result;
	bool		compressed_owned = false;
	Size		data_todo;
	AppenderBlockHeader hdr;
	File		file;
	int64		off;

	if (VARATT_IS_COMPRESSED(value))
	{
		/* already compressed (e.g. detoasted from a heap TOAST value) */
		compressed = value;
		stored = value;
	}
	else if (attstorage == TYPSTORAGE_EXTENDED)
	{
		Datum		c = toast_compress_datum(PointerGetDatum(value),
											 attcompression);

		if (DatumGetPointer(c) != NULL)
		{
			compressed = (struct varlena *) DatumGetPointer(c);
			stored = compressed;
			compressed_owned = true;
		}
	}

	if (compressed)
	{
		data_todo = VARSIZE(compressed) - VARHDRSZ;
		toast_pointer.va_rawsize =
			VARDATA_COMPRESSED_GET_EXTSIZE(compressed) + VARHDRSZ;
		VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, data_todo,
													 VARDATA_COMPRESSED_GET_COMPRESS_METHOD(compressed));
	}
	else
	{
		data_todo = VARSIZE(value) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(value);
		toast_pointer.va_extinfo = data_todo;
	}
	toast_pointer.va_valueid = appender_allocate_valueid(w);
	toast_pointer.va_toastrelid = relid;

	off = seg->eof;
	memset(&hdr, 0, sizeof(hdr));
	hdr.version = APPENDER_FORMAT_VERSION;
	hdr.kind = APPENDER_BLOCK_KIND_VALUE;
	hdr.rowcount = 1;
	hdr.first_rownum = toast_pointer.va_valueid;
	hdr.datalen = (uint32) data_todo;
	hdr.rawlen = (uint32) (toast_pointer.va_rawsize - VARHDRSZ);
	hdr.natts = 0;
	hdr.codec = APPENDER_CODEC_NONE;

	file = appender_writer_file(w, seg->segno);
	appender_seg_ensure_room(file, off + appender_block_size(&hdr),
							 &seg->horizon);
	appender_seg_write_block(file, off, &hdr, VARDATA(stored));
	appender_xlog_block(&w->rlocator, seg->segno, off, &hdr, VARDATA(stored));
	FileWriteback(file, off, appender_block_size(&hdr),
				  WAIT_EVENT_DATA_FILE_FLUSH);

	seg->eof = off + appender_block_size(&hdr);
	seg->dirty = true;

	/* record the value in the segment's directory */
	if (seg->npending == seg->maxpending)
	{
		int			newmax = seg->maxpending ? seg->maxpending * 2
			: APPENDER_VALDIR_INTERVAL;

		seg->pending = seg->pending
			? repalloc(seg->pending, newmax * sizeof(AppenderValDirEntry))
			: MemoryContextAlloc(TopTransactionContext,
								 newmax * sizeof(AppenderValDirEntry));
		seg->maxpending = newmax;
	}
	seg->pending[seg->npending].valueid = toast_pointer.va_valueid;
	seg->pending[seg->npending].offset = off;
	seg->npending++;
	if (seg->npending >= APPENDER_VALDIR_INTERVAL)
		appender_flush_valdir(w, seg);

	if (compressed_owned)
		pfree(compressed);

	result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));
	return result;
}

static int
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
 * Write the buffered directory entries as a VALDIR block chained to the
 * previous one.  Flushed every APPENDER_VALDIR_INTERVAL values, at savepoints
 * and at commit, so the open segment has a directory too and sealing stays a
 * pure flag flip.  Entries are sorted so the reader can binary-search them.
 */
static void
appender_flush_valdir(AppenderWriter *w, AppenderWSeg *seg)
{
	AppenderBlockHeader hdr;
	File		file;
	int64		off;
	int64		datalen;
	int64		prev_off = seg->valdir_off;
	char	   *payload;

	if (seg->npending == 0)
		return;

	qsort(seg->pending, seg->npending, sizeof(AppenderValDirEntry),
		  appender_valdir_entry_cmp);

	datalen = sizeof(int64) + seg->npending * sizeof(AppenderValDirEntry);
	payload = palloc(datalen);
	memcpy(payload, &prev_off, sizeof(int64));
	memcpy(payload + sizeof(int64), seg->pending,
		   seg->npending * sizeof(AppenderValDirEntry));

	memset(&hdr, 0, sizeof(hdr));
	hdr.version = APPENDER_FORMAT_VERSION;
	hdr.kind = APPENDER_BLOCK_KIND_VALDIR;
	hdr.rowcount = (uint32) seg->npending;
	hdr.first_rownum = seg->pending[0].valueid; /* smallest id after sorting */
	hdr.datalen = hdr.rawlen = (uint32) datalen;
	hdr.natts = 0;
	hdr.codec = APPENDER_CODEC_NONE;

	off = seg->eof;
	file = appender_writer_file(w, seg->segno);
	appender_seg_ensure_room(file, off + appender_block_size(&hdr),
							 &seg->horizon);
	appender_seg_write_block(file, off, &hdr, payload);
	appender_xlog_block(&w->rlocator, seg->segno, off, &hdr, payload);
	FileWriteback(file, off, appender_block_size(&hdr),
				  WAIT_EVENT_DATA_FILE_FLUSH);

	seg->eof = off + appender_block_size(&hdr);
	seg->valdir_off = off;
	seg->dirty = true;
	seg->npending = 0;
	pfree(payload);
}

void
appender_writer_insert(Relation rel, TupleTableSlot *slot, CommandId cid)
{
	TupleDesc	desc = RelationGetDescr(rel);
	AppenderWriter *w = appender_get_writer(&rel->rd_locator, true);
	Datum	   *values;
	bool	   *isnull;
	Datum	   *detoasted = NULL;
	MemTuple	mtup;
	uint32		mtsize;
	AppenderWSeg *seg;
	bool		need_ext = false;

	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	appender_writer_savepoint(w);

	slot_getallattrs(slot);
	values = slot->tts_values;
	isnull = slot->tts_isnull;

	/* MemTuples must be self-contained: fetch out-of-line values. */
	for (int i = 0; i < desc->natts; i++)
	{
		struct varlena *v;

		if (isnull[i] || TupleDescAttr(desc, i)->attlen != -1)
			continue;
		v = (struct varlena *) DatumGetPointer(values[i]);
		if (!VARATT_IS_EXTERNAL(v) || VARATT_IS_EXTERNAL_EXPANDED(v))
			continue;
		if (detoasted == NULL)
		{
			detoasted = palloc(desc->natts * sizeof(Datum));
			memcpy(detoasted, values, desc->natts * sizeof(Datum));
			values = detoasted;
		}
		values[i] = PointerGetDatum(detoast_external_attr(v));
	}

	/* Externalizing needs a fresh block, so it can start after value blocks. */
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		struct varlena *v;

		if (isnull[i] || attr->attlen != -1)
			continue;
		if (attr->attstorage != TYPSTORAGE_EXTENDED &&
			attr->attstorage != TYPSTORAGE_EXTERNAL)
			continue;
		v = (struct varlena *) DatumGetPointer(values[i]);
		if (VARATT_IS_EXTERNAL(v) || VARATT_IS_SHORT(v))
			continue;
		if (VARSIZE(v) - VARHDRSZ <= APPENDER_TOAST_THRESHOLD)
			continue;
		need_ext = true;
		break;
	}

	/* STORAGE MAIN values are compressed inline and never toasted. */
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		struct varlena *v;

		if (isnull[i] || attr->attlen != -1 ||
			attr->attstorage != TYPSTORAGE_MAIN)
			continue;
		v = (struct varlena *) DatumGetPointer(values[i]);
		if (VARATT_IS_EXTERNAL(v) || VARATT_IS_SHORT(v))
			continue;
		if (VARSIZE(v) - VARHDRSZ <= APPENDER_TOAST_THRESHOLD)
			continue;
		if (!VARATT_IS_COMPRESSED(v))
		{
			Datum		c = toast_compress_datum(PointerGetDatum(v),
												 attr->attcompression);

			if (DatumGetPointer(c) != NULL)
			{
				if (detoasted == NULL)
				{
					detoasted = palloc(desc->natts * sizeof(Datum));
					memcpy(detoasted, values, desc->natts * sizeof(Datum));
					values = detoasted;
				}
				values[i] = c;
			}
		}
	}

	if (w->bufrows > 0 && (w->bufnatts != desc->natts || need_ext))
		appender_buf_flush(w);
	if (w->bufrows == 0)
		appender_buf_start(w, desc->natts);
	seg = appender_cur_seg(w);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		struct varlena *v;

		if (isnull[i] || attr->attlen != -1)
			continue;
		if (attr->attstorage != TYPSTORAGE_EXTENDED &&
			attr->attstorage != TYPSTORAGE_EXTERNAL)
			continue;
		v = (struct varlena *) DatumGetPointer(values[i]);
		if (VARATT_IS_EXTERNAL(v) || VARATT_IS_SHORT(v))
			continue;
		if (VARSIZE(v) - VARHDRSZ <= APPENDER_TOAST_THRESHOLD)
			continue;

		values[i] = PointerGetDatum(appender_externalize_value(w, seg,
															  RelationGetRelid(rel),
															  v, attr->attstorage,
															  attr->attcompression));
	}

	if (need_ext)
	{
		w->bufoff = seg->eof;
		w->buffirst = seg->rows;
	}

	mtup = memtuple_form(appender_binding(rel, desc->natts), values, isnull);
	mtsize = memtuple_get_size(mtup);

	if (w->bufrows > 0 &&
		(w->buflen + mtsize > APPENDER_BLOCK_TARGET ||
		 w->bufrows >= APPENDER_BLOCK_MAX_ROWS))
	{
		appender_buf_flush(w);
		appender_buf_start(w, desc->natts);
	}

	if (w->buflen + mtsize > w->bufcap)
	{
		Size		newcap = Max(w->buflen + mtsize, (Size) APPENDER_BLOCK_TARGET);

		w->buf = w->buf ? repalloc(w->buf, newcap)
			: MemoryContextAlloc(TopTransactionContext, newcap);
		w->bufcap = newcap;
	}
	memcpy(w->buf + w->buflen, mtup, mtsize);
	w->buflen += mtsize;
	w->bufrows++;

	seg = appender_cur_seg(w);
	seg->eof = w->bufoff + sizeof(AppenderBlockHeader) + w->buflen;
	seg->rows = w->buffirst + w->bufrows;
	appender_mark_cid(seg, cid);

	appender_tid_set(&slot->tts_tid, seg->segno, seg->rows - 1);
	slot->tts_tableOid = RelationGetRelid(rel);

	pfree(mtup);
	if (detoasted)
		pfree(detoasted);
	pgstat_count_heap_insert(rel, 1);
}

/* Write out buffered rows so that a scan of the storage can read them. */
void
appender_writer_flush(const RelFileLocator *rlocator)
{
	AppenderWriter *w = appender_get_writer(rlocator, false);

	if (w)
		appender_buf_flush(w);
}

static void
appender_unlink_new_segs(AppenderWriter *w, int from)
{
	for (int i = from; i < w->nsegs; i++)
	{
		if (!w->segs[i].is_new)
			continue;
		if (w->file_segno == w->segs[i].segno)
			appender_close_file(w);
		appender_seg_unlink(&w->rlocator, w->segs[i].segno);
		appender_xlog_unlink(&w->rlocator, w->segs[i].segno);
	}
}

/* Forget a storage's writer whose segments were just removed. */
void
appender_writer_discard(const RelFileLocator *rlocator)
{
	AppenderWriter *w = appender_get_writer(rlocator, false);

	if (w)
	{
		appender_close_file(w);
		hash_search(appender_writers, rlocator, HASH_REMOVE, NULL);
	}
}

/*
 * This backend's own appends to a storage visible to a snapshot with the
 * given command id: rows of commands before curcid.
 */
int
appender_own_segments(const RelFileLocator *rlocator, CommandId curcid,
					  AppenderScanSeg **segs)
{
	AppenderWriter *w = appender_get_writer(rlocator, false);
	AppenderScanSeg *out;
	int			n = 0;

	*segs = NULL;
	if (w == NULL || w->nsegs == 0)
		return 0;

	out = palloc0(w->nsegs * sizeof(AppenderScanSeg));
	for (int i = 0; i < w->nsegs; i++)
	{
		AppenderWSeg *seg = &w->segs[i];
		AppenderCidMark *mark = NULL;

		for (int j = seg->nmarks - 1; j >= 0; j--)
		{
			if (seg->marks[j].cid < curcid)
			{
				mark = &seg->marks[j];
				break;
			}
		}
		if (mark == NULL || mark->rows == seg->start_rows)
			continue;

		out[n].segno = seg->segno;
		out[n].own_off = seg->start_eof;
		out[n].own_eof = mark->eof;
		out[n].own_first = seg->start_rows;
		out[n].own_rows = mark->rows - seg->start_rows;
		n++;
	}
	*segs = out;
	return n;
}

static int
appender_scanseg_cmp(const void *a, const void *b)
{
	return pg_cmp_s32(((const AppenderScanSeg *) a)->segno,
					  ((const AppenderScanSeg *) b)->segno);
}

/*
 * Segments of a storage as seen by snapshot: committed rows from the
 * manifest merged with this backend's own appends.
 */
int
appender_visible_segments(const RelFileLocator *rlocator, Snapshot snapshot,
						  const AppenderScanSeg *own, int nown,
						  AppenderScanSeg **segs)
{
	AppenderSegment *meta;
	int			nmeta = appender_meta_segments(rlocator, snapshot, &meta);
	AppenderScanSeg *out = palloc0((nmeta + nown + 1) * sizeof(AppenderScanSeg));
	int			n = 0;

	for (int i = 0; i < nmeta; i++)
	{
		out[n].segno = meta[i].segno;
		out[n].eof = meta[i].eof;
		out[n].rows = (uint32) meta[i].rowcount;
		n++;
	}
	pfree(meta);

	for (int j = 0; j < nown; j++)
	{
		AppenderScanSeg *dst = NULL;

		for (int i = 0; i < nmeta; i++)
		{
			if (out[i].segno == own[j].segno)
			{
				dst = &out[i];
				break;
			}
		}
		if (dst == NULL)
		{
			dst = &out[n++];
			dst->segno = own[j].segno;
			dst->eof = APPENDER_SEG_DATA_START;
			dst->rows = 0;
		}
		dst->own_off = own[j].own_off;
		dst->own_eof = own[j].own_eof;
		dst->own_first = own[j].own_first;
		dst->own_rows = own[j].own_rows;
	}

	if (n > 1)
		qsort(out, n, sizeof(AppenderScanSeg), appender_scanseg_cmp);
	*segs = out;
	return n;
}

bool
appender_writer_active(void)
{
	return appender_writers != NULL && hash_get_num_entries(appender_writers) > 0;
}

/*
 * Make this transaction's appends durable and publish them: write pending
 * blocks, fsync the touched segments (and the directory of new ones), then
 * store the new segment positions.  The commit record that follows makes the
 * manifest rows, and so the rows they cover, visible atomically.
 */
void
appender_writer_pre_commit(void)
{
	HASH_SEQ_STATUS status;
	AppenderWriter *w;

	if (appender_writers == NULL)
		return;

	hash_seq_init(&status, appender_writers);
	while ((w = hash_seq_search(&status)) != NULL)
	{
		bool		newfile = false;

		if (appender_storage_dropped(&w->rlocator))
			continue;

		appender_buf_flush(w);
		for (int i = 0; i < w->nsegs; i++)
			appender_flush_valdir(w, &w->segs[i]);

		for (int i = 0; i < w->nsegs; i++)
		{
			AppenderWSeg *seg = &w->segs[i];

			if (seg->dirty)
			{
				appender_seg_sync(appender_writer_file(w, seg->segno));
				appender_xlog_sync(&w->rlocator, seg->segno);
			}
			newfile |= seg->is_new;
		}

		if (newfile)
		{
			char	   *dirpath = GetDatabasePath(w->rlocator.dbOid,
												  w->rlocator.spcOid);

			fsync_fname(dirpath, true);
			pfree(dirpath);
		}

		for (int i = 0; i < w->nsegs; i++)
		{
			AppenderWSeg *seg = &w->segs[i];
			AppenderSegment row;

			if (!seg->is_new && !seg->sealed_here &&
				seg->rows == seg->start_rows)
				continue;

			row.segno = seg->segno;
			row.state = seg->sealed ? APPENDER_SEG_SEALED : APPENDER_SEG_OPEN;
			row.eof = seg->eof;
			row.rowcount = seg->rows;
			row.valdir_off = seg->valdir_off;
			appender_meta_store(&w->rlocator, &row);
		}
	}
}

void
appender_writer_end(bool isCommit)
{
	HASH_SEQ_STATUS status;
	AppenderWriter *w;

	if (appender_writers == NULL)
		return;

	hash_seq_init(&status, appender_writers);
	while ((w = hash_seq_search(&status)) != NULL)
	{
		if (!isCommit)
			appender_unlink_new_segs(w, 0);
		appender_close_file(w);
	}
	appender_writers = NULL;
}

static void
appender_writer_restore(AppenderWriter *w, const AppenderWMark *m)
{
	w->buflen = 0;
	w->bufrows = 0;

	appender_unlink_new_segs(w, m->nsegs);
	w->nsegs = m->nsegs;
	if (w->file_segno != 0 && !appender_writer_has_seg(w, w->file_segno))
		appender_close_file(w);

	if (w->nsegs > 0)
	{
		AppenderWSeg *seg = appender_cur_seg(w);

		seg->eof = m->eof;
		seg->rows = m->rows;
		seg->valdir_off = m->valdir_off;
		seg->npending = 0;
		seg->sealed = m->sealed;
		seg->sealed_here = m->sealed_here;
		seg->nmarks = m->nmarks;
		if (m->nmarks > 0)
			seg->marks[m->nmarks - 1] = m->lastmark;
	}
}

void
appender_writer_subxact_end(bool isCommit)
{
	int			level = GetCurrentTransactionNestLevel();
	HASH_SEQ_STATUS status;
	AppenderWriter *w;
	List	   *dead = NIL;
	ListCell   *lc;

	if (appender_writers == NULL)
		return;

	hash_seq_init(&status, appender_writers);
	while ((w = hash_seq_search(&status)) != NULL)
	{
		if (isCommit)
		{
			if (w->create_level == level)
				w->create_level = level - 1;
			if (w->nstack > 0 && w->stack[w->nstack - 1].level == level)
			{
				int			below = w->nstack > 1 ? w->stack[w->nstack - 2].level
					: w->create_level;

				if (below >= level - 1)
					w->nstack--;
				else
					w->stack[w->nstack - 1].level = level - 1;
			}
			continue;
		}

		if (w->create_level >= level)
		{
			appender_unlink_new_segs(w, 0);
			appender_close_file(w);
			dead = lappend(dead, &w->rlocator);
			continue;
		}

		{
			AppenderWMark *restore = NULL;

			while (w->nstack > 0 && w->stack[w->nstack - 1].level >= level)
				restore = &w->stack[--w->nstack];
			if (restore)
				appender_writer_restore(w, restore);
		}
	}

	foreach(lc, dead)
		hash_search(appender_writers, lfirst(lc), HASH_REMOVE, NULL);
	list_free(dead);
}

/*
 * MemTuple bindings, one per (relation, natts): blocks written before an
 * ALTER TABLE ADD COLUMN keep the narrower layout.
 */
typedef struct AppenderBindKey
{
	Oid			relid;
	int			natts;
} AppenderBindKey;

typedef struct AppenderBindEntry
{
	AppenderBindKey key;
	MemTupleBinding *bind;
} AppenderBindEntry;

static HTAB *appender_bindings = NULL;

MemTupleBinding *
appender_binding(Relation rel, int natts)
{
	AppenderBindKey key;
	AppenderBindEntry *entry;
	bool		found;

	if (appender_bindings == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(AppenderBindKey);
		ctl.entrysize = sizeof(AppenderBindEntry);
		ctl.hcxt = CacheMemoryContext;
		appender_bindings = hash_create("appender bindings", 64, &ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	memset(&key, 0, sizeof(key));
	key.relid = RelationGetRelid(rel);
	key.natts = natts;
	entry = hash_search(appender_bindings, &key, HASH_ENTER, &found);
	if (!found)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(CacheMemoryContext);

		entry->bind = NULL;
		PG_TRY();
		{
			entry->bind = create_memtuple_binding(
												  CreateTupleDescTruncatedCopy(RelationGetDescr(rel), natts));
		}
		PG_FINALLY();
		{
			MemoryContextSwitchTo(oldctx);
			if (entry->bind == NULL)
				hash_search(appender_bindings, &key, HASH_REMOVE, NULL);
		}
		PG_END_TRY();
	}
	return entry->bind;
}

/* Relcache invalidation: drop bindings of relid, or all of them. */
void
appender_binding_forget(Oid relid)
{
	HASH_SEQ_STATUS status;
	AppenderBindEntry *entry;

	if (appender_bindings == NULL)
		return;

	hash_seq_init(&status, appender_bindings);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (OidIsValid(relid) && entry->key.relid != relid)
			continue;
		FreeTupleDesc(entry->bind->tupdesc);
		destroy_memtuple_binding(entry->bind);
		hash_search(appender_bindings, &entry->key, HASH_REMOVE, NULL);
	}
}

/* Deform a MemTuple formed with natts attributes into a slot of desc. */
void
appender_deform(MemTuple mtup, MemTupleBinding *bind, int natts,
				TupleDesc desc, TupleTableSlot *slot)
{
	memtuple_deform(mtup, bind, slot->tts_values, slot->tts_isnull);
	for (int i = natts; i < desc->natts; i++)
		slot->tts_values[i] = getmissingattr(desc, i + 1, &slot->tts_isnull[i]);
}
