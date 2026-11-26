/*
 * col/f2/scope/spool.h - the (int8, int8) rows a scope build collects (the pairs of a scope, the keys alone as (key, 0)): in an array while the build's tracker has room for it, then in a sort that
 * the memory of maintenance_work_mem bounds and that writes its runs to temporary files, so that no build ends for the number of its pairs. Rows are read back ascending by both numbers.
 */
#ifndef SIFT_F2_SCOPE_SPOOL_H
#define SIFT_F2_SCOPE_SPOOL_H

#include "col/f2/cube/meta.h"
#include "core/memtrack.h"

typedef struct SiftScopeSpool SiftScopeSpool;

/* mem: the tracker whose root context holds the array; the spool is made in the current context */
extern SiftScopeSpool *sift_scope_spool_new(SiftMemTracker *mem);
extern void sift_scope_spool_put(SiftScopeSpool *s, int64 a, int64 b);
/* Has the array left for the sort: its rows are read with sift_scope_spool_next only */
extern bool sift_scope_spool_spilled(const SiftScopeSpool *s);
/* The array of a spool that has not spilled (not sorted): its rows and their number */
extern SiftScopeMemberKey *sift_scope_spool_rows(SiftScopeSpool *s, uint32 *n);
/* Sorts the rows, and starts the reads at the first; sift_scope_spool_rewind starts them again */
extern void sift_scope_spool_sort(SiftScopeSpool *s);
extern void sift_scope_spool_rewind(SiftScopeSpool *s);
extern bool sift_scope_spool_next(SiftScopeSpool *s, int64 *a, int64 *b);
extern void sift_scope_spool_free(SiftScopeSpool *s);

#endif							/* SIFT_F2_SCOPE_SPOOL_H */
