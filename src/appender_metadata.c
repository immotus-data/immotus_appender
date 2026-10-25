/*-------------------------------------------------------------------------
 *
 * appender_metadata.c
 *    Segment manifest of appender tables.
 *
 * appender_meta is an ordinary heap table.  It is modified with the heap AM
 * directly (bypassing the guard trigger) and its indexes are maintained here.
 * Every lookup is an index scan on (spcoid, relfilenode, segno), so its cost
 * does not grow with the number of appender tables.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "appender_datafile.h"
#include "appender_metadata.h"
#include "catalog/index.h"
#include "commands/extension.h"
#include "executor/tuptable.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define Natts_appender_meta				8

#define Anum_appender_meta_spcoid		1
#define Anum_appender_meta_relfilenode	2
#define Anum_appender_meta_segno		3
#define Anum_appender_meta_state		4
#define Anum_appender_meta_eof			5
#define Anum_appender_meta_rowcount		6
#define Anum_appender_meta_valdir_off	7
#define Anum_appender_meta_version		8

/* Column numbers within the appender_meta_key index. */
#define Anum_appender_meta_key_spcoid		1
#define Anum_appender_meta_key_relfilenode	2
#define Anum_appender_meta_key_segno		3

static Oid	appender_meta_oid = InvalidOid;
static Oid	appender_meta_index_oid = InvalidOid;

static void
appender_meta_lookup(void)
{
	Oid			extoid = get_extension_oid("immotus_appender", true);

	if (OidIsValid(extoid))
	{
		Oid			nsp = get_extension_schema(extoid);

		appender_meta_oid = get_relname_relid("appender_meta", nsp);
		appender_meta_index_oid = get_relname_relid("appender_meta_key", nsp);
	}
}

Oid
appender_meta_relid(bool missing_ok)
{
	if (!OidIsValid(appender_meta_oid) || !OidIsValid(appender_meta_index_oid))
		appender_meta_lookup();

	if ((!OidIsValid(appender_meta_oid) || !OidIsValid(appender_meta_index_oid)) &&
		!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("appender_meta is missing"),
				 errhint("Re-create the immotus_appender extension.")));

	return appender_meta_oid;
}

void
appender_meta_invalidate(Oid relid)
{
	if (!OidIsValid(relid) || relid == appender_meta_oid ||
		relid == appender_meta_index_oid)
	{
		appender_meta_oid = InvalidOid;
		appender_meta_index_oid = InvalidOid;
	}
}

static void
appender_meta_row(TupleTableSlot *slot, AppenderSegment *seg)
{
	slot_getallattrs(slot);
	seg->segno = DatumGetInt32(slot->tts_values[Anum_appender_meta_segno - 1]);
	seg->state = DatumGetInt16(slot->tts_values[Anum_appender_meta_state - 1]);
	seg->eof = DatumGetInt64(slot->tts_values[Anum_appender_meta_eof - 1]);
	seg->rowcount = DatumGetInt64(slot->tts_values[Anum_appender_meta_rowcount - 1]);
	seg->valdir_off = DatumGetInt64(slot->tts_values[Anum_appender_meta_valdir_off - 1]);
}

/* Index scan over a storage's rows, or one segment's row if segno > 0. */
static IndexScanDesc
appender_meta_beginscan(Relation meta, Relation index,
						const RelFileLocator *rlocator, int segno,
						Snapshot snapshot, ScanKeyData *keys)
{
	int			nkeys = 2;
	IndexScanDesc scan;

	ScanKeyInit(&keys[0], Anum_appender_meta_key_spcoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(rlocator->spcOid));
	ScanKeyInit(&keys[1], Anum_appender_meta_key_relfilenode,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rlocator->relNumber));
	if (segno > 0)
	{
		ScanKeyInit(&keys[2], Anum_appender_meta_key_segno,
					BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(segno));
		nkeys = 3;
	}

	scan = index_beginscan(meta, index, snapshot, NULL, nkeys, 0);
	index_rescan(scan, keys, nkeys, NULL, 0);
	return scan;
}

/* One segment's row visible under snapshot: its contents and/or its TID. */
static bool
appender_meta_fetch(Relation meta, const RelFileLocator *rlocator, int segno,
					Snapshot snapshot, AppenderSegment *seg, ItemPointer tid)
{
	Relation	index = index_open(appender_meta_index_oid, AccessShareLock);
	Snapshot	snap = RegisterSnapshot(snapshot);
	TupleTableSlot *slot = table_slot_create(meta, NULL);
	ScanKeyData keys[3];
	IndexScanDesc scan = appender_meta_beginscan(meta, index, rlocator, segno,
												 snap, keys);
	bool		found = index_getnext_slot(scan, ForwardScanDirection, slot);

	if (found)
	{
		if (seg)
			appender_meta_row(slot, seg);
		if (tid)
			*tid = slot->tts_tid;
	}

	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	UnregisterSnapshot(snap);
	index_close(index, AccessShareLock);
	return found;
}

/*
 * Add index entries for a heap tuple of appender_meta just inserted or
 * updated, as the executor would.  Only plain-column indexes are supported.
 */
static void
appender_meta_index_insert(Relation meta, HeapTuple tup,
						   TU_UpdateIndexes update_indexes)
{
	List	   *indexes;
	ListCell   *lc;

	if (update_indexes == TU_None)
		return;

	indexes = RelationGetIndexList(meta);
	foreach(lc, indexes)
	{
		Relation	index = index_open(lfirst_oid(lc), RowExclusiveLock);
		IndexInfo  *info = BuildIndexInfo(index);
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];

		if (info->ii_ReadyForInserts &&
			(update_indexes == TU_All || info->ii_Summarizing))
		{
			if (info->ii_Expressions != NIL || info->ii_Predicate != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("expression or partial index \"%s\" on appender_meta is not supported",
								RelationGetRelationName(index))));

			for (int i = 0; i < info->ii_NumIndexAttrs; i++)
				values[i] = heap_getattr(tup, info->ii_IndexAttrNumbers[i],
										 RelationGetDescr(meta), &isnull[i]);

			index_insert(index, values, isnull, &tup->t_self, meta,
						 index->rd_index->indisunique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false, info);
			index_insert_cleanup(index, info);
		}
		index_close(index, RowExclusiveLock);
	}
	list_free(indexes);
}

/*
 * Segments of a storage visible under snapshot, sorted by segno.  Returns the
 * count; *segs is palloc'd in the current memory context.
 */
int
appender_meta_segments(const RelFileLocator *rlocator, Snapshot snapshot,
					   AppenderSegment **segs)
{
	Relation	meta = table_open(appender_meta_relid(false), AccessShareLock);
	Relation	index = index_open(appender_meta_index_oid, AccessShareLock);
	Snapshot	snap = RegisterSnapshot(snapshot);
	TupleTableSlot *slot = table_slot_create(meta, NULL);
	ScanKeyData keys[3];
	IndexScanDesc scan = appender_meta_beginscan(meta, index, rlocator, 0,
												 snap, keys);
	int			n = 0;
	int			cap = 8;
	AppenderSegment *out = palloc(cap * sizeof(AppenderSegment));

	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		if (n == cap)
		{
			cap *= 2;
			out = repalloc(out, cap * sizeof(AppenderSegment));
		}
		appender_meta_row(slot, &out[n++]);
	}

	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	UnregisterSnapshot(snap);
	index_close(index, AccessShareLock);
	table_close(meta, AccessShareLock);

	*segs = out;
	return n;
}

bool
appender_meta_get(const RelFileLocator *rlocator, int segno, Snapshot snapshot,
				  AppenderSegment *seg)
{
	Relation	meta = table_open(appender_meta_relid(false), AccessShareLock);
	bool		found = appender_meta_fetch(meta, rlocator, segno, snapshot,
										seg, NULL);

	table_close(meta, AccessShareLock);
	return found;
}

/*
 * Insert or update a segment's row.  The caller holds the segment lock, so
 * no other transaction updates the same row concurrently; simple_heap_update
 * errors out if that is ever violated.
 */
void
appender_meta_store(const RelFileLocator *rlocator, const AppenderSegment *seg)
{
	Relation	meta = table_open(appender_meta_relid(false), RowExclusiveLock);
	ItemPointerData oldtid;
	bool		found = appender_meta_fetch(meta, rlocator, seg->segno,
										GetLatestSnapshot(), NULL, &oldtid);
	Datum		values[Natts_appender_meta];
	bool		nulls[Natts_appender_meta] = {0};
	HeapTuple	tup;
	TU_UpdateIndexes update_indexes = TU_All;

	values[Anum_appender_meta_spcoid - 1] = ObjectIdGetDatum(rlocator->spcOid);
	values[Anum_appender_meta_relfilenode - 1] = ObjectIdGetDatum(rlocator->relNumber);
	values[Anum_appender_meta_segno - 1] = Int32GetDatum(seg->segno);
	values[Anum_appender_meta_state - 1] = Int16GetDatum(seg->state);
	values[Anum_appender_meta_eof - 1] = Int64GetDatum(seg->eof);
	values[Anum_appender_meta_rowcount - 1] = Int64GetDatum(seg->rowcount);
	values[Anum_appender_meta_valdir_off - 1] = Int64GetDatum(seg->valdir_off);
	values[Anum_appender_meta_version - 1] = Int16GetDatum(APPENDER_FORMAT_VERSION);

	tup = heap_form_tuple(RelationGetDescr(meta), values, nulls);
	if (found)
		simple_heap_update(meta, &oldtid, tup, &update_indexes);
	else
		simple_heap_insert(meta, tup);
	appender_meta_index_insert(meta, tup, update_indexes);
	heap_freetuple(tup);

	table_close(meta, RowExclusiveLock);
}

/* Delete every segment row of a storage (transactional). */
void
appender_meta_delete(const RelFileLocator *rlocator)
{
	Oid			metaid = appender_meta_relid(true);
	Relation	meta;
	Relation	index;
	Snapshot	snap;
	TupleTableSlot *slot;
	ScanKeyData keys[3];
	IndexScanDesc scan;
	List	   *tids = NIL;
	ListCell   *lc;

	if (!OidIsValid(metaid) ||
		!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(metaid)) ||
		!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(appender_meta_index_oid)))
		return;

	meta = table_open(metaid, RowExclusiveLock);
	index = index_open(appender_meta_index_oid, AccessShareLock);
	snap = RegisterSnapshot(GetLatestSnapshot());
	slot = table_slot_create(meta, NULL);
	scan = appender_meta_beginscan(meta, index, rlocator, 0, snap, keys);
	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		ItemPointer tid = palloc(sizeof(ItemPointerData));

		*tid = slot->tts_tid;
		tids = lappend(tids, tid);
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	UnregisterSnapshot(snap);
	index_close(index, AccessShareLock);

	/* deletion needs no index work: dead entries are removed by vacuum */
	foreach(lc, tids)
		simple_heap_delete(meta, (ItemPointer) lfirst(lc));
	list_free_deep(tids);

	table_close(meta, RowExclusiveLock);
}
