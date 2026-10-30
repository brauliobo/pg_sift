/*
 * agg/order/cost.c - what a Top-N costs
 * The bounds of a LIMIT, and the price of the heap and of the sort it replaces, by the
 * width and kind of the key.
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/topn.h"
#include "agg/order/int.h"

/* LIMIT and OFFSET are the same at every execution of the plan: no sub-plan, no executor parameter, no volatile function */
static bool
topn_bound_ok(Node *e)
{
	return e == NULL || (sift_operand_ok(e) && !contain_volatile_functions(e));
}

/* The value the planner expects of a LIMIT or OFFSET (a parameter is a constant of a custom plan); -1 for NULL, `unknown` when it is not a constant */
static int64
topn_value(PlannerInfo *root, Node *e, int64 unknown)
{
	Node	   *v = estimate_expression_value(root, e);

	if (!IsA(v, Const))
		return unknown;
	return ((Const *) v)->constisnull ? -1 : DatumGetInt64(((Const *) v)->constvalue);
}

/*
 * LIMIT and OFFSET of the query, as far as the planner knows them; false when there is no bound it can use. A parameter of a generic plan
 * is not known: the scan gets its value when it starts, and handles any (agg/topn.h), so the plan assumes a few rows. What the heap cannot
 * hold (more rows than SIFT_TOPN_MAX_ROWS, more keys than it has) the scan sorts: the path is priced with that sort (topn_sort_cost).
 */
bool
topn_bounds(Query *parse, PlannerInfo *root, double *count, double *offset)
{
	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return false;
	*count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	*offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	return *count >= 1 && *offset >= 0;
}

/*
 * the heap of n rows takes the rows read: each is compared with the n-th and the better ones sift down, log2(n) steps (a heap of a page is the baseline).
 * cmpx: what a compare of byte strings costs besides an integer's: every row is compared once, and the rows that enter the heap (about n for
 * every doubling of the rows read) sift down log2(n) steps
 */
Cost
topn_heap_cost(double read, double n, double cmpx)
{
	double		pushes = n * (1.0 + log(Max(read / Max(n, 1.0), 1.0)));

	return (n <= TOPN_SORT_ROWS ? 0 : 4.0 * cpu_operator_cost * read * log2(n / TOPN_SORT_ROWS)) +
		cmpx * cpu_operator_cost * (read + pushes * log2(Max(n, 2.0)));
}

/*
 * Every row that passes, with its keys and TID: sorted in memory (the keys are compared once per step of a sort, as PostgreSQL's cost_sort
 * prices it, without the tuples) up to hash_mem, or, past it, by a tuplesort that writes runs to disk (cost_sort's model of it)
 */
Cost
topn_sort_cost(double rows, int nkey, double keybytes, double cmpx)
{
	double		budget = work_mem * 1024.0,
				mem = rows * ((nkey + (nkey + 63) / 64 + 1) * sizeof(int64) + keybytes),
				cost = cpu_operator_cost * rows * ((log2(Max(rows, 2.0)) + 1.0) * (1.0 + cmpx) + TOPN_KEY_READ * (nkey - 1));

	/* byte strings do not fit the array: the rows go to a tuplesort at once, which spills past work_mem */
	if (mem > (keybytes > 0 ? budget : Min((double) get_hash_memory_limit(), 512.0 * 1024 * 1024)))
	{
		double		bytes = rows * (MAXALIGN((nkey + 1) * sizeof(int64)) + MAXALIGN(SizeofHeapTupleHeader) + keybytes),
					passes = Max(1.0, ceil(log(Max(bytes / budget, 2.0)) / log(6.0)));

		cost += TOPN_SPILL_STEP * cpu_operator_cost * rows * log2(Max(rows, 2.0)) + 2.0 * (bytes / BLCKSZ) * passes * (seq_page_cost * 0.75 + random_page_cost * 0.25);
	}
	return cost;
}

/* The bytes a key of byte strings holds in an entry of the heap or a row of the sort: the column's average width, a header and the allocation's */
double
topn_key_width(AggCtx *c, Node *e, Oid typid)
{
	Node	   *x = strip_relabel(e);
	int32		w = IsA(x, Var) ? get_attavgwidth(c->heapoid, ((Var *) x)->varattno) : 0;

	return (w > 0 ? w : get_typavgwidth(typid, -1)) + sizeof(uint32) + 16;
}
