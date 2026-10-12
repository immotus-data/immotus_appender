/*-------------------------------------------------------------------------
 *
 * appender_datafile.c
 *    I/O on appender segment files.
 *
 * Segment files bypass the shared buffer manager and are accessed through
 * fd.c.  Durability (fsync ordering against the EOF commit point) and WAL
 * logging are the caller's business; this module only reads, writes and
 * validates the on-disk format.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "appender_datafile.h"
#include "common/relpath.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

static pg_crc32c
appender_block_crc(const AppenderBlockHeader *hdr, const char *payload)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (const char *) hdr + sizeof(pg_crc32c),
				sizeof(AppenderBlockHeader) - sizeof(pg_crc32c));
	if (hdr->datalen > 0)
		COMP_CRC32C(crc, payload, hdr->datalen);
	FIN_CRC32C(crc);
	return crc;
}

void
appender_seg_path(const RelFileLocator *rlocator, int segno, char *path)
{
	RelPathStr	base = relpathperm(*rlocator, MAIN_FORKNUM);

	snprintf(path, MAXPGPATH, "%s.dat.%d", base.str, segno);
}

static void
appender_write_file_header(File file, int segno)
{
	AppenderFileHeader fhdr;

	memset(&fhdr, 0, sizeof(fhdr));
	fhdr.magic = APPENDER_FILE_MAGIC;
	fhdr.version = APPENDER_FORMAT_VERSION;
	fhdr.segno = segno;

	if (FileWrite(file, &fhdr, sizeof(fhdr), 0,
				  WAIT_EVENT_DATA_FILE_WRITE) != sizeof(fhdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write header of file \"%s\": %m",
						FilePathName(file))));
}

/*
 * Create (or reset) a segment file and write its header.  A stale file left
 * by an aborted or crashed writer is truncated.
 */
File
appender_seg_create(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];
	File		file;

	appender_seg_path(rlocator, segno, path);
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	appender_write_file_header(file, segno);
	return file;
}

File
appender_seg_open(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];
	File		file;

	appender_seg_path(rlocator, segno, path);
	file = PathNameOpenFile(path, O_RDWR | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	return file;
}

/* Fill in the crc and write one block at offset. */
void
appender_seg_write_block(File file, int64 offset, AppenderBlockHeader *hdr,
						 const char *payload)
{
	hdr->crc = appender_block_crc(hdr, payload);

	if (FileWrite(file, hdr, sizeof(*hdr), offset,
				  WAIT_EVENT_DATA_FILE_WRITE) != sizeof(*hdr) ||
		(hdr->datalen > 0 &&
		 FileWrite(file, payload, hdr->datalen, offset + sizeof(*hdr),
				   WAIT_EVENT_DATA_FILE_WRITE) != hdr->datalen))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block at offset %lld of file \"%s\": %m",
						(long long) offset, FilePathName(file))));
}

/* Read and sanity-check a block header; the payload crc is checked later. */
void
appender_seg_read_header(File file, int64 offset, AppenderBlockHeader *hdr)
{
	if (FileRead(file, hdr, sizeof(*hdr), offset,
				 WAIT_EVENT_DATA_FILE_READ) != sizeof(*hdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block header at offset %lld of file \"%s\": %m",
						(long long) offset, FilePathName(file))));

	if (hdr->version != APPENDER_FORMAT_VERSION ||
		hdr->kind != APPENDER_BLOCK_KIND_DATA ||
		hdr->codec != APPENDER_CODEC_NONE ||
		hdr->rowcount == 0 ||
		hdr->rawlen != hdr->datalen ||
		hdr->datalen % MAXIMUM_ALIGNOF != 0 ||
		!AllocSizeIsValid(hdr->datalen))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid block header at offset %lld of file \"%s\"",
						(long long) offset, FilePathName(file))));
}

void
appender_seg_read_payload(File file, int64 offset,
						  const AppenderBlockHeader *hdr, char *payload)
{
	if (hdr->datalen > 0 &&
		FileRead(file, payload, hdr->datalen, offset + sizeof(*hdr),
				 WAIT_EVENT_DATA_FILE_READ) != hdr->datalen)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block at offset %lld of file \"%s\": %m",
						(long long) offset, FilePathName(file))));

	if (!EQ_CRC32C(appender_block_crc(hdr, payload), hdr->crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block checksum mismatch at offset %lld of file \"%s\"",
						(long long) offset, FilePathName(file))));
}

void
appender_seg_sync(File file)
{
	if (FileSync(file, WAIT_EVENT_DATA_FILE_SYNC) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						FilePathName(file))));
}

void
appender_seg_unlink(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];

	appender_seg_path(rlocator, segno, path);
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
}

/* Remove every segment file of a relation storage; returns the count. */
int
appender_seg_unlink_all(const RelFileLocator *rlocator)
{
	char	   *dirpath = GetDatabasePath(rlocator->dbOid, rlocator->spcOid);
	DIR		   *dir;
	struct dirent *de;
	int			nremoved = 0;

	dir = AllocateDir(dirpath);
	while ((de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
	{
		unsigned int relnum;
		int			segno;
		int			consumed = 0;

		if (sscanf(de->d_name, "%u.dat.%d%n", &relnum, &segno, &consumed) == 2 &&
			de->d_name[consumed] == '\0' &&
			relnum == rlocator->relNumber)
		{
			appender_seg_unlink(rlocator, segno);
			nremoved++;
		}
	}
	FreeDir(dir);
	pfree(dirpath);
	return nremoved;
}

int64
appender_seg_disk_size(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];
	struct stat st;

	appender_seg_path(rlocator, segno, path);
	if (stat(path, &st) < 0)
	{
		if (errno == ENOENT)
			return 0;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}
	return (int64) st.st_size;
}

void
appender_seg_redo_create(const RelFileLocator *rlocator, int segno)
{
	File		file = appender_seg_create(rlocator, segno);

	FileClose(file);
}

void
appender_seg_redo_write(const RelFileLocator *rlocator, int segno,
						int64 offset, const char *data, Size len)
{
	char		path[MAXPGPATH];
	File		file;

	appender_seg_path(rlocator, segno, path);
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	if (FileWrite(file, data, len, offset, WAIT_EVENT_DATA_FILE_WRITE) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	FileClose(file);
}

void
appender_seg_redo_sync(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];
	File		file;

	appender_seg_path(rlocator, segno, path);
	file = PathNameOpenFile(path, O_RDWR | PG_BINARY);
	if (file < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}
	appender_seg_sync(file);
	FileClose(file);
}
