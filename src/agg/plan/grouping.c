/*
 * agg/plan/grouping.c - the grouping of a spec
 * GROUP BY keys, grouping sets and their top-N, compiled into the keys the scan
 * builds.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

#include "agg/plan/int.h"

/*
 * rows: what the groups are made of. By PostgreSQL's estimate of the groups (distinct false) they are no more than the rows its statistics
 * of the table expect after the filters; by the distinct values of the key columns they are no more than the rows given, whatever the
 * filters keep (a column's values under the filters on it alone, column_distinct).
 */
double
set_groups(AggCtx *c, PlannerInfo *root, SiftAggSpec *s, List *keys, double rows, bool distinct)
{
	double		n = 0;

	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		List	   *sub = NIL,
				   *rest = NIL;
		double		bucketed = 1.0,
					g = 1.0;

		for (int j = 0; j < list_length(keys); j++)
		{
			double		b = key_groups(c, &s->key[j]);

			if (s->nsets != 0 && !(s->setmask[set] >> j & 1))
				continue;
			if (b > 0)
				bucketed *= b;
			else
				sub = lappend(sub, stats_expr(c, list_nth(keys, j)));
		}
		foreach_ptr(Node, e, sub)
		{
			double		nd;

			if (distinct && IsA(e, Var) && ((Var *) e)->varno == c->relid && (nd = column_distinct(c, (Var *) e)) > 0)
				g *= nd;
			else
				rest = lappend(rest, e);
		}
		/* Min and Max evaluate their arguments more than once: the estimate is made once */
		if (rest != NIL)
			g *= Max(estimate_num_groups(root, rest, rows, NULL, NULL), 1.0);
		n += Min(rows, Max(g, 1.0) * bucketed);
	}
	return n;
}

/* the index among the group columns of the one with the sortgroupref ref */
static int
key_of_ref(Query *parse, int ref)
{
	foreach_node(SortGroupClause, sgc, parse->groupClause)
		if (sgc->tleSortGroupRef == ref)
			return foreach_current_index(sgc);
	SIFT_INVARIANT(false, "sift: grouping column %d not found", ref);
}

/* GROUPING(columns): the keys of its arguments; the executor makes its bitmask of the set of each group */
bool
compile_grouping(Query *parse, GroupingFunc *g, SiftAggAgg *out)
{
	if (g->agglevelsup != 0 || parse->groupingSets == NIL)
		return agg_refuse_on(GROUPING_SETS, "GROUPING() of an outer query level, or in a query without grouping sets");
	out->kind = SAA_GROUPING;
	out->leaf = -1;
	out->restype = INT4OID;
	out->nref = list_length(g->refs);
	out->ref = palloc(out->nref * sizeof(int));
	foreach_int(ref, g->refs)
		out->ref[foreach_current_index(ref)] = key_of_ref(parse, ref);
	out->desc = "GROUPING";
	return true;
}

/*
 * The grouping sets as PostgreSQL expanded them (parse->groupingSets: a list of lists of sortgrouprefs, ROLLUP and CUBE included,
 * duplicates as written): a mask over the group columns for each, and the number of the set as one more key, so that the groups of
 * two sets never meet. The scan emits it as a constant column, which nothing reads.
 */
void
add_sets(Query *parse, SiftAggSpec *s, List **scan_tlist)
{
	SiftAggKey *k = &s->key[s->nkeys];

	s->nsets = list_length(parse->groupingSets);
	s->setmask = palloc(s->nsets * sizeof(uint32));
	foreach_ptr(List, set, parse->groupingSets)
	{
		uint32		mask = (uint32) 1 << s->nkeys;

		foreach_int(ref, set)
			mask |= (uint32) 1 << key_of_ref(parse, ref);
		s->setmask[foreach_current_index(set)] = mask;
	}
	k->kind = SAK_SET;
	k->leaf = -1;
	k->typid = INT4OID;
	k->typmod = -1;
	k->eqop = Int4EqualOperator;
	k->desc = "grouping set";
	*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(0), false, true),
													   list_length(*scan_tlist) + 1, NULL, false));
	s->nkeys++;
}

/*
 * ORDER BY an aggregate LIMIT n of a plain GROUP BY (no HAVING, window, DISTINCT or grouping sets above the groups): the first sort key is an
 * aggregate of the scan whose value the executor ranks without making the group a tuple, so the groups after the n-th (+ OFFSET) are not emitted.
 * The sort and the limit stay in PostgreSQL's plan: it ranks the ties by the other keys.
 */
void
group_topn(PlannerInfo *root, SiftAggSpec *s, List *aggs, List *groupClause, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	SortGroupClause *sgc;
	Node	   *e;
	TypeCacheEntry *tc;
	Const	   *lim,
			   *off = NULL;
	int64		k;
	int			ai = -1;

	if (parse->sortClause == NIL || parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || groupClause != parse->groupClause ||
		parse->groupClause == NIL || parse->groupingSets != NIL || parse->hasWindowFuncs || parse->hasDistinctOn || parse->distinctClause != NIL ||
		parse->hasTargetSRFs || parse->havingQual != NULL || (gextra && gextra->havingQual))
		return;
	lim = (Const *) parse->limitCount;
	if (!IsA(lim, Const) || lim->constisnull || (parse->limitOffset && (!IsA(parse->limitOffset, Const) || ((Const *) parse->limitOffset)->constisnull)))
		return;
	if (parse->limitOffset)
		off = (Const *) parse->limitOffset;
	k = DatumGetInt64(lim->constvalue) + (off ? DatumGetInt64(off->constvalue) : 0);
	if (k <= 0 || k > 1000000)
		return;
	sgc = linitial_node(SortGroupClause, parse->sortClause);
	e = strip_relabel((Node *) get_sortgroupclause_expr(sgc, root->processed_tlist));
	foreach_ptr(Node, a, aggs)
		if (IsA(a, Aggref) && equal(strip_relabel(a), e))
			ai = foreach_current_index(a);
	if (ai < 0 || !sift_aggspec_gtopn_kind(&s->agg[ai]))
		return;
	tc = lookup_type_cache(exprType(e), TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	if (sgc->sortop != tc->lt_opr && sgc->sortop != tc->gt_opr)
		return;
	s->gtopn_k = (int) k;
	s->gtopn_agg = ai;
	s->gtopn_desc = sgc->sortop == tc->gt_opr;
	s->gtopn_nulls_first = sgc->nulls_first;
}
