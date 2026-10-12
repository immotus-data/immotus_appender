/*-------------------------------------------------------------------------
 *
 * appender_metadata.c
 *    Segment manifest of appender tables.
 *
 * The manifest is maintained with simple_heap_insert/update/delete, which
 * bypass triggers and index maintenance; the table therefore has no indexes
 * and is read with a sequential scan.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "appender_datafile.h"
#include "appender_metadata.h"
#include "commands/extension.h"
#include "common/int.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define Natts_appender_meta				7
#define Anum_appender_meta_spcoid		1
#define Anum_appender_meta_relfilenode	2
#define Anum_appender_meta_segno		3
#define Anum_appender_meta_state		4
#define Anum_appender_meta_eof			5
#define Anum_appender_meta_rowcount		6
#define Anum_appender_meta_version		7

static Oid	appender_meta_oid = InvalidOid;

Oid
appender_meta_relid(bool missing_ok)
{
	if (!OidIsValid(appender_meta_oid))
	{
		Oid			extoid = get_extension_oid("immotus_appender", true);

		if (OidIsValid(extoid))
			appender_meta_oid = get_relname_relid("appender_meta",
												  get_extension_schema(extoid));
	}

	if (!OidIsValid(appender_meta_oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("appender_meta is missing"),
				 errhint("Re-create the immotus_appender extension.")));

	return appender_meta_oid;
}

void
appender_meta_invalidate(Oid relid)
{
	if (!OidIsValid(relid) || relid == appender_meta_oid)
		appender_meta_oid = InvalidOid;
}

static bool
appender_meta_row(HeapTuple tup, TupleDesc desc, const RelFileLocator *rlocator,
				  AppenderSegment *seg)
{
	Datum		values[Natts_appender_meta];
	bool		nulls[Natts_appender_meta];

	heap_deform_tuple(tup, desc, values, nulls);

	if (DatumGetObjectId(values[Anum_appender_meta_spcoid - 1]) != rlocator->spcOid ||
		DatumGetObjectId(values[Anum_appender_meta_relfilenode - 1]) != rlocator->relNumber)
		return false;

	seg->segno = DatumGetInt32(values[Anum_appender_meta_segno - 1]);
	seg->state = DatumGetInt16(values[Anum_appender_meta_state - 1]);
	seg->eof = DatumGetInt64(values[Anum_appender_meta_eof - 1]);
	seg->rowcount = DatumGetInt64(values[Anum_appender_meta_rowcount - 1]);
	return true;
}

static int
appender_segment_cmp(const void *a, const void *b)
{
	return pg_cmp_s32(((const AppenderSegment *) a)->segno,
					  ((const AppenderSegment *) b)->segno);
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
	TupleDesc	desc = RelationGetDescr(meta);
	Snapshot	snap = RegisterSnapshot(snapshot);
	TableScanDesc scan = table_beginscan(meta, snap, 0, NULL);
	HeapTuple	tup;
	int			n = 0;
	int			cap = 8;
	AppenderSegment *out = palloc(cap * sizeof(AppenderSegment));

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		if (n == cap)
		{
			cap *= 2;
			out = repalloc(out, cap * sizeof(AppenderSegment));
		}
		if (appender_meta_row(tup, desc, rlocator, &out[n]))
			n++;
	}

	table_endscan(scan);
	UnregisterSnapshot(snap);
	table_close(meta, AccessShareLock);

	if (n > 1)
		qsort(out, n, sizeof(AppenderSegment), appender_segment_cmp);

	*segs = out;
	return n;
}

bool
appender_meta_get(const RelFileLocator *rlocator, int segno, Snapshot snapshot,
				  AppenderSegment *seg)
{
	AppenderSegment *segs;
	int			n = appender_meta_segments(rlocator, snapshot, &segs);
	bool		found = false;

	for (int i = 0; i < n; i++)
	{
		if (segs[i].segno == segno)
		{
			*seg = segs[i];
			found = true;
			break;
		}
	}
	pfree(segs);
	return found;
}

/*
 * Insert or update a segment's row.  The caller holds the segment lock, so
 * no other transaction updates the same row concurrently.
 */
void
appender_meta_store(const RelFileLocator *rlocator, const AppenderSegment *seg)
{
	Relation	meta = table_open(appender_meta_relid(false), RowExclusiveLock);
	TupleDesc	desc = RelationGetDescr(meta);
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(meta, snap, 0, NULL);
	HeapTuple	tup;
	ItemPointerData oldtid;
	bool		found = false;
	Datum		values[Natts_appender_meta];
	bool		nulls[Natts_appender_meta] = {0};

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		AppenderSegment row;

		if (appender_meta_row(tup, desc, rlocator, &row) && row.segno == seg->segno)
		{
			oldtid = tup->t_self;
			found = true;
			break;
		}
	}
	table_endscan(scan);
	UnregisterSnapshot(snap);

	values[Anum_appender_meta_spcoid - 1] = ObjectIdGetDatum(rlocator->spcOid);
	values[Anum_appender_meta_relfilenode - 1] = ObjectIdGetDatum(rlocator->relNumber);
	values[Anum_appender_meta_segno - 1] = Int32GetDatum(seg->segno);
	values[Anum_appender_meta_state - 1] = Int16GetDatum(seg->state);
	values[Anum_appender_meta_eof - 1] = Int64GetDatum(seg->eof);
	values[Anum_appender_meta_rowcount - 1] = Int64GetDatum(seg->rowcount);
	values[Anum_appender_meta_version - 1] = Int16GetDatum(APPENDER_FORMAT_VERSION);

	tup = heap_form_tuple(desc, values, nulls);
	if (found)
	{
		TU_UpdateIndexes update_indexes;

		simple_heap_update(meta, &oldtid, tup, &update_indexes);
	}
	else
		simple_heap_insert(meta, tup);
	heap_freetuple(tup);

	table_close(meta, RowExclusiveLock);
}

/* Delete every segment row of a storage (transactional). */
void
appender_meta_delete(const RelFileLocator *rlocator)
{
	Oid			metaid = appender_meta_relid(true);
	Relation	meta;
	TupleDesc	desc;
	Snapshot	snap;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *tids = NIL;
	ListCell   *lc;

	if (!OidIsValid(metaid) ||
		!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(metaid)))
		return;

	meta = table_open(metaid, RowExclusiveLock);
	desc = RelationGetDescr(meta);
	snap = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(meta, snap, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		AppenderSegment row;

		if (appender_meta_row(tup, desc, rlocator, &row))
		{
			ItemPointer tid = palloc(sizeof(ItemPointerData));

			*tid = tup->t_self;
			tids = lappend(tids, tid);
		}
	}
	table_endscan(scan);
	UnregisterSnapshot(snap);

	foreach(lc, tids)
		simple_heap_delete(meta, (ItemPointer) lfirst(lc));
	list_free_deep(tids);

	table_close(meta, RowExclusiveLock);
}
