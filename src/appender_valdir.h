/*-------------------------------------------------------------------------
 *
 * appender_valdir.h
 *    Value directories of appender segments and out-of-line value fetch.
 *
 * A segment's directory maps value ids to the offsets of their value blocks
 * and is consulted when a TOAST slice is fetched.  appender_valdir.c owns the
 * per-backend cache of parsed directories and the toast fetch path.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDER_VALDIR_H
#define APPENDER_VALDIR_H

#include "utils/rel.h"

struct varlena;

/* Per-backend memory budget of cached value directories (GUC). */
extern PGDLLIMPORT int appender_valdir_cache_mb;

extern void appender_valdir_cache_reset(void);
extern int	appender_valdir_entry_cmp(const void *a, const void *b);
extern void appender_fetch_toast_slice(Relation toastrel, Oid valueid,
									   int32 attrsize, int32 sliceoffset,
									   int32 slicelength,
									   struct varlena *result);

#endif							/* APPENDER_VALDIR_H */
