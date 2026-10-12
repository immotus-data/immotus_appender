/*-------------------------------------------------------------------------
 *
 * appender_metadata.h
 *    EOF metadata (appender_meta) for immotus_appender.
 *
 * The appender_meta table is a regular heap table owned by the extension.  A
 * reader using its own snapshot sees the EOF committed as of that snapshot,
 * hence exactly the committed prefix of the data file.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_METADATA_H
#define APPENDER_METADATA_H

#include "postgres.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

/* Logical EOF of the relation's datafile as visible under the given snapshot. */
extern int64 appender_get_eof(Relation rel, Snapshot snapshot);

/*
 * Record a new EOF for the relation (insert or update the appender_meta row).
 * Called once per transaction at PRE_COMMIT; the row update commits/rolls back
 * with it, so an aborted insert leaves the old EOF and its datafile tail is
 * ignored.
 */
extern void appender_update_eof(Oid relid, int64 eof);

/* Drop the relation's appender_meta row (EOF resets to 0 for a new relfilenode). */
extern void appender_reset_eof(Oid relid);

#endif							/* APPENDER_METADATA_H */
