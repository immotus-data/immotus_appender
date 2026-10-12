/*-------------------------------------------------------------------------
 *
 * appender_lifecycle.h
 *    Transactional removal of segment files and related housekeeping.
 *
 * Segment files are not known to smgr, so the core pending-delete machinery
 * never removes them.  This module keeps its own list: storage that a
 * committed DROP/TRUNCATE/rewrite leaves behind is unlinked at commit, and
 * storage created by an aborted (sub)transaction is unlinked at abort.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_LIFECYCLE_H
#define APPENDER_LIFECYCLE_H

#include "access/xact.h"
#include "nodes/parsenodes.h"
#include "storage/relfilelocator.h"

extern void appender_storage_drop_at_commit(const RelFileLocator *rlocator);
extern void appender_storage_drop_at_abort(const RelFileLocator *rlocator);
extern bool appender_storage_dropped(const RelFileLocator *rlocator);
extern bool appender_storage_pending(void);
extern void appender_storage_xact_end(bool isCommit);
extern void appender_storage_subxact_end(bool isCommit);

extern void appender_check_createdb(CreatedbStmt *stmt);

#endif							/* APPENDER_LIFECYCLE_H */
