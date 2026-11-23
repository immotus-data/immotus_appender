/*-------------------------------------------------------------------------
 *
 * appender_xlog.h
 *    WAL records of the appender resource manager.
 *
 * Segment files live outside the buffer manager, so every change to them is
 * logged here: segment creation, whole blocks, commit-time fsyncs (so a
 * standby syncs at the same points as the primary) and unlinks.  Redo is
 * idempotent: replaying a record twice leaves the same file contents.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_XLOG_H
#define APPENDER_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

#include "appender_datafile.h"

/*
 * Reserved for immotus_appender on the PostgreSQL wiki:
 * https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 */
#define RM_APPENDER_ID				152

#define XLOG_APPENDER_CREATE		0x00
#define XLOG_APPENDER_BLOCK			0x10
#define XLOG_APPENDER_SYNC			0x20
#define XLOG_APPENDER_UNLINK		0x30
#define XLOG_APPENDER_OPMASK		0x70

typedef struct xl_appender_seg
{
	RelFileLocator rlocator;
	int32		segno;			/* 0 in UNLINK means every segment */
} xl_appender_seg;

typedef struct xl_appender_block
{
	RelFileLocator rlocator;
	int32		segno;
	int64		offset;
	/* block header and payload follow */
} xl_appender_block;

/* Emit a DEBUG1 line for every replayed or skipped record (tests). */
extern bool appender_debug_log;

extern void appender_xlog_create(const RelFileLocator *rlocator, int segno);
extern void appender_xlog_block(const RelFileLocator *rlocator, int segno,
								int64 offset, const AppenderBlockHeader *hdr,
								const char *payload);
extern void appender_xlog_sync(const RelFileLocator *rlocator, int segno);
extern void appender_xlog_unlink(const RelFileLocator *rlocator, int segno);

extern void appender_redo(XLogReaderState *record);
extern void appender_desc(StringInfo buf, XLogReaderState *record);
extern const char *appender_identify(uint8 info);

#endif							/* APPENDER_XLOG_H */
