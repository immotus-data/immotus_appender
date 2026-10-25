/*-------------------------------------------------------------------------
 *
 * appender.c
 *    The appender table access method.
 *
 * Rows are only ever appended.  They live in segment files outside the
 * buffer manager (appender_datafile.c), their visibility is defined by the
 * segment manifest (appender_metadata.c) together with this backend's own
 * uncommitted appends (appender_writer.c), and every change to segment files
 * is WAL-logged by a custom resource manager (appender_xlog.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/predicate.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "appender_datafile.h"
#include "appender_lifecycle.h"
#include "appender_metadata.h"
#include "appender_tid.h"
#include "appender_writer.h"
#include "appender_xlog.h"

PG_MODULE_MAGIC;

static const RmgrData appender_rmgr = {
	.rm_name = "appender",
	.rm_redo = appender_redo,
	.rm_desc = appender_desc,
	.rm_identify = appender_identify,
};

static object_access_hook_type prev_object_access_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static int	appender_valdir_cache_mb = 64;

/*
 * Virtual slots that deform a stored row lazily, on the first access to its
 * attributes.  Heap-tuple copies keep the TID: ANALYZE sorts its sample by
 * t_self.  Filled in from TTSOpsVirtual at load time.
 */
static TupleTableSlotOps appender_slot_ops;

typedef struct AppenderTupleTableSlot
{
	VirtualTupleTableSlot base;
	MemTuple	mtup;			/* row not deformed yet, NULL if virtual */
	MemTupleBinding *bind;
	int			mtup_natts;		/* attributes stored in mtup */
} AppenderTupleTableSlot;

static void
appender_slot_clear(TupleTableSlot *slot)
{
	((AppenderTupleTableSlot *) slot)->mtup = NULL;
	TTSOpsVirtual.clear(slot);
}

/* Missing trailing attributes are filled in by slot_getsomeattrs_int(). */
static void
appender_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	AppenderTupleTableSlot *aslot = (AppenderTupleTableSlot *) slot;
	int			upto = Min(natts, aslot->mtup_natts);

	Assert(aslot->mtup != NULL);
	if (slot->tts_nvalid < upto)
	{
		memtuple_deform_range(aslot->mtup, aslot->bind, slot->tts_nvalid, upto,
							  slot->tts_values, slot->tts_isnull);
		slot->tts_nvalid = upto;
	}
}

static void
appender_slot_materialize(TupleTableSlot *slot)
{
	AppenderTupleTableSlot *aslot = (AppenderTupleTableSlot *) slot;

	if (aslot->mtup)
	{
		slot_getallattrs(slot);
		aslot->mtup = NULL;
	}
	TTSOpsVirtual.materialize(slot);
}

static void
appender_slot_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	((AppenderTupleTableSlot *) dstslot)->mtup = NULL;
	TTSOpsVirtual.copyslot(dstslot, srcslot);
}

static HeapTuple
appender_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTuple	tup;

	slot_getallattrs(slot);
	tup = TTSOpsVirtual.copy_heap_tuple(slot);
	tup->t_self = slot->tts_tid;
	tup->t_tableOid = slot->tts_tableOid;
	return tup;
}

static MinimalTuple
appender_slot_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	slot_getallattrs(slot);
	return TTSOpsVirtual.copy_minimal_tuple(slot, extra);
}

/* Store a row to be deformed on demand; mtup and bind must outlive it. */
static void
appender_slot_store(TupleTableSlot *slot, MemTuple mtup, MemTupleBinding *bind,
					int natts)
{
	AppenderTupleTableSlot *aslot = (AppenderTupleTableSlot *) slot;

	ExecClearTuple(slot);
	aslot->mtup = mtup;
	aslot->bind = bind;
	aslot->mtup_natts = natts;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
}

/* ----------------------------------------------------------------------------
 * Scans
 * ------------------------------------------------------------------------- */

typedef struct AppenderParallelScanData
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 cursor;	/* next unclaimed block, see APPENDER_CURSOR */
	int32		nown;			/* leader's own appends */
	AppenderScanSeg own[FLEXIBLE_ARRAY_MEMBER];
} AppenderParallelScanData;

/* Block position shared by parallel workers: segment index, range, offset. */
#define APPENDER_CURSOR(seg, range, off) \
	(((uint64) (seg) << 48) | ((uint64) (range) << 47) | (uint64) (off))
#define APPENDER_CURSOR_SEG(c)		((int) ((c) >> 48))
#define APPENDER_CURSOR_RANGE(c)	((int) (((c) >> 47) & 1))
#define APPENDER_CURSOR_OFF(c)		((int64) ((c) & ((UINT64CONST(1) << 47) - 1)))

typedef struct AppenderScanDescData
{
	TableScanDescData rs_base;
	MemoryContext mcxt;
	AppenderScanSeg *segs;
	int			nsegs;
	int			cur;			/* segment index, -1 before the first */
	int			range;			/* 0: committed rows, 1: own rows */
	int64		off;			/* next block to read */
	int64		range_start;
	int64		range_end;
	int64		vbase;			/* position of range_start across all ranges */
	File		file;
	int			file_segno;
	AppenderBlockHeader hdr;
	char	   *block;
	Size		blockcap;
	bool		loaded;
	uint32		row;			/* next row of the loaded block */
	uint32		rowoff;
	MemTupleBinding *bind;		/* layout of bind_natts attributes of
								 * bind_desc */
	TupleDesc	bind_desc;
	int			bind_natts;
	int64		an_hi;			/* ANALYZE: end of the sampled range */
} AppenderScanDescData;

typedef AppenderScanDescData *AppenderScanDesc;

static inline CommandId
appender_snapshot_cid(Snapshot snapshot)
{
	return IsMVCCSnapshot(snapshot) ? snapshot->curcid : InvalidCommandId;
}

static inline Snapshot
appender_meta_snapshot(Snapshot snapshot)
{
	return IsMVCCSnapshot(snapshot) ? snapshot : GetLatestSnapshot();
}

static inline CommandId
appender_active_cid(void)
{
	return ActiveSnapshotSet() ? GetActiveSnapshot()->curcid : InvalidCommandId;
}

static void
appender_scan_init_segs(AppenderScanDesc scan)
{
	Relation	rel = scan->rs_base.rs_rd;
	Snapshot	snapshot = scan->rs_base.rs_snapshot;
	AppenderScanSeg *own;
	int			nown;
	MemoryContext oldctx = MemoryContextSwitchTo(scan->mcxt);

	if (scan->rs_base.rs_parallel)
	{
		AppenderParallelScanData *p = (AppenderParallelScanData *) scan->rs_base.rs_parallel;

		own = p->own;
		nown = p->nown;
	}
	else
	{
		appender_writer_flush(&rel->rd_locator);
		nown = appender_own_segments(&rel->rd_locator,
									 appender_snapshot_cid(snapshot), &own);
	}

	scan->nsegs = appender_visible_segments(&rel->rd_locator,
											appender_meta_snapshot(snapshot),
											own, nown, &scan->segs);
	scan->cur = -1;
	scan->range = 1;
	scan->off = scan->range_start = scan->range_end = scan->vbase = 0;
	scan->loaded = false;
	MemoryContextSwitchTo(oldctx);
}

static File
appender_scan_file(AppenderScanDesc scan, int segno)
{
	if (scan->file < 0 || scan->file_segno != segno)
	{
		if (scan->file >= 0)
			FileClose(scan->file);
		scan->file = appender_seg_open(&scan->rs_base.rs_rd->rd_locator, segno);
		scan->file_segno = segno;
	}
	return scan->file;
}

/* Move to the next non-empty byte range; false at the end of the scan. */
static bool
appender_scan_next_range(AppenderScanDesc scan)
{
	scan->vbase += scan->range_end - scan->range_start;
	scan->off = scan->range_start = scan->range_end = 0;
	scan->loaded = false;

	for (;;)
	{
		AppenderScanSeg *s;

		if (scan->cur >= 0 && scan->cur < scan->nsegs && scan->range == 0)
		{
			s = &scan->segs[scan->cur];
			scan->range = 1;
			if (s->own_eof > s->own_off)
			{
				scan->off = scan->range_start = s->own_off;
				scan->range_end = s->own_eof;
				return true;
			}
		}

		scan->cur++;
		if (scan->cur >= scan->nsegs)
		{
			scan->cur = scan->nsegs;
			return false;
		}

		scan->range = 0;
		s = &scan->segs[scan->cur];
		if (s->eof > APPENDER_SEG_DATA_START)
		{
			scan->off = scan->range_start = APPENDER_SEG_DATA_START;
			scan->range_end = s->eof;
			return true;
		}
	}
}

static void
appender_scan_read_payload(AppenderScanDesc scan, File file, int64 off)
{
	if (scan->hdr.datalen > scan->blockcap)
	{
		if (scan->block)
			pfree(scan->block);
		scan->block = MemoryContextAlloc(scan->mcxt, scan->hdr.datalen);
		scan->blockcap = scan->hdr.datalen;
	}
	appender_seg_read_payload(file, off, &scan->hdr, scan->block);
	scan->loaded = true;
	scan->row = 0;
	scan->rowoff = 0;
}

static void
appender_check_block(AppenderScanDesc scan, File file, int64 off, int64 end)
{
	if (off + appender_block_size(&scan->hdr) > end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block at offset %lld of file \"%s\" extends past the segment end",
						(long long) off, FilePathName(file))));
}

/* Read the block at scan->off (header only unless payload) and step past it. */
static void
appender_scan_load(AppenderScanDesc scan, bool payload)
{
	AppenderScanSeg *s = &scan->segs[scan->cur];
	File		file = appender_scan_file(scan, s->segno);

	for (;;)
	{
		appender_seg_read_header(file, scan->off, &scan->hdr);
		appender_check_block(scan, file, scan->off, scan->range_end);
		if (scan->hdr.kind == APPENDER_BLOCK_KIND_DATA)
			break;
		scan->off += appender_block_size(&scan->hdr);
		if (scan->off >= scan->range_end)
			return;			/* only value blocks remain */
	}
	if (payload)
		appender_scan_read_payload(scan, file, scan->off);
	scan->off += appender_block_size(&scan->hdr);
}

/* Byte range of (seg, range); false if it is empty. */
static bool
appender_range_bounds(AppenderScanDesc scan, int seg, int range,
					  int64 *lo, int64 *hi)
{
	AppenderScanSeg *s;

	if (seg >= scan->nsegs)
		return false;
	s = &scan->segs[seg];
	*lo = range == 0 ? APPENDER_SEG_DATA_START : s->own_off;
	*hi = range == 0 ? s->eof : s->own_eof;
	return *hi > *lo;
}

/*
 * Claim the next block of a parallel scan.  Workers advance the shared
 * cursor past a block with compare-and-swap once they have read its header;
 * the payload is read and decoded outside of any coordination.
 */
static bool
appender_parallel_claim(AppenderScanDesc scan)
{
	AppenderParallelScanData *p = (AppenderParallelScanData *) scan->rs_base.rs_parallel;
	uint64		cur = pg_atomic_read_u64(&p->cursor);

	for (;;)
	{
		int			seg = APPENDER_CURSOR_SEG(cur);
		int			range = APPENDER_CURSOR_RANGE(cur);
		int64		off = APPENDER_CURSOR_OFF(cur);
		int64		lo;
		int64		hi;
		File		file;

		if (!appender_range_bounds(scan, seg, range, &lo, &hi) || off >= hi)
		{
			/* step to the start of the next non-empty range */
			do
			{
				if (seg >= scan->nsegs)
					return false;
				if (range == 0)
					range = 1;
				else
				{
					seg++;
					range = 0;
				}
			} while (!appender_range_bounds(scan, seg, range, &lo, &hi));

			pg_atomic_compare_exchange_u64(&p->cursor, &cur,
										   APPENDER_CURSOR(seg, range, lo));
			cur = pg_atomic_read_u64(&p->cursor);
			continue;
		}

		if (off < lo)
		{
			pg_atomic_compare_exchange_u64(&p->cursor, &cur,
										   APPENDER_CURSOR(seg, range, lo));
			cur = pg_atomic_read_u64(&p->cursor);
			continue;
		}

		file = appender_scan_file(scan, scan->segs[seg].segno);
		appender_seg_read_header(file, off, &scan->hdr);
		appender_check_block(scan, file, off, hi);

		if (pg_atomic_compare_exchange_u64(&p->cursor, &cur,
										   APPENDER_CURSOR(seg, range,
														   off + appender_block_size(&scan->hdr))))
		{
			scan->cur = seg;
			appender_scan_read_payload(scan, file, off);
			return true;
		}
		/* another participant took it; cur now holds the new position */
	}
}

static MemTuple
appender_block_row(const AppenderBlockHeader *hdr, char *block, uint32 rowoff)
{
	MemTuple	mtup = (MemTuple) (block + rowoff);

	if (rowoff + sizeof(uint32) > hdr->datalen ||
		!memtuple_lead_bit_set(mtup) ||
		rowoff + memtuple_get_size(mtup) > hdr->datalen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid row at offset %u of an appender block", rowoff)));
	return mtup;
}

static void
appender_free_binding(MemTupleBinding *bind)
{
	FreeTupleDesc(bind->tupdesc);
	destroy_memtuple_binding(bind);
}

/*
 * Layout of rows formed with natts attributes, as seen through desc.  The
 * descriptor is the slot's, not the relation's: a table rewrite reads the
 * old rows through the pre-ALTER descriptor.
 */
static MemTupleBinding *
appender_make_binding(TupleDesc desc, int natts)
{
	if (natts > desc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("appender block has %d attributes, the table only %d",
						natts, desc->natts)));
	return create_memtuple_binding(CreateTupleDescTruncatedCopy(desc, natts));
}

static void
appender_scan_emit(AppenderScanDesc scan, TupleTableSlot *slot)
{
	Relation	rel = scan->rs_base.rs_rd;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	MemTuple	mtup = appender_block_row(&scan->hdr, scan->block, scan->rowoff);

	if (scan->bind == NULL || scan->bind_desc != desc ||
		scan->bind_natts != scan->hdr.natts)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(scan->mcxt);

		if (scan->bind)
			appender_free_binding(scan->bind);
		scan->bind = NULL;
		scan->bind = appender_make_binding(desc, scan->hdr.natts);
		scan->bind_desc = desc;
		scan->bind_natts = scan->hdr.natts;
		MemoryContextSwitchTo(oldctx);
	}

	if (slot->tts_ops == &appender_slot_ops)
		appender_slot_store(slot, mtup, scan->bind, scan->hdr.natts);
	else
	{
		ExecClearTuple(slot);
		appender_deform(mtup, scan->bind, scan->hdr.natts, desc, slot);
		ExecStoreVirtualTuple(slot);
	}
	appender_tid_set(&slot->tts_tid, scan->segs[scan->cur].segno,
					 scan->hdr.first_rownum + scan->row);
	slot->tts_tableOid = RelationGetRelid(rel);

	scan->rowoff += memtuple_get_size(mtup);
	scan->row++;
}

static TableScanDesc
appender_beginscan(Relation rel, Snapshot snapshot, int nkeys, ScanKey key,
				   ParallelTableScanDesc pscan, uint32 flags)
{
	AppenderScanDesc scan;

	if (nkeys > 0)
		elog(ERROR, "appender: scan keys are not supported");

	RelationIncrementReferenceCount(rel);

	if (snapshot == NULL)
	{
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		flags |= SO_TEMP_SNAPSHOT;
	}

	scan = palloc0(sizeof(AppenderScanDescData));
	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;
	scan->mcxt = CurrentMemoryContext;
	scan->file = -1;

	if (flags & SO_TYPE_SEQSCAN)
		PredicateLockRelation(rel, snapshot);

	appender_scan_init_segs(scan);
	return (TableScanDesc) scan;
}

static void
appender_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	AppenderScanDesc scan = (AppenderScanDesc) sscan;

	if (scan->segs)
		pfree(scan->segs);
	appender_scan_init_segs(scan);
}

static void
appender_endscan(TableScanDesc sscan)
{
	AppenderScanDesc scan = (AppenderScanDesc) sscan;

	if (scan->file >= 0)
		FileClose(scan->file);
	if (scan->block)
		pfree(scan->block);
	if (scan->bind)
		appender_free_binding(scan->bind);
	if (scan->segs)
		pfree(scan->segs);
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);
	RelationDecrementReferenceCount(scan->rs_base.rs_rd);
	pfree(scan);
}

static bool
appender_getnextslot(TableScanDesc sscan, ScanDirection direction,
					 TupleTableSlot *slot)
{
	AppenderScanDesc scan = (AppenderScanDesc) sscan;

	if (ScanDirectionIsBackward(direction))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("backward scans of appender tables are not supported")));

	for (;;)
	{
		if (scan->loaded && scan->row < scan->hdr.rowcount)
		{
			appender_scan_emit(scan, slot);
			return true;
		}
		scan->loaded = false;

		if (scan->rs_base.rs_parallel)
		{
			if (!appender_parallel_claim(scan))
			{
				ExecClearTuple(slot);
				return false;
			}
		}
		else if (scan->off < scan->range_end)
			appender_scan_load(scan, true);
		else if (!appender_scan_next_range(scan))
		{
			ExecClearTuple(slot);
			return false;
		}
	}
}

/*
 * ANALYZE samples "blocks" of BLCKSZ bytes of the concatenated data ranges,
 * as sized by appender_relation_size().  A sampled range yields the rows of
 * the blocks that start inside it, so every block is counted exactly once.
 * Sample positions arrive in increasing order, so one forward pass over the
 * block headers serves the whole ANALYZE.
 */
static bool
appender_scan_analyze_next_block(TableScanDesc sscan, ReadStream *stream)
{
	AppenderScanDesc scan = (AppenderScanDesc) sscan;
	BufferAccessStrategy strategy;
	BlockNumber blkno = read_stream_next_block(stream, &strategy);
	int64		lo;

	if (blkno == InvalidBlockNumber)
		return false;

	lo = (int64) blkno * BLCKSZ;
	scan->an_hi = lo + BLCKSZ;
	scan->loaded = false;

	for (;;)
	{
		if (scan->off >= scan->range_end)
		{
			if (!appender_scan_next_range(scan))
				break;
			continue;
		}
		if (scan->vbase + (scan->off - scan->range_start) >= lo)
			break;
		appender_scan_load(scan, false);
	}
	return true;
}

static bool
appender_scan_analyze_next_tuple(TableScanDesc sscan, TransactionId OldestXmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	AppenderScanDesc scan = (AppenderScanDesc) sscan;

	for (;;)
	{
		if (scan->loaded && scan->row < scan->hdr.rowcount)
		{
			appender_scan_emit(scan, slot);
			*liverows += 1;
			return true;
		}
		scan->loaded = false;

		if (scan->off >= scan->range_end)
		{
			if (!appender_scan_next_range(scan))
				return false;
			continue;
		}
		if (scan->vbase + (scan->off - scan->range_start) >= scan->an_hi)
			return false;
		appender_scan_load(scan, true);
	}
}

/* ----------------------------------------------------------------------------
 * Parallel scans hand out blocks through a shared cursor.
 * ------------------------------------------------------------------------- */

static Size
appender_parallelscan_size(int nown)
{
	return MAXALIGN(add_size(offsetof(AppenderParallelScanData, own),
							 mul_size(nown, sizeof(AppenderScanSeg))));
}

static Size
appender_parallelscan_estimate(Relation rel)
{
	AppenderScanSeg *own;
	int			nown;

	/* workers read the files, so the leader's buffered rows go first */
	appender_writer_flush(&rel->rd_locator);
	nown = appender_own_segments(&rel->rd_locator, appender_active_cid(), &own);
	return appender_parallelscan_size(nown);
}

static Size
appender_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	AppenderParallelScanData *p = (AppenderParallelScanData *) pscan;
	AppenderScanSeg *own;
	int			nown = appender_own_segments(&rel->rd_locator,
											 appender_active_cid(), &own);

	p->base.phs_locator = rel->rd_locator;
	p->base.phs_syncscan = false;
	pg_atomic_init_u64(&p->cursor, 0);
	p->nown = nown;
	if (nown > 0)
		memcpy(p->own, own, nown * sizeof(AppenderScanSeg));
	return appender_parallelscan_size(nown);
}

static void
appender_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	pg_atomic_write_u64(&((AppenderParallelScanData *) pscan)->cursor, 0);
}

/* ----------------------------------------------------------------------------
 * Row fetch by TID
 * ------------------------------------------------------------------------- */

/* Last block located by TID, to make sequential TID lookups cheap. */
static struct
{
	RelFileLocator rlocator;
	int			segno;
	int64		range_start;
	int64		blockoff;
	uint32		first;
	bool		valid;
}			appender_fetch_cache;

/* Find which visible byte range holds rownum of segno; false if none. */
static bool
appender_locate_row(Relation rel, Snapshot snapshot, int segno, uint32 rownum,
					int64 *lo, int64 *hi)
{
	AppenderScanSeg *own;
	AppenderScanSeg *segs;
	int			nown;
	int			nsegs;
	bool		found = false;

	appender_writer_flush(&rel->rd_locator);
	nown = appender_own_segments(&rel->rd_locator,
								 appender_snapshot_cid(snapshot), &own);
	nsegs = appender_visible_segments(&rel->rd_locator,
									  appender_meta_snapshot(snapshot),
									  own, nown, &segs);

	for (int i = 0; i < nsegs && !found; i++)
	{
		AppenderScanSeg *s = &segs[i];

		if (s->segno != segno)
			continue;
		if (rownum < s->rows)
		{
			*lo = APPENDER_SEG_DATA_START;
			*hi = s->eof;
			found = true;
		}
		else if (rownum >= s->own_first && rownum - s->own_first < s->own_rows)
		{
			*lo = s->own_off;
			*hi = s->own_eof;
			found = true;
		}
	}

	pfree(segs);
	if (own)
		pfree(own);
	return found;
}

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

static void
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
	while (doff > 0)
	{
		int64		cur = doff;
		File		file = appender_seg_open(rlocator, segno);
		AppenderBlockHeader hdr;
		char	   *payload;
		int			n;
		int64		expected;

		appender_seg_read_header(file, doff, &hdr);
		if (hdr.kind != APPENDER_BLOCK_KIND_VALDIR)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid value directory at offset %lld of segment %d of relation %u/%u/%u",
							(long long) doff, segno,
							rlocator->spcOid, rlocator->dbOid,
							rlocator->relNumber)));
		payload = palloc(hdr.datalen);
		appender_seg_read_payload(file, doff, &hdr, payload);
		FileClose(file);

		n = (int) hdr.rowcount;
		expected = sizeof(int64) + (int64) n * sizeof(AppenderValDirEntry);
		if (expected != hdr.datalen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid value directory payload at offset %lld of segment %d of relation %u/%u/%u",
							(long long) doff, segno,
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
 * directory published yet.
 */
static bool
appender_find_toast_value_in_segment(const RelFileLocator *rlocator, int segno,
									 int64 valdir_off, int64 scan_lo,
									 int64 scan_hi, Oid valueid, int64 *off)
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
			FileClose(file);
			return true;
		}
		p += appender_block_size(&hdr);
	}
	FileClose(file);
	return false;
}

static void
appender_read_toast_slice_checked(File file, int64 off,
							  const AppenderBlockHeader *hdr,
							  int32 sliceoffset, int32 slicelength,
							  char *out)
{
	AppenderBlockHeader hdr0 = *hdr;
	pg_crc32c	crc;
	int64		pos = 0;
	char		buf[8192];

	hdr0.crc = 0;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((const char *) &hdr0) + sizeof(pg_crc32c),
					sizeof(AppenderBlockHeader) - sizeof(pg_crc32c));

	while (pos < hdr->datalen)
	{
		int64		left = hdr->datalen - pos;
		int64		want = Min((int64) sizeof(buf), left);
		int64		chunk_lo = pos;
		int64		chunk_hi = pos + want;
		int64		copy_lo = Max(chunk_lo, (int64) sliceoffset);
		int64		copy_hi = Min(chunk_hi, (int64) sliceoffset + slicelength);

		if (FileRead(file, buf, want,
					 off + sizeof(AppenderBlockHeader) + pos,
					 WAIT_EVENT_DATA_FILE_READ) != want)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read external value from segment block at offset %lld: %m",
							(long long) off)));

		COMP_CRC32C(crc, buf, want);

		if (copy_hi > copy_lo)
			memcpy(out + (copy_lo - sliceoffset),
				   buf + (copy_lo - chunk_lo),
				   copy_hi - copy_lo);

		pos += want;
	}

	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, hdr->crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block checksum mismatch at offset %lld", (long long) off)));
}

/*
 * Fetch a slice of an external value stored in value blocks.  The stored
 * bytes are copied verbatim, so a compressed value is returned in its stored
 * form and decompressed by the caller.  valueid is a global OID; it is found
 * through the segment's directory, with a scan fallback for values that are
 * still uncommitted.
 */
static void
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
	File		file;

	if (slicelength <= 0)
		return;

	nmeta = appender_meta_segments(&rlocator, GetLatestSnapshot(), &meta);
	/* a row can be detoasted by the command that wrote it (RETURNING, triggers) */
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
												 valueid, &off))
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
												 valueid, &off))
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

	appender_read_toast_slice_checked(file, off, &hdr,
								 sliceoffset, slicelength,
								 VARDATA(result));
	FileClose(file);
}

static bool
appender_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	int			segno;
	uint32		rownum;
	int64		lo;
	int64		hi;
	int64		off;
	File		file;
	AppenderBlockHeader hdr;
	bool		found = false;
	char	   *payload;
	uint32		rowoff = 0;
	MemTuple	mtup;

	if (!appender_tid_is_valid(tid))
		return false;
	segno = appender_tid_segno(tid);
	rownum = appender_tid_rownum(tid);

	if (!appender_locate_row(rel, snapshot, segno, rownum, &lo, &hi))
		return false;

	off = lo;
	if (appender_fetch_cache.valid &&
		RelFileLocatorEquals(appender_fetch_cache.rlocator, rel->rd_locator) &&
		appender_fetch_cache.segno == segno &&
		appender_fetch_cache.range_start == lo &&
		appender_fetch_cache.first <= rownum &&
		appender_fetch_cache.blockoff < hi)
		off = appender_fetch_cache.blockoff;

	file = appender_seg_open(&rel->rd_locator, segno);
	while (off < hi)
	{
		appender_seg_read_header(file, off, &hdr);
		if (off + appender_block_size(&hdr) > hi)
			break;
		if (hdr.kind == APPENDER_BLOCK_KIND_DATA &&
			rownum >= hdr.first_rownum && rownum - hdr.first_rownum < hdr.rowcount)
		{
			found = true;
			break;
		}
		off += appender_block_size(&hdr);
	}

	if (!found)
	{
		FileClose(file);
		return false;
	}

	appender_fetch_cache.rlocator = rel->rd_locator;
	appender_fetch_cache.segno = segno;
	appender_fetch_cache.range_start = lo;
	appender_fetch_cache.blockoff = off;
	appender_fetch_cache.first = hdr.first_rownum;
	appender_fetch_cache.valid = true;

	payload = palloc(Max(hdr.datalen, 1));
	appender_seg_read_payload(file, off, &hdr, payload);
	FileClose(file);

	for (uint32 i = hdr.first_rownum; i < rownum; i++)
		rowoff += memtuple_get_size(appender_block_row(&hdr, payload, rowoff));
	mtup = appender_block_row(&hdr, payload, rowoff);

	ExecClearTuple(slot);
	if (slot->tts_tupleDescriptor == RelationGetDescr(rel))
		appender_deform(mtup, appender_binding(rel, hdr.natts), hdr.natts,
						slot->tts_tupleDescriptor, slot);
	else
	{
		MemTupleBinding *bind = appender_make_binding(slot->tts_tupleDescriptor,
													  hdr.natts);

		appender_deform(mtup, bind, hdr.natts, slot->tts_tupleDescriptor, slot);
		appender_free_binding(bind);
	}
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
	pfree(payload);
	return true;
}

static bool
appender_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return appender_tid_is_valid(tid);
}

static void
appender_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	/* rows are never updated: a TID is always the latest version */
}

static bool
appender_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	int64		lo;
	int64		hi;

	return appender_tid_is_valid(&slot->tts_tid) &&
		appender_locate_row(rel, snapshot, appender_tid_segno(&slot->tts_tid),
							appender_tid_rownum(&slot->tts_tid), &lo, &hi);
}

/* ----------------------------------------------------------------------------
 * Modifications
 * ------------------------------------------------------------------------- */

static void
appender_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					  int options, struct BulkInsertStateData *bistate)
{
	appender_writer_insert(rel, slot, cid);
}

static void
appender_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
					  CommandId cid, int options,
					  struct BulkInsertStateData *bistate)
{
	for (int i = 0; i < nslots; i++)
		appender_writer_insert(rel, slots[i], cid);
}

static void
appender_finish_bulk_insert(Relation rel, int options)
{
}

static void
appender_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								  CommandId cid, int options,
								  struct BulkInsertStateData *bistate,
								  uint32 specToken)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("INSERT ... ON CONFLICT is not supported for appender tables")));
}

static void
appender_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
									uint32 specToken, bool succeeded)
{
	elog(ERROR, "appender: unexpected speculative insertion");
}

static TM_Result
appender_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					  Snapshot snapshot, Snapshot crosscheck, bool wait,
					  TM_FailureData *tmfd, bool changingPart)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("DELETE is not supported for appender tables")));
	return TM_Invisible;
}

static TM_Result
appender_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					  bool wait, TM_FailureData *tmfd,
					  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("UPDATE is not supported for appender tables")));
	return TM_Invisible;
}

/*
 * Rows can never be updated or deleted, so there is nothing to wait for or
 * conflict with: report the row as locked if it is visible.
 */
static TM_Result
appender_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
					TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					LockWaitPolicy wait_policy, uint8 flags,
					TM_FailureData *tmfd)
{
	if (!appender_fetch_row_version(rel, tid, snapshot, slot))
	{
		tmfd->ctid = *tid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
		return TM_Invisible;
	}
	return TM_Ok;
}

/* ----------------------------------------------------------------------------
 * DDL
 * ------------------------------------------------------------------------- */

/*
 * New storage for a relation: CREATE, TRUNCATE or a table rewrite.  The
 * previous storage, if any, goes away at commit; the new one at abort.
 * Segment files carry no transaction ids, so there is nothing to freeze.
 */
static void
appender_relation_set_new_filelocator(Relation rel,
									  const RelFileLocator *newrlocator,
									  char persistence,
									  TransactionId *freezeXid,
									  MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (persistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("appender tables must be permanent"),
				 errdetail("Unlogged and temporary appender tables are not supported.")));

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	srel = RelationCreateStorage(*newrlocator, persistence, true);
	smgrclose(srel);

	if (!RelFileLocatorEquals(*newrlocator, rel->rd_locator))
	{
		appender_storage_drop_at_commit(&rel->rd_locator);
		appender_meta_delete(&rel->rd_locator);
	}
	appender_storage_drop_at_abort(newrlocator);
}

/*
 * TRUNCATE of storage created in the current (sub)transaction: a rollback
 * removes the storage anyway, so it is emptied in place.
 */
static void
appender_relation_nontransactional_truncate(Relation rel)
{
	appender_writer_discard(&rel->rd_locator);
	if (appender_seg_unlink_all(&rel->rd_locator) > 0)
		appender_xlog_unlink(&rel->rd_locator, 0);
	appender_meta_delete(&rel->rd_locator);
	RelationTruncate(rel, 0);
}

static void
appender_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("moving appender tables to another tablespace is not supported")));
}

static void
appender_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								   Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples,
								   double *tups_vacuumed,
								   double *tups_recently_dead)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("CLUSTER and VACUUM FULL are not supported for appender tables")));
}

/* Committed data bytes and rows plus this backend's own appends. */
static void
appender_rel_totals(Relation rel, int64 *bytes, int64 *rows)
{
	AppenderScanSeg *own;
	AppenderScanSeg *segs;
	int			nown = appender_own_segments(&rel->rd_locator, InvalidCommandId, &own);
	int			nsegs = appender_visible_segments(&rel->rd_locator,
												  GetLatestSnapshot(),
												  own, nown, &segs);

	*bytes = 0;
	*rows = 0;
	for (int i = 0; i < nsegs; i++)
	{
		*bytes += Max(segs[i].eof - APPENDER_SEG_DATA_START, 0) +
			(segs[i].own_eof - segs[i].own_off);
		*rows += (int64) segs[i].rows + segs[i].own_rows;
	}
	pfree(segs);
	if (own)
		pfree(own);
}

static void
appender_vacuum_rel(Relation rel, VacuumParams *params,
					BufferAccessStrategy bstrategy)
{
	TimestampTz starttime = GetCurrentTimestamp();
	int64		bytes;
	int64		rows;
	bool		frozenxid_updated;
	bool		minmulti_updated;

	/* nothing to prune or freeze: just refresh the statistics */
	appender_rel_totals(rel, &bytes, &rows);
	vac_update_relstats(rel, (BlockNumber) ((bytes + BLCKSZ - 1) / BLCKSZ),
						(double) rows, 0, 0, false,
						InvalidTransactionId, InvalidMultiXactId,
						&frozenxid_updated, &minmulti_updated, false);
	pgstat_report_vacuum(RelationGetRelid(rel), rel->rd_rel->relisshared,
						 rows, 0, starttime);
}

static uint64
appender_relation_size(Relation rel, ForkNumber forknum)
{
	int64		bytes;
	int64		rows;

	if (forknum != MAIN_FORKNUM && forknum != InvalidForkNumber)
		return 0;
	appender_rel_totals(rel, &bytes, &rows);
	return (uint64) bytes;
}

static void
appender_relation_estimate_size(Relation rel, int32 *attr_widths,
								BlockNumber *pages, double *tuples,
								double *allvisfrac)
{
	int64		bytes;
	int64		rows;

	appender_rel_totals(rel, &bytes, &rows);
	*pages = (BlockNumber) ((bytes + BLCKSZ - 1) / BLCKSZ);
	*tuples = (double) rows;
	*allvisfrac = 0;
}

static bool
appender_relation_needs_toast_table(Relation rel)
{
	return false;
}

/* ----------------------------------------------------------------------------
 * Not supported
 * ------------------------------------------------------------------------- */

static void
appender_not_supported(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s is not supported for appender tables", what)));
}

static IndexFetchTableData *
appender_index_fetch_begin(Relation rel)
{
	appender_not_supported("indexing");
	return NULL;
}

static void
appender_index_fetch_reset(IndexFetchTableData *scan)
{
}

static void
appender_index_fetch_end(IndexFetchTableData *scan)
{
}

static bool
appender_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
						   Snapshot snapshot, TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	appender_not_supported("indexing");
	return false;
}

static TransactionId
appender_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	appender_not_supported("indexing");
	return InvalidTransactionId;
}

static double
appender_index_build_range_scan(Relation table_rel, Relation index_rel,
								struct IndexInfo *index_info, bool allow_sync,
								bool anyvisible, bool progress,
								BlockNumber start_blockno,
								BlockNumber numblocks,
								IndexBuildCallback callback,
								void *callback_state, TableScanDesc scan)
{
	appender_not_supported("indexing");
	return 0;
}

static void
appender_index_validate_scan(Relation table_rel, Relation index_rel,
							 struct IndexInfo *index_info, Snapshot snapshot,
							 struct ValidateIndexState *state)
{
	appender_not_supported("indexing");
}

static bool
appender_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
								bool *recheck, uint64 *lossy_pages,
								uint64 *exact_pages)
{
	appender_not_supported("bitmap scanning");
	return false;
}

static bool
appender_scan_sample_next_block(TableScanDesc scan,
								struct SampleScanState *scanstate)
{
	appender_not_supported("TABLESAMPLE");
	return false;
}

static bool
appender_scan_sample_next_tuple(TableScanDesc scan,
								struct SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	appender_not_supported("TABLESAMPLE");
	return false;
}

static const TupleTableSlotOps *
appender_slot_callbacks(Relation rel)
{
	return &appender_slot_ops;
}

static const TableAmRoutine appender_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = appender_slot_callbacks,

	.scan_begin = appender_beginscan,
	.scan_end = appender_endscan,
	.scan_rescan = appender_rescan,
	.scan_getnextslot = appender_getnextslot,

	.parallelscan_estimate = appender_parallelscan_estimate,
	.parallelscan_initialize = appender_parallelscan_initialize,
	.parallelscan_reinitialize = appender_parallelscan_reinitialize,

	.index_fetch_begin = appender_index_fetch_begin,
	.index_fetch_reset = appender_index_fetch_reset,
	.index_fetch_end = appender_index_fetch_end,
	.index_fetch_tuple = appender_index_fetch_tuple,

	.tuple_fetch_row_version = appender_fetch_row_version,
	.tuple_tid_valid = appender_tuple_tid_valid,
	.tuple_get_latest_tid = appender_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = appender_tuple_satisfies_snapshot,
	.index_delete_tuples = appender_index_delete_tuples,

	.tuple_insert = appender_tuple_insert,
	.tuple_insert_speculative = appender_tuple_insert_speculative,
	.tuple_complete_speculative = appender_tuple_complete_speculative,
	.multi_insert = appender_multi_insert,
	.tuple_delete = appender_tuple_delete,
	.tuple_update = appender_tuple_update,
	.tuple_lock = appender_tuple_lock,
	.finish_bulk_insert = appender_finish_bulk_insert,

	.relation_set_new_filelocator = appender_relation_set_new_filelocator,
	.relation_nontransactional_truncate = appender_relation_nontransactional_truncate,
	.relation_copy_data = appender_relation_copy_data,
	.relation_copy_for_cluster = appender_relation_copy_for_cluster,
	.relation_vacuum = appender_vacuum_rel,
	.scan_analyze_next_block = appender_scan_analyze_next_block,
	.scan_analyze_next_tuple = appender_scan_analyze_next_tuple,
	.index_build_range_scan = appender_index_build_range_scan,
	.index_validate_scan = appender_index_validate_scan,

	.relation_size = appender_relation_size,
	.relation_needs_toast_table = appender_relation_needs_toast_table,
	.relation_fetch_toast_slice = appender_fetch_toast_slice,
	.relation_estimate_size = appender_relation_estimate_size,

	.scan_bitmap_next_tuple = appender_scan_bitmap_next_tuple,
	.scan_sample_next_block = appender_scan_sample_next_block,
	.scan_sample_next_tuple = appender_scan_sample_next_tuple,
};

/* ----------------------------------------------------------------------------
 * Transaction and catalog hooks
 * ------------------------------------------------------------------------- */

static void
appender_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			appender_writer_pre_commit();
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (appender_writer_active() || appender_storage_pending())
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has modified appender tables")));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			appender_writer_end(true);
			appender_storage_xact_end(true);
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			appender_writer_end(false);
			appender_storage_xact_end(false);
			break;
		default:
			break;
	}
}

static void
appender_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_COMMIT_SUB || event == SUBXACT_EVENT_ABORT_SUB)
	{
		bool		isCommit = (event == SUBXACT_EVENT_COMMIT_SUB);

		appender_writer_subxact_end(isCommit);
		appender_storage_subxact_end(isCommit);
	}
}

/* Dropping an appender relation removes its segments at commit. */
static void
appender_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					   int subId, void *arg)
{
	HeapTuple	tup;

	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access != OAT_DROP || classId != RelationRelationId || subId != 0)
		return;

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(tup))
		return;

	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if ((form->relkind == RELKIND_RELATION || form->relkind == RELKIND_MATVIEW) &&
			OidIsValid(form->relfilenode) &&
			form->relam == get_table_am_oid("appender", true))
		{
			RelFileLocator rlocator;

			rlocator.spcOid = OidIsValid(form->reltablespace) ?
				form->reltablespace : MyDatabaseTableSpace;
			rlocator.dbOid = MyDatabaseId;
			rlocator.relNumber = form->relfilenode;

			appender_storage_drop_at_commit(&rlocator);
			appender_meta_delete(&rlocator);
		}
	}
	ReleaseSysCache(tup);
}

static void
appender_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						bool readOnlyTree, ProcessUtilityContext context,
						ParamListInfo params, QueryEnvironment *queryEnv,
						DestReceiver *dest, QueryCompletion *qc)
{
	if (IsA(pstmt->utilityStmt, CreatedbStmt))
		appender_check_createdb((CreatedbStmt *) pstmt->utilityStmt);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

static void
appender_relcache_callback(Datum arg, Oid relid)
{
	appender_binding_forget(relid);
	appender_meta_invalidate(relid);
	appender_valdir_cache_reset();
}

void
_PG_init(void)
{
	/* custom WAL resource managers can only be registered at startup */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("immotus_appender must be loaded via shared_preload_libraries"),
				 errhint("Add immotus_appender to shared_preload_libraries and restart the server.")));

	DefineCustomIntVariable("immotus_appender.segment_size",
							"Size at which an appender segment is sealed.",
							NULL,
							&appender_segment_size_mb,
							1024, 1, 1024 * 1024,
							PGC_USERSET, GUC_UNIT_MB,
							NULL, NULL, NULL);
	DefineCustomIntVariable("immotus_appender.prealloc_chunk",
							"Chunk of a segment preallocated (zero-filled) at once.",
							"Zero-filled ahead so commits inside the chunk only fdatasync; 0 disables.",
							&appender_prealloc_chunk_kb,
							1024, 0, 65536,
							PGC_SIGHUP, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomIntVariable("immotus_appender.valdir_cache_size",
							"Per-backend memory budget of cached value directories.",
							"Least-recently-used segment directories are evicted when the cache grows past this size.",
							&appender_valdir_cache_mb,
							64, 1, 1024 * 1024,
							PGC_USERSET, GUC_UNIT_MB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("immotus_appender.debug_log",
							 "Emit a DEBUG1 line for every replayed or skipped segment record.",
							 "For tests: proves which records crash recovery replays.",
							 &appender_debug_log,
							 false,
							 PGC_SIGHUP,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	MarkGUCPrefixReserved("immotus_appender");

	appender_slot_ops = TTSOpsVirtual;
	appender_slot_ops.base_slot_size = sizeof(AppenderTupleTableSlot);
	appender_slot_ops.clear = appender_slot_clear;
	appender_slot_ops.getsomeattrs = appender_slot_getsomeattrs;
	appender_slot_ops.materialize = appender_slot_materialize;
	appender_slot_ops.copyslot = appender_slot_copyslot;
	appender_slot_ops.copy_heap_tuple = appender_slot_copy_heap_tuple;
	appender_slot_ops.copy_minimal_tuple = appender_slot_copy_minimal_tuple;

	RegisterCustomRmgr(RM_APPENDER_ID, &appender_rmgr);
	RegisterXactCallback(appender_xact_callback, NULL);
	RegisterSubXactCallback(appender_subxact_callback, NULL);
	CacheRegisterRelcacheCallback(appender_relcache_callback, (Datum) 0);

	prev_object_access_hook = object_access_hook;
	object_access_hook = appender_object_access;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = appender_ProcessUtility;
}

/* ----------------------------------------------------------------------------
 * SQL-callable functions
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(appender_tableam_handler);

Datum
appender_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&appender_methods);
}

/* Trigger on appender_meta: the manifest is maintained by this module only. */
PG_FUNCTION_INFO_V1(appender_meta_guard);

Datum
appender_meta_guard(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("appender_meta is managed by immotus_appender and must not be modified directly")));
	PG_RETURN_NULL();
}

/* On-disk size of an appender relation's segment files. */
PG_FUNCTION_INFO_V1(appender_relation_size_sql);

Datum
appender_relation_size_sql(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	AppenderScanSeg *own;
	AppenderScanSeg *segs;
	int			nown;
	int			nsegs;
	int64		size = 0;

	if (rel->rd_tableam != &appender_methods)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an appender table",
						RelationGetRelationName(rel))));

	nown = appender_own_segments(&rel->rd_locator, InvalidCommandId, &own);
	nsegs = appender_visible_segments(&rel->rd_locator, GetLatestSnapshot(),
									  own, nown, &segs);
	for (int i = 0; i < nsegs; i++)
		size += appender_seg_disk_size(&rel->rd_locator, segs[i].segno);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT64(size);
}
