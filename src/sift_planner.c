/*
 * sift_planner.c - planner side of the index: amcostestimate and the
 * matching of expressions to index keys (DESIGN.md (f) 6). The hooks and the
 * custom scans of M3 live here too.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "sift_am.h"
#include "sift_eval.h"
#include "sift_planner.h"

/* Dictionary pages a term lookup reads: root array hit, DICTIDX, DICT block */
#define DICT_PAGES_PER_TERM		2.0
/* A posting costs about a byte of codec blob, tf sidecar included (docs/CODEC.md) */
#define BYTES_PER_POSTING		1.0
/* Decoding and combining postings, per posting in operator costs */
#define POSTING_CPU_FRACTION	(1.0 / 16)

bool
sift_index_matches_key(IndexOptInfo *index, Node *expr)
{
	if (index->ncolumns != 1 || index->relam != sift_catalog()->am)
		return false;
	while (IsA(expr, RelabelType))
		expr = (Node *) ((RelabelType *) expr)->arg;
	if (index->indexkeys[0] != 0)
		return IsA(expr, Var) && ((Var *) expr)->varno == index->rel->relid &&
			((Var *) expr)->varattno == index->indexkeys[0];
	return equal(expr, linitial(index->indexprs));
}

/* Query texts of the index's quals; false when one is not a constant */
static bool
qual_queries(IndexPath *path, text ***queries, int *nqueries)
{
	*queries = palloc(list_length(path->indexclauses) * sizeof(text *));
	*nqueries = 0;
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		Node	   *arg = lsecond(castNode(OpExpr, ic->rinfo->clause)->args);

		if (!IsA(arg, Const) || ((Const *) arg)->constisnull)
			return false;
		(*queries)[(*nqueries)++] = DatumGetTextPP(((Const *) arg)->constvalue);
	}
	return true;
}

/*
 * Selectivity comes from the operator's estimator. The index work follows
 * from the dictionary terms the queries resolve to: a few pages per term
 * plus the postings read, sequentially.
 */
void
sift_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	GenericCosts costs = {0};
	text	  **queries;
	int			nqueries;

	genericcostestimate(root, path, loop_count, &costs);
	if (qual_queries(path, &queries, &nqueries))
	{
		SiftEstimate est;
		double		random_cost,
					seq_cost,
					dict_pages;
		Relation	index = index_open(path->indexinfo->indexoid, AccessShareLock);

		sift_eval_estimate(index, queries, nqueries, &est);
		index_close(index, AccessShareLock);
		get_tablespace_page_costs(path->indexinfo->reltablespace, &random_cost, &seq_cost);
		dict_pages = est.terms * DICT_PAGES_PER_TERM;
		costs.numIndexPages = dict_pages + ceil(est.postings * BYTES_PER_POSTING / BLCKSZ);
		costs.indexTotalCost = dict_pages * random_cost + (costs.numIndexPages - dict_pages) * seq_cost +
			est.postings * cpu_operator_cost * POSTING_CPU_FRACTION;
		costs.indexStartupCost = costs.indexTotalCost;
	}
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = costs.numIndexPages;
}
