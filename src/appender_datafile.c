/*-------------------------------------------------------------------------
 *
 * appender_datafile.c
 *    Access to the append-only data files that back appender tables.
 *
 * Data files bypass the shared buffer manager and are accessed directly
 * through fd.c (FileRead/FileWrite).  Blocks are variable-size and are
 * never updated in place; durability (fsync and EOF metadata) is handled
 * by the caller.  This file only does I/O.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "appender_datafile.h"
#include "common/relpath.h"
#include "utils/wait_event.h"

/*
 * Open (and optionally create) a data file <relfilenumber>.dat.<filenum> for
 * the given relation locator.  Returns a File handle; caller must FileClose.
 */
File
appender_datafile_open(RelFileLocator *rlocator, int filenum, bool create)
{
	RelPathStr	base = relpathperm(*rlocator, MAIN_FORKNUM);
	char		path[MAXPGPATH];
	int			flags = O_RDWR | PG_BINARY;
	File		file;

	/*
	 * Use an ".dat.<filenum>" suffix, not "<relfilenumber>.<filenum>": the latter
	 * is the main-fork segment numbering that md.c / dbsize.c treat specially
	 * (pg_relation_size would count our datafile as a heap segment, mdunlink
	 * would delete it on DROP, mdtruncate on TRUNCATE).
	 */
	snprintf(path, sizeof(path), "%s.dat.%d", base.str, filenum);

	if (create)
		flags |= O_CREAT;

	file = PathNameOpenFile(path, flags);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open append-only data file \"%s\": %m",
						path)));

	return file;
}

/*
 * Append one data block at the given byte offset.
 *
 * offset is the logical append point: the committed EOF, not necessarily
 * the physical end of file.  Appending at the committed EOF (rather than
 * FileSize) makes an aborted insert's tail get overwritten by the next
 * committed insert, so aborted data can never become visible later.
 *
 * Returns the byte offset of the block start (== offset).
 */
off_t
appender_datafile_append(File file, const char *payload, uint32 payloadlen,
						uint32 rowcount, off_t offset)
{
	AppenderBlockHeader hdr;
	ssize_t		written;

	hdr.kind = APPENDER_BLOCK_KIND_DATA;
	hdr.rowcount = rowcount;
	hdr.datalen = payloadlen;
	hdr.reserved = 0;

	written = FileWrite(file, &hdr, sizeof(hdr), offset,
						WAIT_EVENT_DATA_FILE_WRITE);
	if (written != sizeof(hdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write append-only block header: %m")));

	offset += sizeof(hdr);

	if (payloadlen > 0)
	{
		written = FileWrite(file, payload, payloadlen, offset,
							WAIT_EVENT_DATA_FILE_WRITE);
		if (written != payloadlen)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write append-only block payload: %m")));
	}

	return offset - sizeof(hdr);
}

/*
 * Read a block's header and payload from the given byte offset.
 *
 * The caller supplies a payload buffer large enough for hdr->datalen bytes.
 */
void
appender_datafile_read_block(File file, off_t offset, AppenderBlockHeader *hdr,
							char *payload)
{
	ssize_t		nread;

	nread = FileRead(file, hdr, sizeof(*hdr), offset,
					 WAIT_EVENT_DATA_FILE_READ);
	if (nread != sizeof(*hdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read append-only block header: %m")));

	if (hdr->datalen > 0)
	{
		nread = FileRead(file, payload, hdr->datalen, offset + sizeof(*hdr),
						 WAIT_EVENT_DATA_FILE_READ);
		if (nread != hdr->datalen)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read append-only block payload: %m")));
	}
}
