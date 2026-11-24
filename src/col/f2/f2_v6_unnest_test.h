/*
 * col/f2/f2_v6_unnest_test.h - what the aggregate scan of an unnest (col/f2/f2_v6_unnest_run.h) decides on the element columns of a granule: the quals of the element (SiftV6Ext.elem_where) and the tests of the
 * elements of a row's array (SiftV6Ext.test: EXISTS, jsonpath). A test is true of a row when some element passes its quals and its comparisons with the columns of the row (a column of the batch, each
 * value of it standing for the elements of its row); the rows it answers are the ones the layout holds, the others are the caller's, from the heap.
 */
#ifndef SIFT_COL_F2_V6_UNNEST_TEST_H
#define SIFT_COL_F2_V6_UNNEST_TEST_H

#include "postgres.h"

#include "nodes/pg_list.h"

#include "agg/v6/v6_ext.h"
#include "col/f2/f2_sink_ext.h"

typedef struct SiftF2ElemEval SiftF2ElemEval;

/* The quals (SiftV6Ext.elem_where) over the element columns; they compiled when the statement was planned */
extern SiftF2ElemEval *sift_f2_elem_eval_open(MemoryContext cx, const List *quals);
/* bit e of keep, for the elements [lo, hi), is whether element e passes the quals; ev: the element columns */
extern void sift_f2_elem_eval(SiftF2ElemEval *ee, const SiftVec *ev, uint32 lo, uint32 hi, uint64 *keep);

typedef struct SiftF2RowTests SiftF2RowTests;

extern SiftF2RowTests *sift_f2_row_tests_open(MemoryContext cx, const SiftV6Ext *x);
/*
 * The tests of the parents of the layout of es that are in rows: yes[k] bit r is whether test k is true of row r, known[k] whether the row has a document (a test of a NULL document is not true). ev: the
 * element columns of the granule; col: the columns of the batch, outer[k * SIFT_V6_XCMPS + c] the one comparison c of test k is with.
 */
extern void sift_f2_row_tests_eval(SiftF2RowTests *t, const SiftF2ElemStream *es, const SiftVec *ev, const SiftVec *col, const int *outer, const uint64 *rows,
								   uint64 yes[][SIFT_VEC_WORDS], uint64 known[][SIFT_VEC_WORDS]);
/* Test k of one row whose nel elements are ev, with the one value outer[c] each comparison c is with */
extern bool sift_f2_row_test_one(SiftF2RowTests *t, int k, const SiftVec *ev, uint32 nel, const SiftVec *outer);

#endif							/* SIFT_COL_F2_V6_UNNEST_TEST_H */
