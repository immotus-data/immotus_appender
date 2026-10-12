/*-------------------------------------------------------------------------
 *
 * appender_lifecycle.c
 *    Transactional removal of segment files and related housekeeping.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
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

static bool
appender_dir_has_segments(const char *dirpath)
{
	DIR		   *dir = AllocateDir(dirpath);
	struct dirent *de;
	bool		found = false;

	while (!found && (de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
	{
		unsigned int relnum;
		int			segno;
		int			consumed = 0;

		if (sscanf(de->d_name, "%u.dat.%d%n", &relnum, &segno, &consumed) == 2 &&
			de->d_name[consumed] == '\0')
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
	HeapTuple	tup;
	Oid			spcoid;
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

	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
	if (!HeapTupleIsValid(tup))
		return;
	spcoid = ((Form_pg_database) GETSTRUCT(tup))->dattablespace;
	ReleaseSysCache(tup);

	dirpath = GetDatabasePath(dboid, spcoid);
	if (appender_dir_has_segments(dirpath))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("template database \"%s\" contains appender tables", template),
				 errhint("Use CREATE DATABASE ... STRATEGY = FILE_COPY.")));
	pfree(dirpath);
}

PG_FUNCTION_INFO_V1(appender_gc);

/*
 * Remove segment files of the current database whose storage no longer
 * belongs to any relation, e.g. left behind by a crash between a DROP's
 * commit and its unlink.  Only the database's default tablespace is scanned.
 *
 * The directory is listed before pg_class is read: a segment file is only
 * created after its relation's pg_class row, so every file seen here either
 * has a row visible to a dirty snapshot or is an orphan.
 */
Datum
appender_gc(PG_FUNCTION_ARGS)
{
	char	   *dirpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	DIR		   *dir;
	struct dirent *de;
	typedef struct
	{
		RelFileNumber relnum;
		int			segno;
	} SegFile;
	SegFile    *files;
	int			nfiles = 0;
	int			maxfiles = 64;
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

	files = palloc(maxfiles * sizeof(SegFile));
	dir = AllocateDir(dirpath);
	while ((de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
	{
		unsigned int relnum;
		int			segno;
		int			consumed = 0;

		if (sscanf(de->d_name, "%u.dat.%d%n", &relnum, &segno, &consumed) == 2 &&
			de->d_name[consumed] == '\0')
		{
			if (nfiles == maxfiles)
			{
				maxfiles *= 2;
				files = repalloc(files, maxfiles * sizeof(SegFile));
			}
			files[nfiles].relnum = relnum;
			files[nfiles].segno = segno;
			nfiles++;
		}
	}
	FreeDir(dir);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RelFileNumber);
	ctl.entrysize = sizeof(RelFileNumber);
	ctl.hcxt = CurrentMemoryContext;
	live = hash_create("appender live relfilenodes", 1024, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	InitDirtySnapshot(dirty);
	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan(pg_class, &dirty, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if (form->reltablespace == InvalidOid ||
			form->reltablespace == MyDatabaseTableSpace)
			(void) hash_search(live, &form->relfilenode, HASH_ENTER, NULL);
	}
	table_endscan(scan);
	table_close(pg_class, AccessShareLock);

	for (int i = 0; i < nfiles; i++)
	{
		RelFileLocator rl;

		if (hash_search(live, &files[i].relnum, HASH_FIND, NULL) != NULL)
			continue;

		rl.spcOid = MyDatabaseTableSpace;
		rl.dbOid = MyDatabaseId;
		rl.relNumber = files[i].relnum;
		appender_seg_unlink(&rl, files[i].segno);
		appender_xlog_unlink(&rl, files[i].segno);
		nremoved++;
	}

	hash_destroy(live);
	pfree(files);
	pfree(dirpath);

	PG_RETURN_INT64(nremoved);
}
