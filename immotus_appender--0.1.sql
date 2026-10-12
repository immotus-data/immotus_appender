/* immotus_appender--0.1.sql */

\echo Use "CREATE EXTENSION immotus_appender" to load this file. \quit

CREATE FUNCTION appender_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD appender TYPE TABLE HANDLER appender_tableam_handler;

/*
 * Segment manifest: one row per segment of an appender relation storage.
 * A row's eof and rowcount become visible together with the commit of the
 * transaction that appended them, which is what makes appends atomic.
 * Maintained by the extension only; it has no index on purpose, because
 * the extension writes it with simple_heap_* calls that skip index upkeep.
 */
CREATE TABLE appender_meta
(
    spcoid          oid      NOT NULL,
    relfilenode     oid      NOT NULL,
    segno           int4     NOT NULL,
    state           int2     NOT NULL,
    eof             int8     NOT NULL,
    rowcount        int8     NOT NULL,
    format_version  int2     NOT NULL
);

CREATE FUNCTION appender_meta_guard()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE TRIGGER appender_meta_guard_trigger
BEFORE INSERT OR UPDATE OR DELETE OR TRUNCATE ON appender_meta
FOR EACH STATEMENT
EXECUTE FUNCTION appender_meta_guard();

CREATE FUNCTION appender_relation_size(regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'appender_relation_size_sql'
LANGUAGE C STRICT;

CREATE FUNCTION appender_gc()
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C;

REVOKE ALL ON FUNCTION appender_gc() FROM PUBLIC;
