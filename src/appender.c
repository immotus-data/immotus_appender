#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/syncscan.h"
#include "access/rmgr.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/valid.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "appender_memtuple.h"
#include "appender_datafile.h"
#include "appender_metadata.h"

PG_MODULE_MAGIC;

/*
 * Custom resource manager for appender WAL records.
 *
 * Row data is not WAL-logged: durability comes from fsync'ing dirty data
 * files at XACT_EVENT_PRE_COMMIT and from the transactional EOF update in
 * appender_meta (a regular heap table, WAL-logged by the standard
 * heap rmgr).  WAL bytes per inserted row are therefore ~0.
 *
 * The rmgr is registered so that WAL-logged appends for replication and PITR
 * can be added later through appender_append_durable() without changing the
 * insert path.  Until such records exist, appender_redo() must never be
 * called.
 *
 * Registered under RM_EXPERIMENTAL_ID until a unique ID is reserved.
 */
#define XLOG_APPENDER_APPEND		0x00	/* append (filenum, offset, data) */

/* WAL callbacks (defined near the bottom of this file). */
static void appender_redo(XLogReaderState *record);
static void appender_desc(StringInfo buf, XLogReaderState *record);
static const char *appender_identify(uint8 info);

/* Bulk insert buffer: defined near the insert path. */
static void appender_buf_flush_all(void);
static void appender_buf_discard(void);

static const RmgrData appender_rmgr = {
	.rm_name = "appender",
	.rm_redo = appender_redo,
	.rm_desc = appender_desc,
	.rm_identify = appender_identify,
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_mask = NULL,
	.rm_decode = NULL,
};

/* ----------------------------------------------------------------------------
 * Forward declarations: all callbacks referenced by appender_methods
 * ------------------------------------------------------------------------- */
static const TupleTableSlotOps *appender_slot_callbacks(Relation rel);
static TableScanDesc appender_beginscan(Relation rel, Snapshot snapshot,
										int nkeys, ScanKey key,
										ParallelTableScanDesc pscan,
										uint32 flags);
static void appender_endscan(TableScanDesc scan);
static void appender_rescan(TableScanDesc scan, ScanKey key,
							bool set_params, bool allow_strat,
							bool allow_sync, bool allow_pagemode);
static bool appender_getnextslot(TableScanDesc scan, ScanDirection direction,
								 TupleTableSlot *slot);
static Size appender_parallelscan_estimate(Relation rel);
static Size appender_parallelscan_initialize(Relation rel,
											 ParallelTableScanDesc pscan);
static void appender_parallelscan_reinitialize(Relation rel,
											   ParallelTableScanDesc pscan);
static struct IndexFetchTableData *appender_index_fetch_begin(Relation rel);
static void appender_index_fetch_reset(struct IndexFetchTableData *scan);
static void appender_index_fetch_end(struct IndexFetchTableData *scan);
static bool appender_index_fetch_tuple(struct IndexFetchTableData *scan,
									   ItemPointer tid, Snapshot snapshot,
									   TupleTableSlot *slot,
									   bool *call_again, bool *all_dead);
static void appender_tuple_insert(Relation rel, TupleTableSlot *slot,
								  CommandId cid, int options,
								  struct BulkInsertStateData *bistate);
static void appender_tuple_insert_speculative(Relation rel,
											  TupleTableSlot *slot,
											  CommandId cid, int options,
											  struct BulkInsertStateData *bistate,
											  uint32 specToken);
static void appender_tuple_complete_speculative(Relation rel,
												TupleTableSlot *slot,
												uint32 specToken,
												bool succeeded);
static void appender_multi_insert(Relation rel, TupleTableSlot **slots,
								  int nslots, CommandId cid, int options,
								  struct BulkInsertStateData *bistate);
static void appender_finish_bulk_insert(Relation rel, int options);
static TM_Result appender_tuple_delete(Relation rel, ItemPointer tid,
									   CommandId cid, Snapshot snapshot,
									   Snapshot crosscheck, bool wait,
									   TM_FailureData *tmfd,
									   bool changingPart);
static TM_Result appender_tuple_update(Relation rel, ItemPointer otid,
									   TupleTableSlot *slot, CommandId cid,
									   Snapshot snapshot, Snapshot crosscheck,
									   bool wait, TM_FailureData *tmfd,
									   LockTupleMode *lockmode,
									   TU_UpdateIndexes *update_indexes);
static TM_Result appender_tuple_lock(Relation rel, ItemPointer tid,
									 Snapshot snapshot,
									 TupleTableSlot *slot, CommandId cid,
									 LockTupleMode mode,
									 LockWaitPolicy wait_policy,
									 uint8 flags, TM_FailureData *tmfd);
static bool appender_fetch_row_version(Relation rel, ItemPointer tid,
									   Snapshot snapshot,
									   TupleTableSlot *slot);
static bool appender_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
static void appender_tuple_get_latest_tid(TableScanDesc scan,
										  ItemPointer tid);
static bool appender_tuple_satisfies_snapshot(Relation rel,
											  TupleTableSlot *slot,
											  Snapshot snapshot);
static void appender_relation_set_new_filelocator(Relation rel,
												  const RelFileLocator *newrloc,
												  char persistence,
												  TransactionId *freezeXid,
												  MultiXactId *minmulti);
static TransactionId appender_index_delete_tuples(Relation rel,
												  TM_IndexDeleteOp *delstate);
static void appender_relation_nontransactional_truncate(Relation rel);
static void appender_relation_copy_data(Relation rel,
										const RelFileLocator *newrloc);
static void appender_relation_copy_for_cluster(Relation OldHeap,
											   Relation NewHeap,
											   Relation OldIndex,
											   bool use_sort,
											   TransactionId OldestXmin,
											   TransactionId *xid_cutoff,
											   MultiXactId *multi_cutoff,
											   double *num_tuples,
											   double *tups_vacuumed,
											   double *tups_recently_dead);
static void appender_vacuum_rel(Relation rel, struct VacuumParams *params,
								BufferAccessStrategy bstrategy);
static bool appender_relation_needs_toast_table(Relation rel);
static uint64 appender_relation_size(Relation rel, ForkNumber forknum);
static void appender_relation_estimate_size(Relation rel, int32 *attr_widths,
											BlockNumber *pages, double *tuples,
											double *allvisfrac);
static double appender_index_build_range_scan(Relation heapRelation,
											  Relation indexRelation,
											  struct IndexInfo *indexInfo,
											  bool allow_sync, bool anyvisible,
											  bool progress,
											  BlockNumber start_blockno,
											  BlockNumber numblocks,
											  IndexBuildCallback callback,
											  void *callback_state,
											  TableScanDesc scan);
static void appender_index_validate_scan(Relation heapRelation,
										 Relation indexRelation,
										 struct IndexInfo *indexInfo,
										 Snapshot snapshot,
										 struct ValidateIndexState *state);
static bool appender_scan_analyze_next_block(TableScanDesc scan,
											 ReadStream *stream);
static bool appender_scan_analyze_next_tuple(TableScanDesc scan,
											 TransactionId OldestXmin,
											 double *liverows,
											 double *deadrows,
											 TupleTableSlot *slot);
static bool appender_scan_bitmap_next_tuple(TableScanDesc scan,
											TupleTableSlot *slot,
											bool *recheck,
											uint64 *lossy_pages,
											uint64 *exact_pages);
static bool appender_scan_sample_next_block(TableScanDesc scan,
											struct SampleScanState *scanstate);
static bool appender_scan_sample_next_tuple(TableScanDesc scan,
											struct SampleScanState *scanstate,
											TupleTableSlot *slot);

/* Internal helpers (not part of the TableAmRoutine). */
static MemTupleBinding *appender_get_binding(Relation rel);
static File appender_open_datafile(Relation rel);

/* ----------------------------------------------------------------------------
 * TableAmRoutine
 * ------------------------------------------------------------------------- */
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

	.tuple_insert = appender_tuple_insert,
	.tuple_insert_speculative = appender_tuple_insert_speculative,
	.tuple_complete_speculative = appender_tuple_complete_speculative,
	.multi_insert = appender_multi_insert,
	.finish_bulk_insert = appender_finish_bulk_insert,

	.tuple_delete = appender_tuple_delete,
	.tuple_update = appender_tuple_update,
	.tuple_lock = appender_tuple_lock,

	.tuple_fetch_row_version = appender_fetch_row_version,
	.tuple_tid_valid = appender_tuple_tid_valid,
	.tuple_get_latest_tid = appender_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = appender_tuple_satisfies_snapshot,

	.relation_set_new_filelocator = appender_relation_set_new_filelocator,
	.relation_nontransactional_truncate =
		appender_relation_nontransactional_truncate,
	.relation_copy_data = appender_relation_copy_data,
	.relation_copy_for_cluster = appender_relation_copy_for_cluster,
	.relation_vacuum = appender_vacuum_rel,
	.scan_analyze_next_block = appender_scan_analyze_next_block,
	.scan_analyze_next_tuple = appender_scan_analyze_next_tuple,
	.index_build_range_scan = appender_index_build_range_scan,
	.index_validate_scan = appender_index_validate_scan,
	.relation_size = appender_relation_size,
	.relation_needs_toast_table = appender_relation_needs_toast_table,
	.relation_estimate_size = appender_relation_estimate_size,

	.scan_sample_next_block = appender_scan_sample_next_block,
	.scan_sample_next_tuple = appender_scan_sample_next_tuple,
	.scan_bitmap_next_tuple = appender_scan_bitmap_next_tuple,
	.index_delete_tuples = appender_index_delete_tuples,
};

/* ----------------------------------------------------------------------------
 * Scan descriptor
 * ------------------------------------------------------------------------- */
typedef struct AppenderScanDescData
{
	TableScanDescData   rs_base;

	/* Frozen at scan start: committed EOF under the scan's snapshot. */
	int64               rs_eof;

	/* Datafile read state. */
	File                rs_file;        /* open datafile handle (read-only) */
	off_t               rs_offset;      /* byte offset of next block to read */
	uint32              rs_blockoff;    /* byte offset of current block (tid block field) */

	char               *rs_block;       /* payload buffer for current block */
	uint32              rs_blockcap;    /* allocated size of rs_block */
	uint32              rs_nrows;       /* rows in current block */
	uint32              rs_row;         /* next row index (0-based) */
	uint32              rs_curoff;      /* offset of next MemTuple in rs_block */

	MemTupleBinding    *rs_binding;     /* deform binding */
} AppenderScanDescData;

typedef AppenderScanDescData *AppenderScanDesc;

/* ----------------------------------------------------------------------------
 * Per-backend caches
 * ------------------------------------------------------------------------- */

/* One-entry cache for a relation's MemTupleBinding (form/deform layout). */
static Oid				appender_bind_relid = InvalidOid;
static MemTupleBinding *appender_bind_cache = NULL;

static MemTupleBinding *
appender_get_binding(Relation rel)
{
	if (appender_bind_relid != RelationGetRelid(rel))
	{
		MemoryContext oldctx;

		if (appender_bind_cache)
		{
			TupleDesc	oldtd = appender_bind_cache->tupdesc;

			destroy_memtuple_binding(appender_bind_cache);
			FreeTupleDesc(oldtd);
			appender_bind_cache = NULL;
		}

		/*
		 * Build the binding over a private copy of the tuple descriptor, kept
		 * in CacheMemoryContext.  Two reasons the naive approach dangles:
		 * (1) the current context during insert/scan is transaction or
		 * per-tuple scoped and freed at statement end; (2) the relcache-owned
		 * rd_att can be invalidated independently of our cache.
		 */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		appender_bind_cache =
			create_memtuple_binding(CreateTupleDescCopy(RelationGetDescr(rel)));
		MemoryContextSwitchTo(oldctx);
		appender_bind_relid = RelationGetRelid(rel);
	}
	return appender_bind_cache;
}

/* One-entry cache for the writable datafile handle. */
static RelFileNumber	appender_file_relnum = InvalidRelFileNumber;
static File				appender_file = -1;

static File
appender_open_datafile_locator(RelFileLocator *rlocator, bool create)
{
	if (appender_file_relnum != rlocator->relNumber)
	{
		if (appender_file >= 0)
			FileClose(appender_file);

		appender_file = appender_datafile_open(rlocator,
											  APPENDER_DATAFILE_FIRST_FILENUM,
											  create);
		appender_file_relnum = rlocator->relNumber;
	}
	return appender_file;
}

static File
appender_open_datafile(Relation rel)
{
	return appender_open_datafile_locator(&rel->rd_locator, true);
}

/*
 * Commit-time fsync batching.
 *
 * Data file writes land in the OS page cache only; they are made durable with
 * one FileSync per relation at XACT_EVENT_PRE_COMMIT, i.e. before the commit
 * record (and therefore the appender_meta EOF update) is flushed to WAL.  The EOF
 * can thus never become durable while the bytes that back it are still only
 * in the page cache.
 *
 * An abort discards the list without fsync: the aborted tail stays invisible
 * and is overwritten by the next committed insert.
 */
#define APPENDER_MAX_DIRTY_DATAFILES	64

static RelFileLocator appender_dirty_locators[APPENDER_MAX_DIRTY_DATAFILES];
static int			appender_dirty_count = 0;

static void
appender_mark_dirty(RelFileLocator *rlocator)
{
	RelFileLocator rloc = *rlocator;
	int			i;

	for (i = 0; i < appender_dirty_count; i++)
	{
		if (appender_dirty_locators[i].relNumber == rloc.relNumber &&
			appender_dirty_locators[i].dbOid == rloc.dbOid &&
			appender_dirty_locators[i].spcOid == rloc.spcOid)
			return;				/* already queued */
	}

	if (appender_dirty_count >= APPENDER_MAX_DIRTY_DATAFILES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many appender relations in one transaction")));

	appender_dirty_locators[appender_dirty_count++] = rloc;
}

static void
appender_flush_dirty(void)
{
	int			i;

	for (i = 0; i < appender_dirty_count; i++)
	{
		File		file;

		file = appender_datafile_open(&appender_dirty_locators[i],
									 APPENDER_DATAFILE_FIRST_FILENUM, false);
		if (FileSync(file, WAIT_EVENT_DATA_FILE_SYNC) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not fsync append-only data file: %m")));
		FileClose(file);
	}
	appender_dirty_count = 0;
}

/* ----------------------------------------------------------------------------
 * In-flight EOF (backend-local, per transaction)
 *
 * The appender_meta EOF is updated only once per transaction (at PRE_COMMIT), not
 * per row: a per-row heap update would leave one dead tuple per row and turn
 * each insert into an O(n) scan, i.e. O(n^2) overall.  Instead the running
 * EOF is kept here and published once at commit.
 *
 * Readers in the same transaction see their own appends via
 * appender_visible_eof(); the cmin gate keeps an older snapshot (e.g. a cursor
 * opened before the INSERT) from seeing them, matching heap semantics.
 * ------------------------------------------------------------------------- */
typedef struct AppenderEofEntry
{
	Oid			relid;
	int64		eof;			/* committed EOF + our own appends */
	CommandId	cmin;			/* command id of the last append */
} AppenderEofEntry;

#define APPENDER_MAX_EOF_ENTRIES	64

static AppenderEofEntry appender_eof_entries[APPENDER_MAX_EOF_ENTRIES];
static int		appender_eof_entry_count = 0;

/* Current append position: committed EOF plus our own in-flight appends. */
static int64
appender_current_eof(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	int			i;

	for (i = 0; i < appender_eof_entry_count; i++)
	{
		if (appender_eof_entries[i].relid == relid)
			return appender_eof_entries[i].eof;
	}

	if (appender_eof_entry_count >= APPENDER_MAX_EOF_ENTRIES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many appender relations in one transaction")));

	appender_eof_entries[appender_eof_entry_count].relid = relid;
	appender_eof_entries[appender_eof_entry_count].eof =
		appender_get_eof(rel, GetLatestSnapshot());
	appender_eof_entries[appender_eof_entry_count].cmin =
		GetCurrentCommandId(false);
	return appender_eof_entries[appender_eof_entry_count++].eof;
}

static void
appender_advance_eof(Oid relid, int64 neweof)
{
	int			i;

	for (i = 0; i < appender_eof_entry_count; i++)
	{
		if (appender_eof_entries[i].relid == relid)
		{
			appender_eof_entries[i].eof = neweof;
			appender_eof_entries[i].cmin = GetCurrentCommandId(false);
			return;
		}
	}
	elog(ERROR, "appender_advance_eof: no in-flight entry for relation %u",
		 relid);
}

/*
 * EOF visible to a reader under snapshot: committed EOF from appender_meta, plus
 * our own in-flight appends made at an earlier command id.
 */
static int64
appender_visible_eof(Relation rel, Snapshot snapshot)
{
	Oid			relid = RelationGetRelid(rel);
	int64		eof = appender_get_eof(rel, snapshot);
	int			i;

	for (i = 0; i < appender_eof_entry_count; i++)
	{
		if (appender_eof_entries[i].relid == relid &&
			appender_eof_entries[i].eof > eof &&
			appender_eof_entries[i].cmin < snapshot->curcid)
			eof = appender_eof_entries[i].eof;
	}
	return eof;
}

/* Publish all in-flight EOFs to appender_meta (called at PRE_COMMIT). */
static void
appender_flush_eof(void)
{
	int			i;

	for (i = 0; i < appender_eof_entry_count; i++)
		appender_update_eof(appender_eof_entries[i].relid,
							appender_eof_entries[i].eof);

	appender_eof_entry_count = 0;
}

static void
appender_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			/* write any partial insert buffer, fsync, then publish EOF */
			appender_buf_flush_all();
			appender_flush_dirty();
			appender_flush_eof();
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			appender_buf_discard();
			appender_dirty_count = 0;
			appender_eof_entry_count = 0;
			break;
		default:
			break;
	}
}

/*
 * Write one data block and make it durable.
 *
 * Every row append goes through here.  The block is written at offset (the
 * committed EOF), the data file is queued for a commit-time fsync, and the
 * transactional commit point is the EOF update in appender_meta.  No WAL record
 * is emitted; WAL-logged appends for replication and PITR can be added here
 * later without changing the insert path.
 */
static off_t
appender_append_durable(RelFileLocator *rlocator, int filenum, off_t offset,
						const char *data, Size len, uint32 rowcount)
{
	File		file = appender_open_datafile_locator(rlocator, true);
	off_t		blockoff;

	(void) filenum;				/* single data file for now */

	blockoff = appender_datafile_append(file, data, len, rowcount, offset);

	/* Batch the fsync to commit time. */
	appender_mark_dirty(rlocator);

	return blockoff;
}

/* ----------------------------------------------------------------------------
 * Bulk insert buffer
 *
 * Several MemTuples are packed into one varblock to amortise the per-row
 * FileWrite syscall and the 16-byte block header that made M1 ~46% slower than
 * heap on bulk INSERT ... SELECT.  One buffer per backend (single active
 * append target at a time); it lives in TopTransactionContext and is flushed
 * when full or at XACT_EVENT_PRE_COMMIT.
 * ------------------------------------------------------------------------- */
#define APPENDER_BLOCK_TARGET	(32 * 1024)	/* target varblock payload size */

typedef struct AppenderInsBuf
{
	Oid				relid;
	RelFileLocator	rlocator;
	char		   *data;		/* packed MemTuples */
	Size			len;		/* used bytes */
	Size			cap;		/* allocated bytes */
	uint32			rowcount;	/* MemTuples in data */
	off_t			blockoff;	/* byte offset of this block in the datafile */
} AppenderInsBuf;

static AppenderInsBuf *appender_ins_buf = NULL;

static void
appender_buf_ensure_capacity(AppenderInsBuf *buf, Size needed)
{
	MemoryContext oldctx;

	needed = Max(needed, (Size) APPENDER_BLOCK_TARGET);
	if (needed <= buf->cap)
		return;

	/* keep the payload alive across tuple_insert calls (transaction lifetime) */
	oldctx = MemoryContextSwitchTo(TopTransactionContext);
	if (buf->data == NULL)
		buf->data = palloc(needed);
	else
		buf->data = repalloc(buf->data, needed);
	MemoryContextSwitchTo(oldctx);
	buf->cap = needed;
}

static void
appender_buf_flush(AppenderInsBuf *buf)
{
	if (buf->rowcount == 0)
		return;

	/* EOF was already advanced per buffered row; just materialise the block. */
	appender_append_durable(&buf->rlocator,
							APPENDER_DATAFILE_FIRST_FILENUM,
							buf->blockoff,
							buf->data, buf->len, buf->rowcount);

	buf->len = 0;
	buf->rowcount = 0;
}

static AppenderInsBuf *
appender_get_insert_buf(Relation rel)
{
	if (appender_ins_buf == NULL)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(TopTransactionContext);

		appender_ins_buf = palloc0(sizeof(AppenderInsBuf));
		MemoryContextSwitchTo(oldctx);
		appender_ins_buf->relid = RelationGetRelid(rel);
		appender_ins_buf->rlocator = rel->rd_locator;
	}
	else if (appender_ins_buf->relid != RelationGetRelid(rel))
	{
		/* switching append targets mid-transaction: flush the old buffer */
		appender_buf_flush(appender_ins_buf);
		appender_ins_buf->relid = RelationGetRelid(rel);
		appender_ins_buf->rlocator = rel->rd_locator;
		appender_ins_buf->len = 0;
		appender_ins_buf->rowcount = 0;
	}
	return appender_ins_buf;
}

static void
appender_buf_flush_all(void)
{
	if (appender_ins_buf != NULL)
	{
		appender_buf_flush(appender_ins_buf);
		appender_ins_buf = NULL;	/* TopTransactionContext frees it */
	}
}

static void
appender_buf_discard(void)
{
	appender_ins_buf = NULL;	/* TopTransactionContext frees it on abort */
}

/*
 * Materialise our own buffered rows for relid so a scan in the same
 * transaction can read them: rows sitting in the insert buffer are not on disk
 * yet, and the scan reads only the datafile.
 */
static void
appender_flush_insert_buf_for_rel(Oid relid)
{
	if (appender_ins_buf != NULL &&
		appender_ins_buf->relid == relid &&
		appender_ins_buf->rowcount > 0)
		appender_buf_flush(appender_ins_buf);
}

static const TupleTableSlotOps *
appender_slot_callbacks(Relation rel)
{
	return &TTSOpsVirtual;
}

/* ----------------------------------------------------------------------------
 * INSERT
 * ------------------------------------------------------------------------- */
static void
appender_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					  int options, struct BulkInsertStateData *bistate)
{
	MemTupleBinding *binding = appender_get_binding(rel);
	MemTuple        mtup;
	uint32          mtsize;
	AppenderInsBuf *buf;

	/* Materialise the incoming slot into Datum/IsNull arrays. */
	slot_getallattrs(slot);

	mtup = memtuple_form(binding, slot->tts_values, slot->tts_isnull);
	mtsize = memtuple_get_size(mtup);

	/*
	 * Pack the MemTuple into the current insert buffer: several tuples
	 * share one varblock, amortising the per-row FileWrite and the 16-byte
	 * block header that made M1 ~46% slower than heap.  The block is written
	 * when full or at commit, so a tuple's TID is (block byte offset, row
	 * number within the block).
	 */
	buf = appender_get_insert_buf(rel);

	if (buf->rowcount == 0)
		buf->blockoff = appender_current_eof(rel);

	if (buf->len > 0 && buf->len + mtsize > APPENDER_BLOCK_TARGET)
	{
		appender_buf_flush(buf);
		buf->blockoff = appender_current_eof(rel);
	}

	appender_buf_ensure_capacity(buf, buf->len + mtsize);
	memcpy(buf->data + buf->len, mtup, mtsize);
	buf->len += mtsize;
	buf->rowcount++;

	/*
	 * Advance the in-flight EOF per row, not per flush: a SELECT in the same
	 * transaction must see rows that are still sitting in the unflushed
	 * buffer.  The block is written at buf->blockoff later, so the logical EOF
	 * is blockoff + header + buffered payload.
	 */
	appender_advance_eof(RelationGetRelid(rel),
						 buf->blockoff + sizeof(AppenderBlockHeader) + buf->len);

	/* TID: block byte offset + row number (byte offset fits <4GB tables). */
	ItemPointerSet(&slot->tts_tid, (BlockNumber) buf->blockoff, buf->rowcount);

	pfree(mtup);
}

static void
appender_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								  CommandId cid, int options,
								  struct BulkInsertStateData *bistate,
								  uint32 tok)
{
	elog(ERROR, "appender: INSERT ON CONFLICT not supported");
}

static void
appender_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
									uint32 tok, bool succeeded)
{
	elog(ERROR, "appender: INSERT ON CONFLICT not supported");
}

static void
appender_multi_insert(Relation rel, TupleTableSlot **slots, int ntuples,
					  CommandId cid, int options,
					  struct BulkInsertStateData *bistate)
{
	int i;

	for (i = 0; i < ntuples; i++)
		appender_tuple_insert(rel, slots[i], cid, options, bistate);
}

static void
appender_finish_bulk_insert(Relation rel, int options)
{
	/* no-op: we flush every tuple immediately */
}

/* ----------------------------------------------------------------------------
 * Visibility
 *
 * Because rows are never updated or deleted, visibility reduces to a single
 * question: did the inserting transaction (xmin) commit, and is that commit
 * visible to the caller's snapshot?  There is no xmax, no multixact, no HOT
 * chain and no freezing-for-wraparound to consider.  The only dead tuples we
 * can ever encounter are the remnants of aborted inserts, which we permanently
 * mark with HEAP_XMIN_INVALID the first time we notice them.
 * ------------------------------------------------------------------------- */

/*
 * Visibility for append-only data comes entirely from the committed EOF in
 * appender_meta, read under the caller's snapshot (appender_get_eof).  MemTuples
 * carry no xmin/xmax, so there is no per-tuple visibility test: a tuple is
 * visible iff its block lies below the snapshot's EOF.
 */
static bool
appender_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	MemTupleBinding *binding = appender_get_binding(rel);
	int64			eof = appender_get_eof(rel, snapshot);
	off_t			blockoff = ItemPointerGetBlockNumber(tid);
	uint32			rowno = ItemPointerGetOffsetNumber(tid);	/* 1-based */
	File			file;
	AppenderBlockHeader hdr;
	char		   *payload;
	MemTuple		mtup;
	uint32			off = 0;
	uint32			i;
	ssize_t			n;

	if (blockoff >= eof)
		return false;			/* block not committed under snapshot */

	file = appender_open_datafile(rel);

	n = FileRead(file, &hdr, sizeof(hdr), blockoff, WAIT_EVENT_DATA_FILE_READ);
	if (n != sizeof(hdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read append-only block header: %m")));

	payload = palloc(hdr.datalen);
	if (hdr.datalen > 0)
	{
		n = FileRead(file, payload, hdr.datalen, blockoff + sizeof(hdr),
					 WAIT_EVENT_DATA_FILE_READ);
		if (n != hdr.datalen)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read append-only block payload: %m")));
	}

	/* Walk MemTuples in the block to the requested row. */
	for (i = 0; i < rowno - 1; i++)
	{
		MemTuple m = (MemTuple) (payload + off);
		off += memtuple_get_size(m);
	}
	mtup = (MemTuple) (payload + off);

	ExecClearTuple(slot);
	memtuple_deform(mtup, binding, slot->tts_values, slot->tts_isnull);
	ExecStoreVirtualTuple(slot);
	ItemPointerSet(&slot->tts_tid, (BlockNumber) blockoff, rowno);

	pfree(payload);
	return true;
}

static bool
appender_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	int64 eof = appender_get_eof(scan->rs_rd, scan->rs_snapshot);

	return (int64) ItemPointerGetBlockNumber(tid) < eof;
}

static void
appender_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	/*
	 * Append-only: the current tuple is always the latest version (no
	 * HOT-chain, no UPDATEs).
	 */
}

static bool
appender_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	int64 eof = appender_get_eof(rel, snapshot);

	/*
	 * The scan only ever produces tuples below the snapshot's EOF, so for a
	 * slot produced by the scan this is trivially true.  Re-derive it from the
	 * tid's block offset for safety (e.g. EPQ rechecks).
	 */
	return (int64) ItemPointerGetBlockNumber(&slot->tts_tid) < eof;
}

/* ----------------------------------------------------------------------------
 * UPDATE / DELETE / LOCK: all raise errors except LOCK
 * ------------------------------------------------------------------------- */
static TM_Result
appender_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					  Snapshot snapshot, Snapshot crosscheck, bool wait,
					  TM_FailureData *tmfd, bool changingPart)
{
	elog(ERROR, "appender: DELETE not supported (use heap)");
	return TM_Invisible;            /* unreachable */
}

static TM_Result
appender_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					  bool wait, TM_FailureData *tmfd,
					  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	elog(ERROR, "appender: UPDATE not supported (use heap)");
	return TM_Invisible;            /* unreachable */
}

static TM_Result
appender_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
					TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					LockWaitPolicy wait_policy, uint8 flags,
					TM_FailureData *tmfd)
{
	/*
	 * Append-only data never changes, so there is nothing to lock: a visible
	 * row can never be concurrently updated or deleted.  We simply fetch the
	 * row version and report success, or TM_Invisible if it is not visible to
	 * the caller's snapshot.
	 */
	if (!appender_fetch_row_version(rel, tid, snapshot, slot))
	{
		tmfd->ctid = *tid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
		return TM_Invisible;
	}
	return TM_Ok;
}

static TableScanDesc
appender_beginscan(Relation rel, Snapshot snapshot, int nkeys, ScanKey key,
				   ParallelTableScanDesc pscan, uint32 flags)
{
	AppenderScanDesc scan;
	RelFileLocator rlocator;

	RelationIncrementReferenceCount(rel);

	scan = (AppenderScanDesc) palloc0(sizeof(AppenderScanDescData));
	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;

	if (nkeys > 0)
	{
		scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
		memcpy(scan->rs_base.rs_key, key, nkeys * sizeof(ScanKeyData));
	}
	else
		scan->rs_base.rs_key = NULL;

	/*
	 * Serial seqscan/sample scan in a serializable transaction: predicate-lock
	 * the whole relation (there is nothing finer-grained to lock).
	 */
	if (flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		Assert(snapshot);
		PredicateLockRelation(rel, snapshot);
	}

	/* Materialise our own buffered rows, then freeze the visible EOF. */
	appender_flush_insert_buf_for_rel(RelationGetRelid(rel));

	/* Freeze the committed EOF under our snapshot (plus our own appends). */
	scan->rs_eof = appender_visible_eof(rel, snapshot);

	/*
	 * Open the datafile lazily: an empty table (or one just TRUNCATEd to a
	 * fresh relfilenode) has EOF 0 and no datafile on disk yet.  The scan
	 * loop stops at rs_offset >= rs_eof, so the handle is only needed when
	 * rs_eof > 0.
	 */
	if (scan->rs_eof > 0)
	{
		rlocator = rel->rd_locator;
		scan->rs_file = appender_datafile_open(&rlocator,
											  APPENDER_DATAFILE_FIRST_FILENUM,
											  false);
	}
	else
		scan->rs_file = -1;

	scan->rs_binding = create_memtuple_binding(RelationGetDescr(rel));

	return (TableScanDesc) scan;
}

static void
appender_endscan(TableScanDesc s)
{
	AppenderScanDesc scan = (AppenderScanDesc) s;

	if (scan->rs_file >= 0)
		FileClose(scan->rs_file);
	if (scan->rs_block)
		pfree(scan->rs_block);
	if (scan->rs_binding)
		destroy_memtuple_binding(scan->rs_binding);
	if (scan->rs_base.rs_key)
		pfree(scan->rs_base.rs_key);
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	RelationDecrementReferenceCount(scan->rs_base.rs_rd);
	pfree(scan);
}

static void
appender_rescan(TableScanDesc s, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	AppenderScanDesc scan = (AppenderScanDesc) s;
	RelFileLocator rlocator;

	/* Rewind: materialise our own buffered rows and refreeze the EOF. */
	appender_flush_insert_buf_for_rel(RelationGetRelid(scan->rs_base.rs_rd));

	if (scan->rs_file >= 0)
		FileClose(scan->rs_file);

	scan->rs_eof = appender_visible_eof(scan->rs_base.rs_rd,
										scan->rs_base.rs_snapshot);

	if (scan->rs_eof > 0)
	{
		rlocator = scan->rs_base.rs_rd->rd_locator;
		scan->rs_file = appender_datafile_open(&rlocator,
											  APPENDER_DATAFILE_FIRST_FILENUM,
											  false);
	}
	else
		scan->rs_file = -1;

	scan->rs_offset = 0;
	scan->rs_blockoff = 0;
	scan->rs_nrows = 0;
	scan->rs_row = 0;
	scan->rs_curoff = 0;
}

static bool
appender_getnextslot(TableScanDesc s, ScanDirection direction,
					 TupleTableSlot *slot)
{
	AppenderScanDesc scan = (AppenderScanDesc) s;

	for (;;)
	{
		/* Hand out the next MemTuple in the current block. */
		if (scan->rs_block != NULL && scan->rs_row < scan->rs_nrows)
		{
			MemTuple	mtup = (MemTuple) (scan->rs_block + scan->rs_curoff);
			uint32		sz = memtuple_get_size(mtup);

			ExecClearTuple(slot);
			memtuple_deform(mtup, scan->rs_binding,
							slot->tts_values, slot->tts_isnull);
			ExecStoreVirtualTuple(slot);
			ItemPointerSet(&slot->tts_tid, (BlockNumber) scan->rs_blockoff,
						   scan->rs_row + 1);

			scan->rs_curoff += sz;
			scan->rs_row++;
			return true;
		}

		/* Read the next block until the snapshot's EOF. */
		if (scan->rs_offset >= scan->rs_eof)
			break;

		{
			AppenderBlockHeader hdr;
			ssize_t		n;

			n = FileRead(scan->rs_file, &hdr, sizeof(hdr), scan->rs_offset,
						 WAIT_EVENT_DATA_FILE_READ);
			if (n != sizeof(hdr))
				elog(ERROR, "appender: short read of block header");

			if (hdr.datalen > scan->rs_blockcap)
			{
				if (scan->rs_block)
					pfree(scan->rs_block);
				scan->rs_block = palloc(hdr.datalen);
				scan->rs_blockcap = hdr.datalen;
			}

			if (hdr.datalen > 0)
			{
				n = FileRead(scan->rs_file, scan->rs_block, hdr.datalen,
							 scan->rs_offset + sizeof(hdr),
							 WAIT_EVENT_DATA_FILE_READ);
				if (n != hdr.datalen)
					elog(ERROR, "appender: short read of block payload");
			}

			scan->rs_blockoff = (uint32) scan->rs_offset;
			scan->rs_offset += sizeof(hdr) + hdr.datalen;
			scan->rs_nrows = hdr.rowcount;
			scan->rs_row = 0;
			scan->rs_curoff = 0;
		}
	}

	ExecClearTuple(slot);
	return false;
}

/* ----------------------------------------------------------------------------
 * Relation management callbacks
 * ------------------------------------------------------------------------- */
static void
appender_relation_set_new_filelocator(Relation rel,
									  const RelFileLocator *newrloc,
									  char persistence,
									  TransactionId *freezeXid,
									  MultiXactId *minmulti)
{
	SMgrRelation srel;

	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();

	/*
	 * register_delete = true: if the creating transaction aborts, the freshly
	 * created file is unlinked (matches heapam_relation_set_new_filelocator).
	 */
	srel = RelationCreateStorage(*newrloc, persistence, true);

	/*
	 * For an unlogged table, set up an init fork so that crash recovery can
	 * reset the relation to an empty state (heap does the same).  Without it,
	 * ResetUnloggedRelations() would fail on the missing fork during startup.
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrloc, INIT_FORKNUM);
	}

	smgrclose(srel);

	/*
	 * The relation now has a fresh relfilenode: any committed EOF recorded
	 * against the old relfilenode's data file is stale.  Reset the metadata
	 * row and any in-flight EOF entry so readers see an empty table.
	 */
	appender_reset_eof(RelationGetRelid(rel));
	{
		int			i;

		for (i = 0; i < appender_eof_entry_count; i++)
		{
			if (appender_eof_entries[i].relid == RelationGetRelid(rel))
			{
				appender_eof_entries[i].eof = 0;
				appender_eof_entries[i].cmin = GetCurrentCommandId(false);
			}
		}
	}
}

static TransactionId
appender_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	elog(ERROR, "appender: index delete not supported");
	return InvalidTransactionId;
}

static void
appender_relation_nontransactional_truncate(Relation rel)
{
	/*
	 * Truncate in place (to zero blocks), mirroring
	 * heapam_relation_nontransactional_truncate().  This callback is only
	 * used for the nontransactional path (e.g. TRUNCATE of a table created in
	 * the current subtransaction), where in-place truncation is safe because
	 * a rollback would throw the whole relation away anyway.
	 */
	RelationTruncate(rel, 0);
}

static void
appender_relation_copy_data(Relation rel, const RelFileLocator *newrloc)
{
	elog(ERROR, "appender: copy data not supported");
}

static void
appender_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								   Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples,
								   double *tups_vacuumed,
								   double *tups_recently_dead)
{
	elog(ERROR, "appender: CLUSTER not supported");
}

static void
appender_vacuum_rel(Relation rel, VacuumParams *params,
					BufferAccessStrategy bstrategy)
{
	BlockNumber nblocks;
	bool		frozenxid_updated = false;
	bool		minmulti_updated = false;

	/*
	 * Append-only storage has no dead tuples to prune and no per-tuple xmin to
	 * freeze: MemTuples carry no xmin, and visibility is the committed EOF.
	 * For this milestone VACUUM only refreshes the size estimate.  A proper
	 * live-tuple count is a later milestone (scan_analyze / pgstat work).
	 */
	nblocks = (BlockNumber) (appender_get_eof(rel, GetLatestSnapshot()) / BLCKSZ);

	vac_update_relstats(rel, nblocks, 0.0,
						0, 0,
						false,
						rel->rd_rel->relfrozenxid,
						rel->rd_rel->relminmxid,
						&frozenxid_updated, &minmulti_updated, false);
}

static bool
appender_relation_needs_toast_table(Relation rel)
{
	/*
	 * No TOAST support in MVP: tuples that don't fit on an 8 KB page will
	 * be rejected.
	 */
	return false;
}

static uint64
appender_relation_size(Relation rel, ForkNumber forknum)
{
	/*
	 * Row data lives in the append-only data file file (relfilenumber.dat.1), not
	 * in the main fork (which is always empty).  Its logical size is the
	 * committed EOF.  There are no FSM/VM forks.
	 *
	 * Note: vanilla pg_relation_size() does NOT consult this callback: it
	 * stat()s fork files directly, so it reports the (empty) main fork.  This
	 * callback still matters for core table_relation_size() callers.
	 */
	if (forknum != MAIN_FORKNUM)
		return 0;

	return (uint64) appender_get_eof(rel, GetLatestSnapshot());
}

static void
appender_relation_estimate_size(Relation rel, int32 *attr_widths,
								BlockNumber *pages, double *tuples,
								double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages, tuples,
									   allvisfrac, 0, BLCKSZ);
}

/* ----------------------------------------------------------------------------
 * Index-related stubs: indexes are not supported in the MVP
 * ------------------------------------------------------------------------- */
static IndexFetchTableData *
appender_index_fetch_begin(Relation rel)
{
	elog(ERROR, "appender: indexes not supported");
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
appender_index_fetch_tuple(struct IndexFetchTableData *scan,
						   ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot, bool *call_again,
						   bool *all_dead)
{
	elog(ERROR, "appender: indexes not supported");
	return false;
}

static double
appender_index_build_range_scan(Relation heapRelation,
								Relation indexRelation,
								struct IndexInfo *indexInfo,
								bool allow_sync,
								bool anyvisible,
								bool progress,
								BlockNumber start_blockno,
								BlockNumber numblocks,
								IndexBuildCallback callback,
								void *callback_state,
								TableScanDesc scan)
{
	elog(ERROR, "appender: index build not supported");
	return 0;
}

static void
appender_index_validate_scan(Relation heapRelation,
							 Relation indexRelation,
							 struct IndexInfo *indexInfo,
							 Snapshot snapshot,
							 struct ValidateIndexState *state)
{
	elog(ERROR, "appender: index validate not supported");
}

/* ----------------------------------------------------------------------------
 * ANALYZE stubs: return no tuples (ANALYZE will see an empty table)
 * ------------------------------------------------------------------------- */
static bool
appender_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
appender_scan_analyze_next_tuple(TableScanDesc scan,
								 TransactionId OldestXmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	return false;
}

/* ----------------------------------------------------------------------------
 * Bitmap / sample scan stubs
 * ------------------------------------------------------------------------- */
static bool
appender_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
								bool *recheck, uint64 *recheck_dist,
								uint64 *recheck_optimistic)
{
	elog(ERROR, "appender: bitmap scans not supported");
	return false;
}

static bool
appender_scan_sample_next_block(TableScanDesc scan,
								struct SampleScanState *scanstate)
{
	elog(ERROR, "appender: TABLESAMPLE not supported");
	return false;
}

static bool
appender_scan_sample_next_tuple(TableScanDesc scan,
								struct SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	elog(ERROR, "appender: TABLESAMPLE not supported");
	return false;
}

/* ----------------------------------------------------------------------------
 * Parallel scan: delegate to standard block-oriented helpers
 * ------------------------------------------------------------------------- */
static Size
appender_parallelscan_estimate(Relation rel)
{
	/* Parallel scan is not supported in this milestone. */
	return 0;
}

static Size
appender_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	return table_block_parallelscan_initialize(rel, pscan);
}

static void
appender_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	table_block_parallelscan_reinitialize(rel, pscan);
}

/* ----------------------------------------------------------------------------
 * WAL redo / description / identification
 * ------------------------------------------------------------------------- */

/*
 * No WAL records are currently written, so there is nothing to redo.  This is
 * reached only once WAL-logged appends are emitted; the redo logic (append to
 * the data file) goes here.
 */
static void
appender_redo(XLogReaderState *record)
{
	elog(PANIC, "appender_redo: unexpected WAL record under the appender rmgr");
}

static void
appender_desc(StringInfo buf, XLogReaderState *record)
{
	appendStringInfo(buf, "appender append");
}

static const char *
appender_identify(uint8 info)
{
	return "APPEND";
}

void
_PG_init(void)
{
	/*
	 * A custom resource manager may only be registered while
	 * shared_preload_libraries is being processed.  Appender WAL records are
	 * written under this rmgr, so the server could not replay them after a
	 * crash unless the library is preloaded: fail hard if it is not.
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("immotus_appender must be loaded via shared_preload_libraries"),
				 errdetail("A custom WAL resource manager can only be registered during server startup."),
				 errhint("Add immotus_appender to shared_preload_libraries in postgresql.conf and restart the server.")));

	RegisterCustomRmgr(RM_EXPERIMENTAL_ID, &appender_rmgr);

	/* Commit-time fsync batching for data files. */
	RegisterXactCallback(appender_xact_callback, NULL);
}

PG_FUNCTION_INFO_V1(appender_tableam_handler);

Datum
appender_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&appender_methods);
}

/*
 * Trigger guard for appender_meta: block any direct user DML on the metadata
 * table.  The extension's own maintenance uses simple_heap_insert/update/
 * delete, which bypass triggers.
 */
PG_FUNCTION_INFO_V1(appender_meta_guard);

Datum
appender_meta_guard(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("appender_meta is managed by immotus_appender and must not be modified directly")));

	PG_RETURN_POINTER(NULL);		/* unreachable */
}
