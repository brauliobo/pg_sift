/*
 * agg/unnest/test.h - what the aggregate scan of an unnest (a format's unnest run) decides on the element columns of a batch: the quals of the element (SiftV6Ext.elem_where) and the tests of the
 * elements of a row's array (SiftV6Ext.test: EXISTS, jsonpath). A test is true of a row when some element passes its quals and its comparisons with the columns of the row (a column of the batch, each
 * value of it standing for the elements of its row); the rows it answers are the ones the layout holds, the others are the caller's, from the heap.
 */
#ifndef SIFT_AGG_V6_UNNEST_TEST_H
#define SIFT_AGG_V6_UNNEST_TEST_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "agg/ext.h"
#include "agg/vec.h"

typedef struct SiftV6ElemEval SiftV6ElemEval;

/* The quals (SiftV6Ext.elem_where) over the element columns; they compiled when the statement was planned */
extern SiftV6ElemEval *sift_v6_elem_eval_open(MemoryContext cx, const List *quals);
/* bit e of keep, for the elements [lo, hi), is whether element e passes the quals; ev: the element columns */
extern void sift_v6_elem_eval(SiftV6ElemEval *ee, const SiftVec *ev, uint32 lo, uint32 hi, uint64 *keep);

/* the arms of a disjunction over the element columns as one qual: the arm, or their OR */
extern Expr *sift_v6_unnest_or_arms(List *arms);

typedef struct SiftV6RowTests SiftV6RowTests;

extern SiftV6RowTests *sift_v6_row_tests_open(MemoryContext cx, const SiftV6Ext *x);
/*
 * The tests of the parents of the layout that are in rows: yes[k] bit r is whether test k is true of row r, known[k] whether the row has a document (a test of a NULL document is not true). ev: the
 * element columns of the granule; col: the columns of the batch, outer[k * SIFT_V6_XCMPS + c] the one comparison c of test k is with.
 */
extern void sift_v6_row_tests_eval(SiftV6RowTests *t, const SiftArrayLayout *lay, const uint64 *doc_valid, const SiftVec *ev, const SiftVec *col, const int *outer, const uint64 *rows,
								   uint64 yes[][SIFT_VEC_WORDS], uint64 known[][SIFT_VEC_WORDS]);
/* Test k of one row whose nel elements are ev, with the one value outer[c] each comparison c is with */
extern bool sift_v6_row_test_one(SiftV6RowTests *t, int k, const SiftVec *ev, uint32 nel, const SiftVec *outer);

#endif							/* SIFT_AGG_V6_UNNEST_TEST_H */
