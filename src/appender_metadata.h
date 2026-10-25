/*-------------------------------------------------------------------------
 *
 * appender_metadata.h
 *    Segment manifest of appender tables.
 *
 * appender_meta holds one row per segment of a relation storage, keyed by
 * (tablespace, relfilenode, segno).  It is an ordinary heap table, so a
 * reader sees the segment EOFs committed as of its snapshot, and the rows
 * follow the storage through TRUNCATE and table rewrites.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_METADATA_H
#define APPENDER_METADATA_H

#include "storage/relfilelocator.h"
#include "utils/snapshot.h"

#define APPENDER_SEG_OPEN		0
#define APPENDER_SEG_SEALED		1

typedef struct AppenderSegment
{
	int32		segno;
	int16		state;
	int64		eof;			/* committed logical end of the segment file */
	int64		rowcount;		/* committed rows in the segment */
	int64		valdir_off;		/* committed offset of the value directory */
	int16		format_version;	/* format of the blocks written to it */
} AppenderSegment;

extern Oid	appender_meta_relid(bool missing_ok);
extern void appender_meta_invalidate(Oid relid);
extern int	appender_meta_segments(const RelFileLocator *rlocator,
								   Snapshot snapshot, AppenderSegment **segs);
extern int	appender_meta_open_segments(const RelFileLocator *rlocator,
									 Snapshot snapshot, AppenderSegment **segs);
extern int	appender_meta_max_segno(const RelFileLocator *rlocator,
							 Snapshot snapshot);
extern bool appender_meta_get(const RelFileLocator *rlocator, int segno,
							  Snapshot snapshot, AppenderSegment *seg);
extern void appender_meta_store(const RelFileLocator *rlocator,
								const AppenderSegment *seg);
extern void appender_meta_delete(const RelFileLocator *rlocator);

#endif							/* APPENDER_METADATA_H */
