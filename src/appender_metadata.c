/*-------------------------------------------------------------------------
 *
 * appender_metadata.c
 *    EOF metadata (appender_meta) for immotus_appender.
 *
 * The EOF of each data file is tracked in a heap table owned by the
 * extension: appender_meta (relid oid, filenum int, eof bigint), located in
 * the extension's schema.  Because it is an ordinary heap table, MVCC and
 * WAL replay work unchanged.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "appender_metadata.h"
#include "appender_datafile.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define APPENDER_META_TABLE		"appender_meta"

/* Cached OID of appender_meta. */
static Oid appender_meta_relid = InvalidOid;

static Oid
appender_meta_oid(void)
{
	if (appender_meta_relid == InvalidOid)
	{
		Oid			extoid = get_extension_oid("immotus_appender", true);

		if (OidIsValid(extoid))
		{
			Oid			nsp = get_extension_schema(extoid);

			appender_meta_relid = get_relname_relid(APPENDER_META_TABLE, nsp);
		}
	}
	return appender_meta_relid;
}

/*
 * Read the relation's EOF as visible under the given snapshot.
 * Column layout: 1 = relid oid, 2 = filenum int, 3 = eof bigint.
 */
int64
appender_get_eof(Relation rel, Snapshot snapshot)
{
	Oid			meta_relid = appender_meta_oid();
	Oid			target_relid = RelationGetRelid(rel);
	Relation	meta;
	TableScanDesc scan;
	TupleTableSlot *slot;
	int64		eof = 0;

	/* The metadata table must exist whenever the extension is installed. */
	if (!OidIsValid(meta_relid))
		elog(ERROR, "appender_meta is missing; re-create the extension");

	meta = table_open(meta_relid, AccessShareLock);

	/*
	 * HeapTupleSatisfiesMVCC asserts that the snapshot is registered (or
	 * active).  Callers may hand us an unregistered snapshot such as the
	 * secondary snapshot returned by GetLatestSnapshot(), so register it for
	 * the duration of the scan.
	 */
	snapshot = RegisterSnapshot(snapshot);
	scan = table_beginscan(meta, snapshot, 0, NULL);
	slot = table_slot_create(meta, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Oid			rowrelid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));

		if (rowrelid == target_relid)
		{
			int64		roweof = DatumGetInt64(slot_getattr(slot, 3, &isnull));

			if (roweof > eof)
				eof = roweof;	/* tolerate stale duplicate rows */
		}
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(meta, AccessShareLock);

	return eof;
}

/*
 * Insert or update the relation's EOF row.  Must run inside the inserting
 * transaction so that the change is atomic with it.
 */
void
appender_update_eof(Oid relid, int64 eof)
{
	Oid			meta_relid = appender_meta_oid();
	Oid			target_relid = relid;
	Relation	meta;
	ItemPointerData oldtid;
	bool		found = false;
	TableScanDesc scan;
	TupleTableSlot *slot;
	Snapshot	scan_snap;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tup;
	TU_UpdateIndexes update_indexes;

	/* appender_meta must exist (created by the extension SQL). */
	if (!OidIsValid(meta_relid))
		elog(ERROR, "appender_meta is missing; re-create the extension");

	meta = table_open(meta_relid, RowExclusiveLock);

	/* Find an existing row (latest snapshot, registered for the scan). */
	scan_snap = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(meta, scan_snap, 0, NULL);
	slot = table_slot_create(meta, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Oid			rowrelid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));

		if (rowrelid == target_relid)
		{
			oldtid = slot->tts_tid;
			found = true;
			break;
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(scan_snap);

	values[0] = ObjectIdGetDatum(target_relid);
	values[1] = Int32GetDatum(APPENDER_DATAFILE_FIRST_FILENUM);
	values[2] = Int64GetDatum(eof);

	tup = heap_form_tuple(RelationGetDescr(meta), values, nulls);

	if (found)
		simple_heap_update(meta, &oldtid, tup, &update_indexes);
	else
		simple_heap_insert(meta, tup);

	heap_freetuple(tup);

	/*
	 * No CommandCounterIncrement here: this is now called once per transaction
	 * at PRE_COMMIT (appender_flush_eof), so there is no "next insert in this
	 * transaction" that needs to see the row.  The commit record makes it
	 * durable.
	 */
	table_close(meta, RowExclusiveLock);
}

/*
 * Reset the relation's EOF: delete its appender_meta row so the next read sees
 * EOF 0.  Called when the relation gets a new relfilenode (TRUNCATE, VACUUM
 * FULL, ALTER TABLE SET TABLESPACE): the old row's EOF refers to the old
 * relfilenode's data file, which no longer exists.
 */
void
appender_reset_eof(Oid relid)
{
	Oid			meta_relid = appender_meta_oid();
	Relation	meta;
	Snapshot	scan_snap;
	TableScanDesc scan;
	TupleTableSlot *slot;
	ItemPointerData tid;
	bool		found = false;

	if (!OidIsValid(meta_relid))
		elog(ERROR, "appender_meta is missing; re-create the extension");

	meta = table_open(meta_relid, RowExclusiveLock);

	scan_snap = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(meta, scan_snap, 0, NULL);
	slot = table_slot_create(meta, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Oid			rowrelid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));

		if (rowrelid == relid)
		{
			tid = slot->tts_tid;
			found = true;
			break;
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(scan_snap);

	if (found)
		simple_heap_delete(meta, &tid);

	table_close(meta, RowExclusiveLock);
}
