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
 * appender_meta holds the logical EOF of each data file.  It is a regular
 * heap table created in the extension's schema, so its rows follow normal
 * MVCC: the EOF committed by an insert transaction becomes visible only to
 * snapshots taken after its commit.  Readers therefore see exactly the
 * committed prefix of a datafile; a crashed or aborted transaction leaves
 * bytes beyond the visible EOF, which are ignored (and truncated on
 * recovery).  This is the AO crash-safety model: data durability via fsync +
 * commit-time EOF, no per-tuple WAL.
 */

/*
 * EOF metadata: one row per (relation, datafile).  No PK/index on purpose:
 * the extension code maintains it with simple_heap_insert/update and does a
 * tiny seqscan; a PK would go stale because simple_heap_* skip index
 * maintenance.
 */
CREATE TABLE appender_meta
(
    relid oid   NOT NULL,
    filenum int   NOT NULL,
    eof   bigint NOT NULL
);

/*
 * Guard against direct DML: appender_meta is maintained exclusively by the
 * extension code (simple_heap_insert/update/delete, which bypass triggers).
 * Any user-level INSERT/UPDATE/DELETE/TRUNCATE would corrupt the EOF metadata
 * for every appender table, so block it.
 */
CREATE FUNCTION appender_meta_guard()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE TRIGGER appender_meta_guard_trigger
BEFORE INSERT OR UPDATE OR DELETE OR TRUNCATE ON appender_meta
FOR EACH STATEMENT
EXECUTE FUNCTION appender_meta_guard();
