/*
 * col/f2/f2_unique.h - UNIQUE indexes of format major 2: the check CREATE UNIQUE INDEX makes of the rows it reads, and the check of each row a writer adds (f2_unique.c)
 */
#ifndef SIFT_F2_UNIQUE_H
#define SIFT_F2_UNIQUE_H

#include "access/genam.h"
#include "access/tableam.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

/* ERROR (0A000) for a key column the leaf cannot look up by equality exactly: the key of a unique index is an integer, boolean, date, timestamp, numeric, uuid, text or varchar column */
extern void sift_f2_unique_check_keys(Relation index);

/*
 * aminsert of a unique index with checkUnique other than UNIQUE_CHECK_NO: a duplicate of a live row, or of one a running transaction wrote, is 23505 (UNIQUE_CHECK_YES) or is reported by
 * the result false once the row is in (UNIQUE_CHECK_PARTIAL: the constraint is deferred or the statement is an ON CONFLICT); UNIQUE_CHECK_EXISTING only checks the row, which is in already. The result
 * is true when the key is known unique.
 */
extern bool sift_f2_unique_insert(Relation index, Relation heap, Datum *values, bool *isnull, ItemPointer heap_tid, IndexUniqueCheck check, IndexInfo *info);

typedef struct SiftF2UniqueBuild SiftF2UniqueBuild;

/* The rows of a build, collected by their key in a sort bounded by maintenance_work_mem: NULL for an index that is not unique */
extern SiftF2UniqueBuild *sift_f2_unique_build_begin(Relation heap, Relation index);
extern void sift_f2_unique_build_add(SiftF2UniqueBuild *u, const Datum *values, const bool *isnull);

/* Sorts what was added and raises 23505 for two rows of one key; frees the sort */
extern void sift_f2_unique_build_end(SiftF2UniqueBuild *u);

#endif							/* SIFT_F2_UNIQUE_H */
