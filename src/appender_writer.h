/*-------------------------------------------------------------------------
 *
 * appender_writer.h
 *    Insert path, per-transaction writer state and own-row visibility.
 *
 * Each writing transaction owns one open segment per relation storage,
 * claimed with a transaction-scoped lock, so concurrent writers never share
 * a segment.  Rows are packed into a block buffer; blocks are written and
 * WAL-logged when full, and at commit the touched segments are fsynced and
 * their new EOFs published in appender_meta.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_WRITER_H
#define APPENDER_WRITER_H

#include "executor/tuptable.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "appender_memtuple.h"

/*
 * A segment as seen by a reader: the committed prefix visible to its
 * snapshot, plus the part this backend appended itself.  The two byte
 * ranges can be separated by rows other transactions committed after the
 * reader's snapshot was taken; those are skipped.
 */
typedef struct AppenderScanSeg
{
	int32		segno;
	int64		eof;			/* committed bytes: [DATA_START, eof) */
	uint32		rows;			/* committed rows: [0, rows) */
	int64		own_off;		/* own bytes: [own_off, own_eof) */
	int64		own_eof;
	uint32		own_first;		/* own rows: [own_first, own_first + own_rows) */
	uint32		own_rows;
} AppenderScanSeg;

extern PGDLLIMPORT int appender_segment_size_mb;

extern void appender_writer_insert(Relation rel, TupleTableSlot *slot,
								   CommandId cid);
extern void appender_writer_flush(const RelFileLocator *rlocator);
extern void appender_writer_discard(const RelFileLocator *rlocator);

extern int	appender_own_segments(const RelFileLocator *rlocator,
								  CommandId curcid, AppenderScanSeg **segs);
extern int	appender_visible_segments(const RelFileLocator *rlocator,
									  Snapshot snapshot,
									  const AppenderScanSeg *own, int nown,
									  AppenderScanSeg **segs);

extern bool appender_writer_active(void);
extern void appender_writer_pre_commit(void);
extern void appender_writer_end(bool isCommit);
extern void appender_writer_subxact_end(bool isCommit);

extern MemTupleBinding *appender_binding(Relation rel, int natts);
extern void appender_binding_forget(Oid relid);
extern void appender_deform(MemTuple mtup, MemTupleBinding *bind, int natts,
							TupleDesc desc, TupleTableSlot *slot);

#endif							/* APPENDER_WRITER_H */
