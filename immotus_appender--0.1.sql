/*-------------------------------------------------------------------------
 *
 * immotus_appender--0.1.sql
 *    SQL portion of immotus_appender extension.
 *
 * Registers the "appender" table access method.
 *
 *-------------------------------------------------------------------------*/

CREATE FUNCTION appender_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD appender TYPE TABLE HANDLER appender_tableam_handler;

/*
 * Metadata for append-only tables.
 *
 * datafiles holds the logical EOF of each data file.  It is a regular
 * heap table, so its rows follow normal MVCC: the EOF committed by an insert
 * transaction becomes visible only to snapshots taken after its commit.
 * Readers therefore see exactly the committed prefix of a datafile; a crashed
 * or aborted transaction leaves bytes beyond the visible EOF, which are
 * ignored (and truncated on recovery).  This is the AO crash-safety model:
 * data durability via fsync + commit-time EOF, no per-tuple WAL.
 */
CREATE SCHEMA immotus_meta;

/*
 * EOF metadata: one row per (relation, datafile).  No PK/index on purpose:
 * the extension code maintains it with simple_heap_insert/update and does a
 * tiny seqscan; a PK would go stale because simple_heap_* skip index
 * maintenance.
 */
CREATE TABLE immotus_meta.datafiles
(
    relid oid   NOT NULL,
    filenum int   NOT NULL,
    eof   bigint NOT NULL
);
