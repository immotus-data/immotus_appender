/*-------------------------------------------------------------------------
 *
 * appender_tid.h
 *    Logical tuple identifiers for appender tables.
 *
 * A TID names a row by (segment number, row number within the segment) and
 * does not depend on where the row physically lives.  The encoding keeps the
 * offset number at or below 256 so that TIDs fit in a TIDBitmap page entry:
 *
 *    BlockNumber  = segno << 16 | rownum >> 8
 *    OffsetNumber = (rownum & 0xFF) + 1
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_TID_H
#define APPENDER_TID_H

#include "storage/itemptr.h"

#define APPENDER_FIRST_SEGNO		1
#define APPENDER_MAX_SEGNO			65534
#define APPENDER_ROWS_PER_SEGMENT	((uint32) 1 << 24)

/* The TID encoding stores the segment number in 16 bits and the row in 24. */
StaticAssertDecl(APPENDER_MAX_SEGNO <= 0xFFFF,
				 "segment number must fit in 16 TID bits");
StaticAssertDecl(APPENDER_ROWS_PER_SEGMENT <= (1U << 24),
				 "row number must fit in 24 TID bits");

static inline void
appender_tid_set(ItemPointer tid, int segno, uint32 rownum)
{
	Assert(segno >= APPENDER_FIRST_SEGNO && segno <= APPENDER_MAX_SEGNO);
	Assert(rownum < APPENDER_ROWS_PER_SEGMENT);

	ItemPointerSet(tid,
				   ((BlockNumber) segno << 16) | (rownum >> 8),
				   (OffsetNumber) ((rownum & 0xFF) + 1));
}

static inline int
appender_tid_segno(ItemPointer tid)
{
	return (int) (ItemPointerGetBlockNumberNoCheck(tid) >> 16);
}

static inline uint32
appender_tid_rownum(ItemPointer tid)
{
	return ((ItemPointerGetBlockNumberNoCheck(tid) & 0xFFFF) << 8) |
		(uint32) (ItemPointerGetOffsetNumberNoCheck(tid) - 1);
}

static inline bool
appender_tid_is_valid(ItemPointer tid)
{
	int			segno = appender_tid_segno(tid);

	return ItemPointerGetOffsetNumberNoCheck(tid) >= 1 &&
		ItemPointerGetOffsetNumberNoCheck(tid) <= 256 &&
		segno >= APPENDER_FIRST_SEGNO && segno <= APPENDER_MAX_SEGNO;
}

#endif							/* APPENDER_TID_H */
