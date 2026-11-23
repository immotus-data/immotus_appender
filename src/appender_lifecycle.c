/*-------------------------------------------------------------------------
 *
 * appender_lifecycle.c
 *    Transactional removal of segment files and related housekeeping.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "appender_compat.h"
#include "appender_datafile.h"
#include "appender_lifecycle.h"
#include "appender_xlog.h"

typedef struct AppenderPendingDrop
{
	RelFileLocator rlocator;
	bool		atCommit;
	int			nestLevel;
	struct AppenderPendingDrop *next;
} AppenderPendingDrop;

/* One segment file found by appender_gc. */
typedef struct SegFile
{
	Oid			spcoid;
	RelFileNumber relnum;
	int			segno;
} SegFile;

/* Storage that a segment file may still belong to. */
typedef struct SegKey
{
	Oid			spcoid;
	RelFileNumber relnum;
} SegKey;

static AppenderPendingDrop *appender_pending = NULL;

static void
appender_storage_push(const RelFileLocator *rlocator, bool atCommit)
{
	AppenderPendingDrop *p = MemoryContextAlloc(TopMemoryContext,
												sizeof(AppenderPendingDrop));

	p->rlocator = *rlocator;
	p->atCommit = atCommit;
	p->nestLevel = GetCurrentTransactionNestLevel();
	p->next = appender_pending;
	appender_pending = p;
}

void
appender_storage_drop_at_commit(const RelFileLocator *rlocator)
{
	appender_storage_push(rlocator, true);
}

void
appender_storage_drop_at_abort(const RelFileLocator *rlocator)
{
	appender_storage_push(rlocator, false);
}

/* Will this storage be removed if the transaction commits? */
bool
appender_storage_dropped(const RelFileLocator *rlocator)
{
	for (AppenderPendingDrop *p = appender_pending; p; p = p->next)
	{
		if (p->atCommit && RelFileLocatorEquals(p->rlocator, *rlocator))
			return true;
	}
	return false;
}

bool
appender_storage_pending(void)
{
	return appender_pending != NULL;
}

static void
appender_storage_unlink(const RelFileLocator *rlocator)
{
	if (appender_seg_unlink_all(rlocator) > 0)
		appender_xlog_unlink(rlocator, 0);
}

void
appender_storage_xact_end(bool isCommit)
{
	AppenderPendingDrop *p = appender_pending;

	appender_pending = NULL;
	while (p)
	{
		AppenderPendingDrop *next = p->next;

		if (p->atCommit == isCommit)
			appender_storage_unlink(&p->rlocator);
		pfree(p);
		p = next;
	}
}

void
appender_storage_subxact_end(bool isCommit)
{
	int			level = GetCurrentTransactionNestLevel();
	AppenderPendingDrop **link = &appender_pending;

	while (*link)
	{
		AppenderPendingDrop *p = *link;

		if (p->nestLevel < level)
		{
			link = &p->next;
			continue;
		}

		if (isCommit)
		{
			p->nestLevel = level - 1;
			link = &p->next;
			continue;
		}

		if (!p->atCommit)
			appender_storage_unlink(&p->rlocator);
		*link = p->next;
		pfree(p);
	}
}

/*
 * Open a directory for reading, or NULL when it does not exist.
 */
static DIR *
appender_open_dir(const char *dirpath)
{
	struct stat st;

	if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;

	return AllocateDir(dirpath);
}

/*
 * Tablespaces that may hold files of a database: its own default one plus
 * every tablespace of the cluster.  A tablespace the database never used has
 * no directory for it, which the callers tolerate.
 */
static List *
appender_tablespaces(Oid dboid)
{
	List	   *result = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	dbtup;

	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
	if (HeapTupleIsValid(dbtup))
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(dbtup);

		result = lappend_oid(result, dbform->dattablespace);
		ReleaseSysCache(dbtup);
	}

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spcoid = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		/* the cluster-wide catalog directory holds no segments */
		if (spcoid == GLOBALTABLESPACE_OID || list_member_oid(result, spcoid))
			continue;

		result = lappend_oid(result, spcoid);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

static bool
appender_dir_has_segments(const char *dirpath)
{
	DIR		   *dir = appender_open_dir(dirpath);
	struct dirent *de;
	bool		found = false;

	if (dir == NULL)
		return false;

	while (!found && (de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
	{
		RelFileNumber relnum;
		int			segno;

		if (appender_seg_parse_name(de->d_name, &relnum, &segno))
			found = true;
	}
	FreeDir(dir);
	return found;
}

/*
 * CREATE DATABASE with the WAL_LOG strategy copies relations block by block
 * through the buffer manager and would silently leave segment files behind.
 */
void
appender_check_createdb(CreatedbStmt *stmt)
{
	const char *template = "template1";
	bool		file_copy = false;
	ListCell   *lc;
	Oid			dboid;
	char	   *dirpath;

	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "template") == 0 && defel->arg)
			template = defGetString(defel);
		else if (strcmp(defel->defname, "strategy") == 0 && defel->arg)
			file_copy = pg_strcasecmp(defGetString(defel), "file_copy") == 0;
	}

	if (file_copy)
		return;

	dboid = get_database_oid(template, true);
	if (!OidIsValid(dboid))
		return;

	foreach(lc, appender_tablespaces(dboid))
	{
		dirpath = GetDatabasePath(dboid, lfirst_oid(lc));
		if (appender_dir_has_segments(dirpath))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("template database \"%s\" contains appender tables", template),
					 errhint("Use CREATE DATABASE ... STRATEGY = FILE_COPY.")));
		pfree(dirpath);
	}
}

PG_FUNCTION_INFO_V1(appender_gc);

/*
 * Remove segment files of the current database whose storage no longer
 * belongs to any relation, e.g. left behind by a crash between a DROP's
 * commit and its unlink.  Every tablespace of the database is scanned.
 *
 * The directories are listed before pg_class is read: a segment file is only
 * created after its relation's pg_class row, so every file seen here either
 * has a row visible to a dirty snapshot or is an orphan.
 */
Datum
appender_gc(PG_FUNCTION_ARGS)
{
	List	   *spaces;
	ListCell   *lc;
	SegFile    *files = NULL;
	int			nfiles = 0;
	int			maxfiles = 0;
	HASHCTL		ctl;
	HTAB	   *live;
	Relation	pg_class;
	TableScanDesc scan;
	HeapTuple	tup;
	SnapshotData dirty;
	int64		nremoved = 0;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to run appender_gc()")));

	spaces = appender_tablespaces(MyDatabaseId);

	foreach(lc, spaces)
	{
		Oid			spcoid = lfirst_oid(lc);
		char	   *dirpath = GetDatabasePath(MyDatabaseId, spcoid);
		DIR		   *dir;
		struct dirent *de;

		dir = appender_open_dir(dirpath);
		if (dir != NULL)
		{
			while ((de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
			{
				RelFileNumber relnum;
				int			segno;

				if (!appender_seg_parse_name(de->d_name, &relnum, &segno))
					continue;

				if (nfiles == maxfiles)
				{
					maxfiles = maxfiles > 0 ? maxfiles * 2 : 64;
					files = files != NULL ?
						repalloc(files, maxfiles * sizeof(SegFile)) :
						palloc(maxfiles * sizeof(SegFile));
				}
				files[nfiles].spcoid = spcoid;
				files[nfiles].relnum = relnum;
				files[nfiles].segno = segno;
				nfiles++;
			}
			FreeDir(dir);
		}
		pfree(dirpath);
	}

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(SegKey);
	ctl.entrysize = sizeof(SegKey);
	ctl.hcxt = CurrentMemoryContext;
	live = hash_create("appender live segments", 1024, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	InitDirtySnapshot(dirty);
	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = appender_table_beginscan(pg_class, &dirty, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);
		SegKey		key;

		key.spcoid = OidIsValid(form->reltablespace) ?
			form->reltablespace : MyDatabaseTableSpace;
		key.relnum = form->relfilenode;
		(void) hash_search(live, &key, HASH_ENTER, NULL);
	}
	table_endscan(scan);
	table_close(pg_class, AccessShareLock);

	for (int i = 0; i < nfiles; i++)
	{
		SegKey		key;
		RelFileLocator rl;

		key.spcoid = files[i].spcoid;
		key.relnum = files[i].relnum;
		if (hash_search(live, &key, HASH_FIND, NULL) != NULL)
			continue;

		rl.spcOid = files[i].spcoid;
		rl.dbOid = MyDatabaseId;
		rl.relNumber = files[i].relnum;
		if (!appender_seg_unlink(&rl, files[i].segno))
			continue;

		appender_xlog_unlink(&rl, files[i].segno);
		nremoved++;
	}

	hash_destroy(live);
	if (files != NULL)
		pfree(files);
	list_free(spaces);

	PG_RETURN_INT64(nremoved);
}
