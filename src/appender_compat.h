/*-------------------------------------------------------------------------
 *
 * appender_compat.h
 *	  Definitions that let the access method build against the supported
 *	  PostgreSQL releases.
 *
 * The code targets 17 and later.  Almost every difference between the
 * branches is a signature of a TableAmRoutine callback or of one of the few
 * server functions the method calls, so they are collected here and the rest
 * of the sources stays free of version tests.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_COMPAT_H
#define APPENDER_COMPAT_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_attribute.h"

#if PG_VERSION_NUM < 170000
#error "immotus_appender requires PostgreSQL 17 or later"
#endif

#if PG_VERSION_NUM >= 200000
#error "immotus_appender has not been ported to PostgreSQL 20 yet"
#endif

/* ---------------------------------------------------------------------------
 * Scan entry points
 * -------------------------------------------------------------------------
 */

/*
 * table_beginscan() takes a ScanOptions argument from 19 on.  The scans below
 * are plain sequential scans, which pass none of the caller-settable options.
 */
#if PG_VERSION_NUM >= 190000
#define appender_table_beginscan(rel, snapshot, nkeys, key) \
	table_beginscan((rel), (snapshot), (nkeys), (key), 0)
#else
#define appender_table_beginscan(rel, snapshot, nkeys, key) \
	table_beginscan((rel), (snapshot), (nkeys), (key))
#endif

/*
 * index_beginscan() gained scan instrumentation in 18 and options in 19.
 * The catalog scans below are never instrumented.
 */
#if PG_VERSION_NUM >= 190000
#define appender_index_beginscan(heaprel, indexrel, snapshot, nkeys, norderbys) \
	index_beginscan((heaprel), (indexrel), (snapshot), NULL, (nkeys), \
					(norderbys), 0)
#elif PG_VERSION_NUM >= 180000
#define appender_index_beginscan(heaprel, indexrel, snapshot, nkeys, norderbys) \
	index_beginscan((heaprel), (indexrel), (snapshot), NULL, (nkeys), \
					(norderbys))
#else
#define appender_index_beginscan(heaprel, indexrel, snapshot, nkeys, norderbys) \
	index_beginscan((heaprel), (indexrel), (snapshot), (nkeys), (norderbys))
#endif

/* ---------------------------------------------------------------------------
 * TableAmRoutine callbacks
 * -------------------------------------------------------------------------
 */

/*
 * The modification callbacks take their options as uint32 from 19 on.
 */
#if PG_VERSION_NUM >= 190000
#define APPENDER_OPTIONS_TYPE uint32
#else
#define APPENDER_OPTIONS_TYPE int
#endif

#define APPENDER_INSERT_ARGS \
	Relation rel, TupleTableSlot *slot, CommandId cid, \
	APPENDER_OPTIONS_TYPE options, struct BulkInsertStateData *bistate

#define APPENDER_INSERT_SPECULATIVE_ARGS \
	Relation rel, TupleTableSlot *slot, CommandId cid, \
	APPENDER_OPTIONS_TYPE options, struct BulkInsertStateData *bistate, \
	uint32 specToken

#define APPENDER_MULTI_INSERT_ARGS \
	Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, \
	APPENDER_OPTIONS_TYPE options, struct BulkInsertStateData *bistate

#define APPENDER_FINISH_BULK_INSERT_ARGS \
	Relation rel, APPENDER_OPTIONS_TYPE options

/*
 * tuple_delete() and tuple_update() take an options argument from 19 on; the
 * trailing "changingPart" flag of tuple_delete() became one of the options.
 */
#if PG_VERSION_NUM >= 190000
#define APPENDER_DELETE_ARGS \
	Relation rel, ItemPointer tid, CommandId cid, uint32 options, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd

#define APPENDER_UPDATE_ARGS \
	Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, \
	uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, \
	TM_FailureData *tmfd, LockTupleMode *lockmode, \
	TU_UpdateIndexes *update_indexes
#else
#define APPENDER_DELETE_ARGS \
	Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, \
	Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart

#define APPENDER_UPDATE_ARGS \
	Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, \
	LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes
#endif

/* 19 added the snapshot CLUSTER reads the old rows with. */
#if PG_VERSION_NUM >= 190000
#define APPENDER_COPY_FOR_CLUSTER_ARGS \
	Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, \
	TransactionId OldestXmin, Snapshot snapshot, TransactionId *xid_cutoff, \
	MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, \
	double *tups_recently_dead
#else
#define APPENDER_COPY_FOR_CLUSTER_ARGS \
	Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, \
	TransactionId OldestXmin, TransactionId *xid_cutoff, \
	MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, \
	double *tups_recently_dead
#endif

/* 19 passes VACUUM's parameters read-only. */
#if PG_VERSION_NUM >= 190000
#define APPENDER_VACUUM_PARAMS const VacuumParams *
#else
#define APPENDER_VACUUM_PARAMS VacuumParams *
#endif

/* 19 dropped OldestXmin from ANALYZE's per-tuple callback. */
#if PG_VERSION_NUM >= 190000
#define APPENDER_ANALYZE_NEXT_TUPLE_ARGS \
	TableScanDesc sscan, double *liverows, double *deadrows, \
	TupleTableSlot *slot
#else
#define APPENDER_ANALYZE_NEXT_TUPLE_ARGS \
	TableScanDesc sscan, TransactionId OldestXmin, double *liverows, \
	double *deadrows, TupleTableSlot *slot
#endif

/* The bitmap scan callback took a TBMIterateResult up to 17. */
#if PG_VERSION_NUM >= 180000
#define APPENDER_BITMAP_NEXT_TUPLE_ARGS \
	TableScanDesc scan, TupleTableSlot *slot, bool *recheck, \
	uint64 *lossy_pages, uint64 *exact_pages
#else
#define APPENDER_BITMAP_NEXT_TUPLE_ARGS \
	TableScanDesc scan, struct TBMIterateResult *tbmres, TupleTableSlot *slot
#endif

/* 19 added the options argument to index_fetch_begin(). */
#if PG_VERSION_NUM >= 190000
#define APPENDER_INDEX_FETCH_BEGIN_ARGS Relation rel, uint32 flags
#else
#define APPENDER_INDEX_FETCH_BEGIN_ARGS Relation rel
#endif

/* ---------------------------------------------------------------------------
 * Smaller differences
 * -------------------------------------------------------------------------
 */

/* copy_minimal_tuple() takes the size of the buffer it copies into in 18. */
#if PG_VERSION_NUM >= 180000
#define APPENDER_COPY_MINIMAL_TUPLE_ARGS TupleTableSlot *slot, Size extra
#define APPENDER_COPY_MINIMAL_TUPLE_EXTRA , extra
#else
#define APPENDER_COPY_MINIMAL_TUPLE_ARGS TupleTableSlot *slot
#define APPENDER_COPY_MINIMAL_TUPLE_EXTRA
#endif

/* 18 renamed the parallel scan's relation id to a RelFileLocator. */
#if PG_VERSION_NUM >= 180000
#define APPENDER_PSCAN_SET_LOCATOR(pscan, rel) \
	((pscan)->phs_locator = (rel)->rd_locator)
#else
#define APPENDER_PSCAN_SET_LOCATOR(pscan, rel) \
	((pscan)->phs_relid = RelationGetRelid(rel))
#endif

/*
 * create_memtuple_binding() describes rows that hold fewer attributes than the
 * table has now, so it works on a truncated copy of the descriptor.  Core grew
 * CreateTupleDescTruncatedCopy() for that in 18.
 */
static inline TupleDesc
appender_truncated_tupdesc(TupleDesc tupdesc, int natts)
{
	Assert(natts <= tupdesc->natts);

#if PG_VERSION_NUM >= 180000
	return CreateTupleDescTruncatedCopy(tupdesc, natts);
#else
	{
		TupleDesc	desc = CreateTemplateTupleDesc(natts);
		int			i;

		memcpy(TupleDescAttr(desc, 0), TupleDescAttr(tupdesc, 0),
			   natts * sizeof(FormData_pg_attribute));

		/* no constraints or defaults are copied along with the attributes */
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			att->attcacheoff = -1;
			att->attnotnull = false;
			att->atthasdef = false;
			att->atthasmissing = false;
			att->attidentity = '\0';
			att->attgenerated = '\0';
		}

		desc->tdtypeid = tupdesc->tdtypeid;
		desc->tdtypmod = tupdesc->tdtypmod;
		return desc;
	}
#endif
}

#endif							/* APPENDER_COMPAT_H */
