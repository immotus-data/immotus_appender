/*-------------------------------------------------------------------------
 *
 * appender_xlog.c
 *    WAL logging and redo for appender segment files.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "appender_xlog.h"

bool		appender_debug_log = false;

void
appender_xlog_create(const RelFileLocator *rlocator, int segno)
{
	xl_appender_seg xlrec;

	xlrec.rlocator = *rlocator;
	xlrec.segno = segno;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogInsert(RM_APPENDER_ID, XLOG_APPENDER_CREATE);
}

void
appender_xlog_block(const RelFileLocator *rlocator, int segno, int64 offset,
					const AppenderBlockHeader *hdr, const char *payload)
{
	xl_appender_block xlrec;

	xlrec.rlocator = *rlocator;
	xlrec.segno = segno;
	xlrec.offset = offset;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterData(hdr, sizeof(*hdr));
	if (hdr->datalen > 0)
		XLogRegisterData(payload, hdr->datalen);
	XLogInsert(RM_APPENDER_ID, XLOG_APPENDER_BLOCK);
}

void
appender_xlog_sync(const RelFileLocator *rlocator, int segno)
{
	xl_appender_seg xlrec;

	xlrec.rlocator = *rlocator;
	xlrec.segno = segno;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogInsert(RM_APPENDER_ID, XLOG_APPENDER_SYNC);
}

/*
 * Flushed at once: written after the commit record, so without the flush a
 * crash could lose it and redo would recreate the files from earlier records.
 */
void
appender_xlog_unlink(const RelFileLocator *rlocator, int segno)
{
	xl_appender_seg xlrec;

	xlrec.rlocator = *rlocator;
	xlrec.segno = segno;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogFlush(XLogInsert(RM_APPENDER_ID, XLOG_APPENDER_UNLINK));
}

void
appender_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);
	uint8		op = info & XLOG_APPENDER_OPMASK;

	/*
	 * After a local crash the segment files already hold every committed
	 * byte: dirty segments are synced before the commit record, so CREATE,
	 * BLOCK and SYNC records need no replay there.  Single-segment UNLINK
	 * records are only ever emitted for segments that never committed (see
	 * appender_unlink_new_segs), and their segment number can later be reused
	 * by a committed segment; replaying one could delete that committed file,
	 * so skip those too.  A whole-storage UNLINK (segno 0) must always
	 * replay: it is flushed after the commit record, so without it a crash
	 * could leave orphan files.  Standbys and archive recovery set
	 * ArchiveRecoveryRequested, so every record still replays there.
	 */
	if (InRecovery && !ArchiveRecoveryRequested)
	{
		bool		must_replay = false;

		if (op == XLOG_APPENDER_UNLINK &&
			((xl_appender_seg *) data)->segno == 0)
			must_replay = true;

		if (!must_replay)
		{
			if (appender_debug_log)
				elog(DEBUG1, "appender: skip %s redo in crash recovery",
					 appender_identify(info));
			return;
		}
	}

	if (appender_debug_log)
		elog(DEBUG1, "appender: redo %s", appender_identify(info));

	switch (op)
	{
		case XLOG_APPENDER_CREATE:
			{
				xl_appender_seg *xlrec = (xl_appender_seg *) data;

				appender_seg_redo_create(&xlrec->rlocator, xlrec->segno);
				break;
			}
		case XLOG_APPENDER_BLOCK:
			{
				xl_appender_block *xlrec = (xl_appender_block *) data;

				appender_seg_redo_write(&xlrec->rlocator, xlrec->segno,
										xlrec->offset,
										data + sizeof(xl_appender_block),
										XLogRecGetDataLen(record) - sizeof(xl_appender_block));
				break;
			}
		case XLOG_APPENDER_SYNC:
			{
				xl_appender_seg *xlrec = (xl_appender_seg *) data;

				appender_seg_redo_sync(&xlrec->rlocator, xlrec->segno);
				break;
			}
		case XLOG_APPENDER_UNLINK:
			{
				xl_appender_seg *xlrec = (xl_appender_seg *) data;

				if (xlrec->segno == 0)
					appender_seg_unlink_all(&xlrec->rlocator);
				else
					appender_seg_unlink(&xlrec->rlocator, xlrec->segno);
				break;
			}
		default:
			elog(PANIC, "appender_redo: unknown op code %u", info);
	}
}

static void
appender_desc_rlocator(StringInfo buf, const RelFileLocator *rlocator, int segno)
{
	appendStringInfo(buf, "rel %u/%u/%u seg %d",
					 rlocator->spcOid, rlocator->dbOid,
					 rlocator->relNumber, segno);
}

void
appender_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);

	if ((info & XLOG_APPENDER_OPMASK) == XLOG_APPENDER_BLOCK)
	{
		xl_appender_block *xlrec = (xl_appender_block *) data;

		appender_desc_rlocator(buf, &xlrec->rlocator, xlrec->segno);
		appendStringInfo(buf, " off " INT64_FORMAT " len %u",
						 (int64) xlrec->offset,
						 (unsigned int) (XLogRecGetDataLen(record) - sizeof(xl_appender_block)));
	}
	else
	{
		xl_appender_seg *xlrec = (xl_appender_seg *) data;

		appender_desc_rlocator(buf, &xlrec->rlocator, xlrec->segno);
	}
}

const char *
appender_identify(uint8 info)
{
	switch (info & XLOG_APPENDER_OPMASK)
	{
		case XLOG_APPENDER_CREATE:
			return "CREATE";
		case XLOG_APPENDER_BLOCK:
			return "BLOCK";
		case XLOG_APPENDER_SYNC:
			return "SYNC";
		case XLOG_APPENDER_UNLINK:
			return "UNLINK";
	}
	return "UNKNOWN";
}
