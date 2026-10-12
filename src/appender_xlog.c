/*-------------------------------------------------------------------------
 *
 * appender_xlog.c
 *    WAL logging and redo for appender segment files.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "appender_xlog.h"

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

void
appender_xlog_unlink(const RelFileLocator *rlocator, int segno)
{
	xl_appender_seg xlrec;

	xlrec.rlocator = *rlocator;
	xlrec.segno = segno;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogInsert(RM_APPENDER_ID, XLOG_APPENDER_UNLINK);
}

void
appender_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);

	switch (info & XLOG_APPENDER_OPMASK)
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

void
appender_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);

	if ((info & XLOG_APPENDER_OPMASK) == XLOG_APPENDER_BLOCK)
	{
		xl_appender_block *xlrec = (xl_appender_block *) data;

		appendStringInfo(buf, "rel %u/%u/%u seg %d off %lld len %u",
						 xlrec->rlocator.spcOid, xlrec->rlocator.dbOid,
						 xlrec->rlocator.relNumber, xlrec->segno,
						 (long long) xlrec->offset,
						 (unsigned int) (XLogRecGetDataLen(record) - sizeof(xl_appender_block)));
	}
	else
	{
		xl_appender_seg *xlrec = (xl_appender_seg *) data;

		appendStringInfo(buf, "rel %u/%u/%u seg %d",
						 xlrec->rlocator.spcOid, xlrec->rlocator.dbOid,
						 xlrec->rlocator.relNumber, xlrec->segno);
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
	return NULL;
}
