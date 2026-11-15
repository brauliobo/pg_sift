/*
 * core/damage.h - the one way to report damaged index data.
 *
 * Bytes the format does not allow are raised through these macros and no other: ERRCODE_INDEX_CORRUPTED (XX002) for the structure of an index (pages,
 * directory, records), ERRCODE_DATA_CORRUPTED (XX001) for data a decoder cannot read. The raise site decides the class by calling its macro, never by the
 * SQLSTATE or the text of an error (scripts/check-damage.sh keeps the SQLSTATEs out of every other file).
 *
 *   SIFT_DAMAGE_INDEX(index, errmsg(...), ...)   structure of the index in hand
 *   SIFT_DAMAGE_DATA(index, errmsg(...), ...)    data a decoder rejected; index is the Relation in hand, NULL for a decoder that has none
 *   SIFT_DAMAGE_OTHER_DATA(errmsg(...), ...)     damage of data that is no index to take out of service: a sift_table (it is the data, it has no heap to fall
 *                                                back to), a value a caller passed in
 *   SIFT_DAMAGE_OTHER_INDEX(errmsg(...), ...)    damage found in an index that is not a sift index (a b-tree the planner reads)
 *
 * The arguments after the Relation are those of ereport (errmsg, errdetail, errhint).
 */
#ifndef SIFT_DAMAGE_H
#define SIFT_DAMAGE_H

#include "core/host.h"

#define SIFT_DAMAGE_RAISE(code, ...) ereport(ERROR, (errcode(code), __VA_ARGS__))

#define SIFT_DAMAGE_INDEX(index, ...)	SIFT_DAMAGE_RAISE(ERRCODE_INDEX_CORRUPTED, __VA_ARGS__)
#define SIFT_DAMAGE_DATA(index, ...)	SIFT_DAMAGE_RAISE(ERRCODE_DATA_CORRUPTED, __VA_ARGS__)
#define SIFT_DAMAGE_OTHER_DATA(...)		SIFT_DAMAGE_RAISE(ERRCODE_DATA_CORRUPTED, __VA_ARGS__)
#define SIFT_DAMAGE_OTHER_INDEX(...)	SIFT_DAMAGE_RAISE(ERRCODE_INDEX_CORRUPTED, __VA_ARGS__)

#endif							/* SIFT_DAMAGE_H */
