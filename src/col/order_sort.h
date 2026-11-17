/*
 * col/order_sort.h - the rows of a merge that do not fit maintenance_work_mem, sorted in the order the merge reads (col/order_sort.c)
 */
#ifndef SIFT_COL_ORDER_SORT_H
#define SIFT_COL_ORDER_SORT_H

#include "col/enc.h"

typedef struct SiftColSorter SiftColSorter;

/* A sort of rows by the leaves of the cut (none: by TID), most significant first, NULL last, then by TID: work_kb of memory, the rest on temporary files */
extern SiftColSorter *sift_colsort_begin(const SiftColSchema *schema, const int *leaf, int nleaf, int work_kb);
/* Row r of b goes in; the sort is over when sift_colsort_done is called, before anything is read */
extern void sift_colsort_put(SiftColSorter *s, const SiftColBatch *b, uint32 r);
/* ... or the copy of row r for a scope key: the integer leaf scope_leaf holds key in it, and the leaves of kept that are false are NULL (sift_cbatch_add_scoped) */
extern void sift_colsort_put_scoped(SiftColSorter *s, const SiftColBatch *b, uint32 r, int scope_leaf, int64 key, const bool *kept);
extern void sift_colsort_done(SiftColSorter *s);
/* Appends the next row, in order, to b (the keys of its rows need not ascend: b is an unsorted batch); false at the end */
extern bool sift_colsort_next(SiftColSorter *s, SiftColBatch *b);
extern void sift_colsort_end(SiftColSorter *s);

#ifdef SIFT_TESTING
extern int64 sift_test_cut_sort_rows;	/* sift._test_col_cut_sorts(): the rows the merges of this backend sorted by a sort, and the kB of temporary files it used */
extern int64 sift_test_cut_sort_disk_kb;
extern int64 sift_test_cut_sort_cut;	/* ... and the rows of an input a window of the merge cut, which the sort did not take */
#endif

#endif							/* SIFT_COL_ORDER_SORT_H */
