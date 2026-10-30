/*
 * col/test/test_int.h - what the test hooks of the column index share
 * the statics one file of the hooks calls from another
 */
#ifndef SIFT_COL_TEST_INT_H
#define SIFT_COL_TEST_INT_H

#ifdef SIFT_TESTING
#include "col/setsum.h"
#endif							/* SIFT_TESTING */

#ifdef SIFT_TESTING
#define TEST_NOWBUF	1

typedef struct TestQuery
{
	SiftColPred *top;
	int			ntop;
	SiftColGroupPred *grp;
	int			ngrp;
	SiftColPred *gpreds;		/* the predicates of all groups, grouped */
} TestQuery;

typedef struct TestScan
{
	Relation	heap;
	Relation	index;
	SiftColSource *src;
	TestQuery	q;
} TestScan;
#endif							/* SIFT_TESTING */

#ifdef SIFT_TESTING
extern void	ctest_scan_open(TestScan *ts, Oid indexoid, const char *preds, int flags);

extern void	ctest_scan_close(TestScan *ts);

extern bool	ctest_test_next(TestScan *ts, SiftColUnit *unit);

extern bool	ctest_unit_select(TestScan *ts, const SiftColUnit *unit, int flags, SiftColSel *sel);
#endif							/* SIFT_TESTING */

#endif