/*
 * core/check.h - sift.check(): the verifiers as steps that either raise (sift.verify, sift.col_verify) or report. A step is one unit of the
 * verification (the store, one segment, the totals of the metapage, the rows against the heap); reporting runs it in a subtransaction, so the
 * pins, locks and memory it took are gone whatever it found, and the next step starts clean. Only a corruption is a problem: any other error
 * (a cancel, a serialization failure on a standby, out of memory) leaves the function as it would from the raising form.
 */
#ifndef SIFT_CHECK_H
#define SIFT_CHECK_H

#include "postgres.h"

#include "fmgr.h"
#include "utils/tuplestore.h"

#define SIFT_CHECK_WHOLE	PG_UINT64_MAX	/* a problem of no one segment */

typedef struct SiftCheck
{
	Oid			relid;
	Tuplestorestate *rows;
	TupleDesc	desc;
	int			max_problems;
	int			nproblems;
} SiftCheck;

typedef void (*SiftCheckStep) (void *arg);

/* ck NULL: runs step, errors raise. Else a corruption it raises becomes a row of segment (SIFT_CHECK_WHOLE: none); false once max_problems are found */
extern bool sift_check_step(SiftCheck *ck, uint64 segment, SiftCheckStep step, void *arg);

/* Are max_problems found: nothing more is looked for */
static inline bool
sift_check_full(const SiftCheck *ck)
{
	return ck != NULL && ck->nproblems >= ck->max_problems;
}

/* The materialized result of a call of sift.check's walker (fcinfo of a function RETURNS TABLE(index, segment, problem, detail)) */
extern SiftCheck *sift_check_begin(FunctionCallInfo fcinfo, Oid relid, int max_problems);

/* The verifier of an index or sift_table by its kind; ck NULL raises */
extern void sift_check_relation(SiftCheck *ck, Oid relid, bool heapallindexed);

/* Each opens the relation, checks that the caller may maintain it and verifies it: col/v1/verify.c, bm25/segment/sql.c, table/funcs.c */
extern void sift_col_check(SiftCheck *ck, Oid indexoid, bool heapallindexed);
extern void sift_bm25_check(SiftCheck *ck, Oid indexoid, bool heapallindexed);
extern void sift_tbl_check(SiftCheck *ck, Oid relid);

#endif							/* SIFT_CHECK_H */
