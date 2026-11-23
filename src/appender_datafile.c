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

#include "access/xlog.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "appender_datafile.h"

int			appender_prealloc_chunk_kb = 1024;

/*
 * The crc32c instruction has a latency of several cycles but a throughput of
 * one per cycle, so three independent streams over adjacent stripes run about
 * three times faster than one.  The stripe CRCs are merged by advancing them
 * over the following stripes, a linear map on 32 bits applied by table.
 */
#if !defined(WORDS_BIGENDIAN) && defined(USE_ARMV8_CRC32C)
#include <arm_acle.h>
#define APPENDER_CRC32C_U64(crc, v) __crc32cd((crc), (v))
#elif !defined(WORDS_BIGENDIAN) && defined(USE_SSE42_CRC32C)
#include <nmmintrin.h>
#define APPENDER_CRC32C_U64(crc, v) ((uint32) _mm_crc32_u64((crc), (v)))
#endif

#ifdef APPENDER_CRC32C_U64

#define APPENDER_CRC_STRIPE 1024

/* [0]: advance over one stripe, [1]: over two; per byte of the CRC */
static uint32 appender_crc_shift[2][4][256];
static bool appender_crc_shift_ready = false;

static void
appender_crc_shift_init(void)
{
	static const char zeros[2 * APPENDER_CRC_STRIPE];

	for (int s = 0; s < 2; s++)
	{
		uint32		basis[32];

		for (int k = 0; k < 32; k++)
		{
			basis[k] = (uint32) 1 << k;
			COMP_CRC32C(basis[k], zeros, (s + 1) * APPENDER_CRC_STRIPE);
		}
		for (int j = 0; j < 4; j++)
			for (int b = 0; b < 256; b++)
			{
				uint32		v = 0;

				for (int k = 0; k < 8; k++)
					if (b & (1 << k))
						v ^= basis[8 * j + k];
				appender_crc_shift[s][j][b] = v;
			}
	}
	appender_crc_shift_ready = true;
}

static inline uint32
appender_crc_advance(int s, uint32 crc)
{
	return appender_crc_shift[s][0][crc & 0xFF] ^
		appender_crc_shift[s][1][(crc >> 8) & 0xFF] ^
		appender_crc_shift[s][2][(crc >> 16) & 0xFF] ^
		appender_crc_shift[s][3][crc >> 24];
}
#endif

static pg_crc32c
appender_crc32c(pg_crc32c crc, const char *p, size_t len)
{
#ifdef APPENDER_CRC32C_U64
#ifdef USE_ASSERT_CHECKING
	pg_crc32c	expected = crc;

	COMP_CRC32C(expected, p, len);
#endif
	if (len >= 3 * APPENDER_CRC_STRIPE && !appender_crc_shift_ready)
		appender_crc_shift_init();

	while (len >= 3 * APPENDER_CRC_STRIPE)
	{
		uint32		a = crc;
		uint32		b = 0;
		uint32		c = 0;

		for (int i = 0; i < APPENDER_CRC_STRIPE; i += 8)
		{
			uint64		va;
			uint64		vb;
			uint64		vc;

			memcpy(&va, p + i, 8);
			memcpy(&vb, p + APPENDER_CRC_STRIPE + i, 8);
			memcpy(&vc, p + 2 * APPENDER_CRC_STRIPE + i, 8);
			a = APPENDER_CRC32C_U64(a, va);
			b = APPENDER_CRC32C_U64(b, vb);
			c = APPENDER_CRC32C_U64(c, vc);
		}
		crc = appender_crc_advance(1, a) ^ appender_crc_advance(0, b) ^ c;
		p += 3 * APPENDER_CRC_STRIPE;
		len -= 3 * APPENDER_CRC_STRIPE;
	}
	COMP_CRC32C(crc, p, len);
	Assert(crc == expected);
#else
	COMP_CRC32C(crc, p, len);
#endif
	return crc;
}

/* CRC of the header fields only; the payload is folded in by the caller. */
static pg_crc32c
appender_block_crc_after_header(const AppenderBlockHeader *hdr)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (const char *) hdr + sizeof(pg_crc32c),
				sizeof(AppenderBlockHeader) - sizeof(pg_crc32c));
	return crc;
}

static pg_crc32c
appender_block_crc(const AppenderBlockHeader *hdr, const char *payload)
{
	pg_crc32c	crc = appender_block_crc_after_header(hdr);

	if (hdr->datalen > 0)
		crc = appender_crc32c(crc, payload, hdr->datalen);
	FIN_CRC32C(crc);
	return crc;
}

void
appender_seg_path(const RelFileLocator *rlocator, int segno, char *path)
{
#if PG_VERSION_NUM >= 180000
	RelPathStr	base = relpathperm(*rlocator, MAIN_FORKNUM);

	snprintf(path, MAXPGPATH, "%s.dat.%d", base.str, segno);
#else
	char	   *base = relpathperm(*rlocator, MAIN_FORKNUM);

	snprintf(path, MAXPGPATH, "%s.dat.%d", base, segno);
	pfree(base);
#endif
}

/*
 * Parse a segment file name "<relfilenode>.dat.<segno>", the inverse of
 * appender_seg_path.  Returns true only when the name matches exactly.
 */
bool
appender_seg_parse_name(const char *name, RelFileNumber *relnum, int *segno)
{
	unsigned int rn;
	int			sn;
	int			consumed = 0;

	if (sscanf(name, "%u.dat.%d%n", &rn, &sn, &consumed) != 2 ||
		name[consumed] != '\0')
		return false;
	*relnum = (RelFileNumber) rn;
	*segno = sn;
	return true;
}

static void appender_seg_cache_forget(const RelFileLocator *rlocator, int segno);

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

	appender_seg_cache_forget(rlocator, segno);
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

/* The one segment file kept open for TID lookups, see appender_seg_open_cached. */
static struct
{
	RelFileLocator rlocator;
	int			segno;
	File		file;
}			appender_seg_cache = {.file = -1};

/* Bumped whenever a segment file is removed or re-created in place. */
uint64		appender_seg_generation = 0;

void
appender_seg_cache_close(void)
{
	if (appender_seg_cache.file >= 0)
		FileClose(appender_seg_cache.file);
	appender_seg_cache.file = -1;
}

/*
 * Open a segment for a read that is likely to be repeated.  The handle is
 * owned by the cache: callers must not close it.  It is dropped at
 * transaction end and whenever a segment file is unlinked or re-created.
 */
File
appender_seg_open_cached(const RelFileLocator *rlocator, int segno)
{
	if (appender_seg_cache.file >= 0 &&
		appender_seg_cache.segno == segno &&
		RelFileLocatorEquals(appender_seg_cache.rlocator, *rlocator))
		return appender_seg_cache.file;

	appender_seg_cache_close();
	appender_seg_cache.file = appender_seg_open(rlocator, segno);
	appender_seg_cache.rlocator = *rlocator;
	appender_seg_cache.segno = segno;
	return appender_seg_cache.file;
}

static void
appender_seg_cache_forget(const RelFileLocator *rlocator, int segno)
{
	appender_seg_generation++;
	if (appender_seg_cache.file >= 0 &&
		appender_seg_cache.segno == segno &&
		RelFileLocatorEquals(appender_seg_cache.rlocator, *rlocator))
		appender_seg_cache_close();
}

/*
 * Fill in the crc and write one block at offset.
 *
 * The crc is written back into *hdr on purpose: the caller logs the same
 * header to WAL right after this call, so the redo image has to carry the
 * crc that the data block on disk carries.
 */
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
				 errmsg("could not write block at offset " INT64_FORMAT " of file \"%s\": %m",
						(int64) offset, FilePathName(file))));
}

/* Read and sanity-check a block header; the payload crc is checked later. */
void
appender_seg_read_header(File file, int64 offset, AppenderBlockHeader *hdr)
{
	if (FileRead(file, hdr, sizeof(*hdr), offset,
				 WAIT_EVENT_DATA_FILE_READ) != sizeof(*hdr))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block header at offset " INT64_FORMAT " of file \"%s\": %m",
						(int64) offset, FilePathName(file))));

	if (hdr->version < APPENDER_FORMAT_VERSION_MIN ||
		hdr->version > APPENDER_FORMAT_VERSION ||
		(hdr->kind != APPENDER_BLOCK_KIND_DATA &&
		 hdr->kind != APPENDER_BLOCK_KIND_VALUE &&
		 hdr->kind != APPENDER_BLOCK_KIND_VALDIR) ||
		hdr->codec != APPENDER_CODEC_NONE ||
		hdr->rowcount == 0 ||
		!AllocSizeIsValid(hdr->datalen))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid block header at offset " INT64_FORMAT " of file \"%s\"",
						(int64) offset, FilePathName(file))));

	if (hdr->kind == APPENDER_BLOCK_KIND_DATA)
	{
		if (hdr->rawlen != hdr->datalen ||
			hdr->datalen % MAXIMUM_ALIGNOF != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid data block header at offset " INT64_FORMAT " of file \"%s\"",
							(int64) offset, FilePathName(file))));
	}
	else if (hdr->kind == APPENDER_BLOCK_KIND_VALUE &&
			 hdr->rawlen < hdr->datalen)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid value block header at offset " INT64_FORMAT " of file \"%s\"",
						(int64) offset, FilePathName(file))));
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
				 errmsg("could not read block at offset " INT64_FORMAT " of file \"%s\": %m",
						(int64) offset, FilePathName(file))));

	if (!EQ_CRC32C(appender_block_crc(hdr, payload), hdr->crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block checksum mismatch at offset " INT64_FORMAT " of file \"%s\"",
						(int64) offset, FilePathName(file))));
}

/*
 * Stream a slice [sliceoffset, sliceoffset + slicelength) of a block payload
 * into out, verifying the whole-block checksum along the way.  Value blocks
 * can be large and TOAST fetches one slice at a time, so the payload is never
 * materialized as a whole.
 */
void
appender_seg_read_slice(File file, int64 off, const AppenderBlockHeader *hdr,
						int32 sliceoffset, int32 slicelength, char *out)
{
	pg_crc32c	crc = appender_block_crc_after_header(hdr);
	int64		pos = 0;
	char		buf[8192];

	while (pos < hdr->datalen)
	{
		int64		left = hdr->datalen - pos;
		int64		want = Min((int64) sizeof(buf), left);
		int64		chunk_lo = pos;
		int64		chunk_hi = pos + want;
		int64		copy_lo = Max(chunk_lo, (int64) sliceoffset);
		int64		copy_hi = Min(chunk_hi, (int64) sliceoffset + slicelength);

		if (FileRead(file, buf, want,
					 off + sizeof(AppenderBlockHeader) + pos,
					 WAIT_EVENT_DATA_FILE_READ) != want)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read external value from segment block at offset " INT64_FORMAT ": %m",
							(int64) off)));

		COMP_CRC32C(crc, buf, want);

		if (copy_hi > copy_lo)
			memcpy(out + (copy_lo - sliceoffset),
				   buf + (copy_lo - chunk_lo),
				   copy_hi - copy_lo);

		pos += want;
	}

	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, hdr->crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block checksum mismatch at offset " INT64_FORMAT,
						(int64) off)));
}

/*
 * Flush a segment.  fdatasync is enough: it syncs the data and the file size.
 * On platforms that offer a write-through sync, honor wal_sync_method the way
 * pg_fsync does, so fsync_writethrough keeps its stronger guarantee.
 */
void
appender_seg_sync(File file)
{
	int			fd;
	int			rc;

	if (!enableFsync)
		return;
	fd = FileGetRawDesc(file);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" to sync: %m",
						FilePathName(file))));
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_SYNC);
#ifdef HAVE_FSYNC_WRITETHROUGH
	if (wal_sync_method == WAL_SYNC_METHOD_FSYNC_WRITETHROUGH)
		rc = pg_fsync(fd);
	else
#endif
		rc = pg_fdatasync(fd);
	pgstat_report_wait_end();
	if (rc != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not sync file \"%s\": %m",
						FilePathName(file))));
}

/*
 * Zero-fill the file past 'end', so later writes within the region do not
 * grow the file and their fdatasync stays cheap.  '*horizon' is the already
 * zero-filled extent, -1 when unknown (then it is read from the file size).
 */
void
appender_seg_ensure_room(File file, int64 end, int64 *horizon)
{
	int64		chunk = (int64) appender_prealloc_chunk_kb * 1024;
	int64		new_horizon;

	if (chunk <= 0 || !enableFsync)
	{
		/* the file may grow without us; the cached horizon is stale */
		*horizon = -1;
		return;
	}

	if (*horizon < 0)
	{
		*horizon = (int64) FileSize(file);
		if (*horizon < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not determine size of file \"%s\": %m",
							FilePathName(file))));
	}
	if (end <= *horizon)
		return;

	new_horizon = *horizon + ((end - *horizon + chunk - 1) / chunk) * chunk;
	if (FileZero(file, *horizon, new_horizon - *horizon,
				 WAIT_EVENT_DATA_FILE_WRITE) != 0)
	{
		/*
		 * Best effort: on a full disk skip the zero-fill.  fdatasync still
		 * makes the data durable, it just pays for the file-size change.
		 */
		if (errno == ENOSPC)
		{
			*horizon = -1;
			return;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not preallocate file \"%s\": %m",
						FilePathName(file))));
	}
	*horizon = new_horizon;
}

/* Remove a segment file; returns false if there was nothing to remove. */
bool
appender_seg_unlink(const RelFileLocator *rlocator, int segno)
{
	char		path[MAXPGPATH];

	appender_seg_cache_forget(rlocator, segno);
	appender_seg_path(rlocator, segno, path);
	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		return false;
	}
	return true;
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
		RelFileNumber relnum;
		int			segno;

		if (appender_seg_parse_name(de->d_name, &relnum, &segno) &&
			relnum == rlocator->relNumber)
		{
			if (appender_seg_unlink(rlocator, segno))
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
	int64		horizon = -1;

	appender_seg_path(rlocator, segno, path);
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	appender_seg_ensure_room(file, offset + (int64) len, &horizon);

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
