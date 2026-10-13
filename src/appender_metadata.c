/*-------------------------------------------------------------------------
 *
 * appender_metadata.c
 *    Segment manifest of appender tables.
 *
 * The manifest is maintained with the CatalogTuple* routines, which bypass
 * triggers but keep its unique index (spcoid, relfilenode, segno) up to date.
 * Every lookup is an index scan, so its cost does not grow with the number
 * of appender tables.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "appender_datafile.h"
#include "appender_metadata.h"
#include "catalog/indexing.h"
#include "commands/extension.h"
#include "utils/fmgroids.h"
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
appender_meta_row(HeapTuple tup, TupleDesc desc, AppenderSegment *seg)
{
	Datum		values[Natts_appender_meta];
	bool		nulls[Natts_appender_meta];

	heap_deform_tuple(tup, desc, values, nulls);
	seg->segno = DatumGetInt32(values[Anum_appender_meta_segno - 1]);
	seg->state = DatumGetInt16(values[Anum_appender_meta_state - 1]);
	seg->eof = DatumGetInt64(values[Anum_appender_meta_eof - 1]);
	seg->rowcount = DatumGetInt64(values[Anum_appender_meta_rowcount - 1]);
}

/* Index scan over a storage's rows, or one segment's row if segno > 0. */
static SysScanDesc
appender_meta_beginscan(Relation meta, const RelFileLocator *rlocator,
						int segno, Snapshot snapshot, ScanKeyData *keys)
{
	int			nkeys = 2;

	ScanKeyInit(&keys[0], Anum_appender_meta_spcoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(rlocator->spcOid));
	ScanKeyInit(&keys[1], Anum_appender_meta_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(rlocator->relNumber));
	if (segno > 0)
	{
		ScanKeyInit(&keys[2], Anum_appender_meta_segno, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(segno));
		nkeys = 3;
	}

	return systable_beginscan(meta, appender_meta_index_oid, true, snapshot,
							  nkeys, keys);
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
	Snapshot	snap = RegisterSnapshot(snapshot);
	ScanKeyData keys[3];
	SysScanDesc scan = appender_meta_beginscan(meta, rlocator, 0, snap, keys);
	HeapTuple	tup;
	int			n = 0;
	int			cap = 8;
	AppenderSegment *out = palloc(cap * sizeof(AppenderSegment));

	while ((tup = systable_getnext(scan)) != NULL)
	{
		if (n == cap)
		{
			cap *= 2;
			out = repalloc(out, cap * sizeof(AppenderSegment));
		}
		appender_meta_row(tup, RelationGetDescr(meta), &out[n++]);
	}

	systable_endscan(scan);
	UnregisterSnapshot(snap);
	table_close(meta, AccessShareLock);

	*segs = out;
	return n;
}

bool
appender_meta_get(const RelFileLocator *rlocator, int segno, Snapshot snapshot,
				  AppenderSegment *seg)
{
	Relation	meta = table_open(appender_meta_relid(false), AccessShareLock);
	Snapshot	snap = RegisterSnapshot(snapshot);
	ScanKeyData keys[3];
	SysScanDesc scan = appender_meta_beginscan(meta, rlocator, segno, snap, keys);
	HeapTuple	tup = systable_getnext(scan);
	bool		found = HeapTupleIsValid(tup);

	if (found)
		appender_meta_row(tup, RelationGetDescr(meta), seg);

	systable_endscan(scan);
	UnregisterSnapshot(snap);
	table_close(meta, AccessShareLock);
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
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());
	ScanKeyData keys[3];
	SysScanDesc scan = appender_meta_beginscan(meta, rlocator, seg->segno,
											   snap, keys);
	HeapTuple	old = systable_getnext(scan);
	ItemPointerData oldtid;
	bool		found = HeapTupleIsValid(old);
	Datum		values[Natts_appender_meta];
	bool		nulls[Natts_appender_meta] = {0};
	HeapTuple	tup;

	if (found)
		oldtid = old->t_self;
	systable_endscan(scan);
	UnregisterSnapshot(snap);

	values[Anum_appender_meta_spcoid - 1] = ObjectIdGetDatum(rlocator->spcOid);
	values[Anum_appender_meta_relfilenode - 1] = ObjectIdGetDatum(rlocator->relNumber);
	values[Anum_appender_meta_segno - 1] = Int32GetDatum(seg->segno);
	values[Anum_appender_meta_state - 1] = Int16GetDatum(seg->state);
	values[Anum_appender_meta_eof - 1] = Int64GetDatum(seg->eof);
	values[Anum_appender_meta_rowcount - 1] = Int64GetDatum(seg->rowcount);
	values[Anum_appender_meta_version - 1] = Int16GetDatum(APPENDER_FORMAT_VERSION);

	tup = heap_form_tuple(RelationGetDescr(meta), values, nulls);
	if (found)
		CatalogTupleUpdate(meta, &oldtid, tup);
	else
		CatalogTupleInsert(meta, tup);
	heap_freetuple(tup);

	table_close(meta, RowExclusiveLock);
}

/* Delete every segment row of a storage (transactional). */
void
appender_meta_delete(const RelFileLocator *rlocator)
{
	Oid			metaid = appender_meta_relid(true);
	Relation	meta;
	Snapshot	snap;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *tids = NIL;
	ListCell   *lc;

	if (!OidIsValid(metaid) ||
		!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(metaid)) ||
		!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(appender_meta_index_oid)))
		return;

	meta = table_open(metaid, RowExclusiveLock);
	snap = RegisterSnapshot(GetLatestSnapshot());
	scan = appender_meta_beginscan(meta, rlocator, 0, snap, keys);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		ItemPointer tid = palloc(sizeof(ItemPointerData));

		*tid = tup->t_self;
		tids = lappend(tids, tid);
	}
	systable_endscan(scan);
	UnregisterSnapshot(snap);

	foreach(lc, tids)
		CatalogTupleDelete(meta, (ItemPointer) lfirst(lc));
	list_free_deep(tids);

	table_close(meta, RowExclusiveLock);
}
