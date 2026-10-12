/*-------------------------------------------------------------------------
 *
 * appender_datafile.h
 *    Definitions for the append-only data files that back appender tables.
 *
 * A data file is a flat file of variable-size blocks, 8-byte aligned.  Each
 * block is a 16-byte AppenderBlockHeader followed by datalen bytes of payload
 * (MemTuples packed back-to-back).  The file is append-only: a block, once
 * written, is never modified.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_DATAFILE_H
#define APPENDER_DATAFILE_H

#include "postgres.h"
#include "storage/fd.h"
#include "storage/relfilelocator.h"

/* Block kinds (only one for now). */
#define APPENDER_BLOCK_KIND_DATA	0

/*
 * File number of the first (and currently only) data file.  File 0 is
 * reserved so multiple data files can be added later without changing the
 * on-disk layout.
 */
#define APPENDER_DATAFILE_FIRST_FILENUM	1

/*
 * On-disk block header.  16 bytes, keeps the payload 8-byte aligned.
 */
typedef struct AppenderBlockHeader
{
	uint32		kind;		/* APPENDER_BLOCK_KIND_* */
	uint32		rowcount;	/* number of MemTuples in this block */
	uint32		datalen;	/* payload length in bytes (uncompressed) */
	uint32		reserved;	/* reserved, keep header 16 bytes */
} AppenderBlockHeader;

/* Total on-disk size of a block given its header. */
static inline off_t
appender_datafile_block_size(const AppenderBlockHeader *hdr)
{
	return sizeof(AppenderBlockHeader) + (off_t) hdr->datalen;
}

extern File appender_datafile_open(RelFileLocator *rlocator, int filenum,
								  bool create);
extern off_t appender_datafile_append(File file, const char *payload,
									 uint32 payloadlen, uint32 rowcount,
									 off_t offset);
extern void appender_datafile_read_block(File file, off_t offset,
										AppenderBlockHeader *hdr,
										char *payload);

#endif							/* APPENDER_DATAFILE_H */
