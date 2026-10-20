/*-------------------------------------------------------------------------
 *
 * appender_datafile.h
 *    On-disk format of appender segment files.
 *
 * Each relation storage (relfilenode) owns segment files named
 * <relfilenode>.dat.<segno>.  A segment file is a 32-byte file header
 * followed by variable-size blocks.  A block is a 32-byte header followed by
 * a payload of MemTuples packed back-to-back.  Blocks are written once and
 * never modified; a sealed segment is immutable.
 *
 * Only this module touches segment files, so another storage backend can be
 * put behind it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_DATAFILE_H
#define APPENDER_DATAFILE_H

#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/relfilelocator.h"

#define APPENDER_FILE_MAGIC			0x41505044	/* "APPD" */
#define APPENDER_FORMAT_VERSION		1

#define APPENDER_BLOCK_KIND_DATA	1
#define APPENDER_CODEC_NONE			0

typedef struct AppenderFileHeader
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		segno;
	uint32		reserved[5];
} AppenderFileHeader;

typedef struct AppenderBlockHeader
{
	pg_crc32c	crc;			/* CRC-32C of the header (crc = 0) and payload */
	uint16		version;
	uint16		kind;
	uint32		rowcount;
	uint32		first_rownum;	/* segment-relative number of the first row */
	uint32		datalen;		/* stored payload bytes */
	uint32		rawlen;			/* payload bytes before compression */
	uint16		natts;			/* attributes the rows were formed with */
	uint16		codec;
	uint32		reserved;
} AppenderBlockHeader;

StaticAssertDecl(sizeof(AppenderFileHeader) == 32, "file header must be 32 bytes");
StaticAssertDecl(sizeof(AppenderBlockHeader) == 32, "block header must be 32 bytes");

#define APPENDER_SEG_DATA_START		((int64) sizeof(AppenderFileHeader))

static inline int64
appender_block_size(const AppenderBlockHeader *hdr)
{
	return (int64) sizeof(AppenderBlockHeader) + hdr->datalen;
}

extern void appender_seg_path(const RelFileLocator *rlocator, int segno,
							  char *path);
extern File appender_seg_create(const RelFileLocator *rlocator, int segno);
extern File appender_seg_open(const RelFileLocator *rlocator, int segno);
extern void appender_seg_write_block(File file, int64 offset,
									 AppenderBlockHeader *hdr,
									 const char *payload);
extern void appender_seg_read_header(File file, int64 offset,
									 AppenderBlockHeader *hdr);
extern void appender_seg_read_payload(File file, int64 offset,
									  const AppenderBlockHeader *hdr,
									  char *payload);
extern void appender_seg_unlink(const RelFileLocator *rlocator, int segno);
extern int	appender_seg_unlink_all(const RelFileLocator *rlocator);
extern int64 appender_seg_disk_size(const RelFileLocator *rlocator, int segno);

/* Redo-side helpers: no File cache, no relcache, tolerate missing files. */
extern void appender_seg_redo_create(const RelFileLocator *rlocator, int segno);
extern void appender_seg_redo_write(const RelFileLocator *rlocator, int segno,
									int64 offset, const char *data, Size len);
extern void appender_seg_redo_sync(const RelFileLocator *rlocator, int segno);

/* Zero-fill past 'end'; commits inside the region fdatasync cheaply. */
extern PGDLLIMPORT int appender_prealloc_chunk_kb;
extern void appender_seg_ensure_room(File file, int64 end, int64 *horizon);
extern void appender_seg_sync(File file);

#endif							/* APPENDER_DATAFILE_H */
