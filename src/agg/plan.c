/*
 * agg/plan.c - planner side of SiftAgg: proves that a single-table aggregate query (optionally unnesting one array or
 * object of the column source) is answered by the column source with the plain plan's result, and adds a
 * Custom Scan (SiftAgg) path for it in create_upper_paths_hook (UPPERREL_GROUP_AGG). Everything not proven keeps the
 * normal plan. Execution is in agg/agg.c; the plan travels as a SiftAggSpec (agg/spec.h).
 *
 * Matching works on the query tree (root->parse), not on the paths: the FROM list must be the table (and at most one lateral
 * set-returning function over one of its jsonb leaves), the quals are compiled to bitmap programs over leaves, and the group
 * keys and aggregates to accumulators. A leaf reference is an expression that equals what the column source stored: an
 * attribute or index expression, a jsonb path of it with the declared type (`(x->>'k')::float8`, `x #>> '{a,b}'`), a field
 * of the unnested function's rows. Any boolean or scalar expression of ONE leaf that is a dictionary (text) or small integer
 * leaf is evaluated by the executor with PostgreSQL's own expression evaluator on every distinct value of the leaf, so its
 * semantics (collations, NULL handling, casts, CASE) are PostgreSQL's by construction.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/*
 * The leaf of the index that a leaf after the source's takes its values from: the value column of a member of an object, the array whose length it
 * is; -1 for the type of a path (the tags of its leaf say it, whatever the leaf holds). Whatever that leaf cannot store is read as NULL.
 */
static int
vleaf_source(AggCtx *c, int j)
{
	const SiftAggVLeaf *v = &c->vleaf[j];

	if (v->count)
		return v->obj;
	return sift_aggvleaf_member(v) ? child_leaf(c, v->obj, "value", leaf_of(c, c->lay->nleaves - c->nvleaf + j)->typid) : -1;
}

/*
 * The FROM list of root's query as AggCtx: the scanned table with a column source, at most one lateral unnest of one of its leaves,
 * the tables that join to it by equality. input_rel (NULL: not the query being planned) is the join the planner made of it.
 */
/*
 * The key of `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`, NULL for any other outer join of the function. An object has one member
 * per key, so the join is the member k of each row (a row without it keeps NULL columns): a row level value, not an unnest. A JSON null and
 * a missing key are the same NULL there; before PostgreSQL 18 a JSON null raises where the missing key does not, so the normal plan stays.
 */
static const char *
left_each_key(LeftJoin *lj, Index rti, FuncKind kind)
{
#if PG_VERSION_NUM >= 180000
	OpExpr	   *op;
	Node	   *a,
			   *b;

	if (kind != FK_EACH || lj->kind != SJ_LEFT || list_length(lj->quals) != 1 || !IsA(linitial(lj->quals), OpExpr))
		return NULL;
	op = linitial_node(OpExpr, lj->quals);
	set_opfuncid(op);
	if (op->opfuncid != F_TEXTEQ)
		return NULL;
	a = strip_relabel(linitial(op->args));
	b = strip_relabel(lsecond(op->args));
	if (IsA(b, Var))
	{
		Node	   *t = a;

		a = b;
		b = t;
	}
	if (!IsA(a, Var) || ((Var *) a)->varno != rti || ((Var *) a)->varlevelsup != 0 || ((Var *) a)->varattno != 1 || !IsA(b, Const) ||
		((Const *) b)->constisnull)
		return NULL;
	return TextDatumGetCString(((Const *) b)->constvalue);
#else
	return NULL;
#endif
}

bool
agg_base(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	Query	   *parse = root->parse;
	IndexOptInfo *ii;
	Bitmapset  *allrtis = NULL;
	List	   *live_rtis = NIL,
			   *joined = NIL;
	int			nin = 0;

	scan_from((Node *) parse->jointree, fs);
	if (!fs->ok)
		return false;
	/* Join removal leaves the original FROM tree in place, but removes its base relations. */
	foreach_int(rti, fs->rtis)
		if (root->simple_rel_array[rti] != NULL)
			live_rtis = lappend_int(live_rtis, rti);
	fs->rtis = live_rtis;
	c->root = root;
	c->group = -1;
	c->nulling = fs->ojrels;
	foreach_int(rti, fs->rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		allrtis = bms_add_member(allrtis, rti);
		if (e->rtekind == RTE_RELATION)
		{
			bool		part = e->inh && e->relkind == RELKIND_PARTITIONED_TABLE;

			if ((e->inh && !part) || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW && !part))
				return false;
			if (c->relid == 0)
				c->relid = rti;
			else if (part || c->joinless)
				return false;
			else
				joined = lappend_int(joined, rti);
		}
		else if (e->rtekind == RTE_FUNCTION && e->lateral && c->func_rti == 0 && !e->funcordinality && list_length(e->functions) == 1 &&
				 func_kind_of(linitial_node(RangeTblFunction, e->functions)) != FK_NONE)
		{
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
			/* the scan unnests the elements that pass: a row without one is no row of an outer join (it keeps NULL columns) */
			foreach_ptr(LeftJoin, lj, fs->lefts)
				if (lj->rti == rti && (c->func_key = left_each_key(lj, rti, c->func_kind)) == NULL)
					return false;
		}
		else if (input_rel == NULL || c->joinless ||
				 (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION && e->rtekind != RTE_CTE))
			return false;
		else
			joined = lappend_int(joined, rti);
	}
	if (c->relid == 0)
		return false;
	/* a plan joined to the scan by an outer join keeps its own rows: the scanned table is never the nullable side */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == c->relid)
			return false;
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	/* the joins come after the source: planning the sub-selects of a query with no column index to scan would be a waste */
	if (c->rel == NULL || !find_source(c, &ii))
		return false;
	c->nsiblings = list_length(joined) + (c->func_rti != 0);
	foreach_int(rti, joined)
		if (!add_join(c, fs, rti))
			return false;
	if (input_rel != NULL)
	{
		if (!bms_is_member(c->relid, input_rel->relids))
			return false;
		for (int m = -1; (m = bms_next_member(input_rel->relids, m)) >= 0;)
			if (rt_fetch(m, parse->rtable)->rtekind != RTE_JOIN)
			{
				if (!bms_is_member(m, allrtis))
					return false;
				nin++;
			}
		if (nin + list_length(c->joins) < list_length(fs->rtis))
			return false;
	}
	/* the rows of a preserved side that nothing matched are known at the end of a single scan of one join */
	if (fs->ojrels && (input_rel == NULL || c->nparts > 0))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind) && jc != llast(c->joins))
			return false;
	build_attexprs(c);
	if (c->func_rti)
	{
		int			leaf = group_leaf_of(c, linitial(((FuncExpr *) c->func->funcexpr)->args), func_leaf_kind(c->func_kind));

		if (leaf < 0)
			return false;
		if (c->func_key)
			c->func_obj = leaf;
		else
		{
			c->group = leaf;
			c->used = bms_add_member(c->used, leaf);
		}
	}
	return true;
}

/* agg_base, then the other table of an outer join of two tables as the scanned one when the first did not give a plan (a preserved side) */
static bool
agg_base_either(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	List	   *planned;

	if (agg_base(c, root, input_rel, fs))
		return true;
	if (!fs->flippable)
		return false;
	planned = c->planned;
	memset(c, 0, sizeof(AggCtx));
	c->planned = planned;
	*fs = (FromScan) {.ok = true, .flip = true};
	return agg_base(c, root, input_rel, fs);
}

SiftAggSpec *
agg_spec_new(AggCtx *c)
{
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->nparts = c->nparts;
	s->partheap = c->partheap;
	s->partindex = c->partindex;
	s->group = c->group;
	return s;
}

/* the share of the heap pages the TID range of the conjuncts leaves, on the first of them (zone_kept) */
static void
tid_share(AggCtx *c, SiftAggSpec *s)
{
	uint64		lo,
				hi;

	if (!sift_aggspec_tid_range(s, &lo, &hi))
		return;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
		{
			s->pred[i]->sel = lo >= hi ? 0.0 : Min(((double) ((hi - 1) >> 16) - (double) (lo >> 16) + 1.0) / Max(rel_pages(c), 1.0), 1.0);
			return;
		}
}

/* the WHERE of the scan: bitmap predicates, and the conjuncts over the joined rows */
bool
agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	List	   *quals = fs->quals,
			   *joined = NIL,
			   *scan_quals = fs->scan_quals,
			   *scan_post = NIL;
	StringInfoData txt;

	if (!setup_joins(c, &quals, &joined, &scan_quals, &scan_post))
		return false;
	quals = list_concat(quals, scan_quals);
	s->pred = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	s->post = palloc(Max(list_length(quals) + list_length(joined) + list_length(scan_post), 1) * sizeof(SiftAggPred *));
	initStringInfo(&txt);
	foreach_ptr(Node, q, quals)
	{
		Node	   *e = normalize_expr(c, q);
		SiftAggPred *p = compile_typeof(c, e);

		if (p == NULL)
			p = compile_wide(c, e);
		if (p == NULL)
			p = compile_tid(c, e);
		if (p == NULL)
			p = compile_pred(c, e);
		if (p == NULL || unsupported_clause(c, q))
			return false;
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	foreach_ptr(Node, q, scan_post)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->nprepost = preserved_join(c) ? s->npost : 0;
	foreach_ptr(Node, q, joined)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->desc_quals = txt.data;
	tid_share(c, s);
	return true;
}

static void
spec_deps(PlannerInfo *root, SiftAggSpec *s)
{
	if (s->search)
		record_deps(root, s->search);
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	for (int i = 0; i < s->ncombo; i++)
		for (int k = 0; k < s->combo[i].nslots; k++)
			record_deps(root, s->combo[i].slot[k].expr);
	for (int i = 0; i < s->njoin; i++)
		for (int k = 0; k < s->join[i].nkey; k++)
			record_deps(root, s->join[i].keyexpr[k]);
	for (int a = 0; a < s->narms; a++)
	{
		for (int i = 0; i < s->arm[a].nfilter; i++)
			pred_deps(root, s->arm[a].filter[i]);
		for (int i = 0; i < s->arm[a].ninner; i++)
			pred_deps(root, s->arm[a].inner[i].filter);
	}
}

/* the groups the scan makes: those of the keys, or of each grouping set */
/* what PostgreSQL has statistics of for a key: the column behind an expression that denotes a stored generated column, else the expression */
static Node *
stats_expr(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	return att > 0 ? list_nth(c->attexpr, att - 1) : e;
}

static double
set_groups(AggCtx *c, PlannerInfo *root, SiftAggSpec *s, List *keys, double rows)
{
	double		n = 0;

	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		List	   *sub = NIL;
		double		bucketed = 1.0,
					g;

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
		/* Min and Max evaluate their arguments more than once: the estimate is made once */
		g = sub == NIL ? 1.0 : estimate_num_groups(root, sub, rows, NULL, NULL);
		n += Min(rows, Max(g, 1.0) * bucketed);
	}
	return n;
}

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the whole
 * query is safe to run in a worker (the workers evaluate the spec's expressions) and the spec's tables can be merged
 * (sift_aggspec_parallel_ok).
 */
static int
parallel_workers(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	/* the relations' own flags, as PostgreSQL's grouping does: the query-wide hazard counts the sub-selects it pulled up into semi joins as restricted */
	if (!sift_aggspec_parallel_ok(s) || c->nsubplan > 0 || !c->root->glob->parallelModeOK || !output_rel->consider_parallel ||
		!input_rel->consider_parallel || !c->rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return 0;
	/* of the equal keys 1.0 and 1.00 the group shows the one of the first row, which the workers' tables merge in no order */
	for (int i = 0; s->nkeys > 0 && i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			return 0;
	return compute_parallel_worker(c->rel, rel_pages(c), -1, max_parallel_workers_per_gather);
}

/*
 * Does the spec read a leaf that holds values the index cannot store? The scan raises an error when a row of its snapshot has one
 * (check_overflow), the normal plan reads them from the heap: the query keeps its plan. Unless the source reads the rows again as SQL reads
 * them (spec_recheck). A numeric leaf is not one: its only such value is NaN, which the scan evaluates (SiftSrcVec.ovf). Nor is the leaf of a
 * jsonb_typeof conjunct (SAP_TYPE): the source keeps the rows of the types and tells the type of each value it cannot store.
 */
static bool
reads_overflow(AggCtx *c, const SiftAggSpec *s)
{
	Bitmapset  *typed = NULL,
			   *leaves;
	int			m = -1;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE)
			typed = bms_add_member(typed, s->pred[i]->leaf);
	leaves = bms_union(c->used, c->func_ovf);
	for (int j = 0; j < c->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) >= 0)
			leaves = bms_add_member(leaves, vleaf_source(c, j));
	while ((m = bms_next_member(leaves, m)) >= 0)
		if (c->stats.ovf[m] && !bms_is_member(m, typed) && spec_recheck(c, s, m) == SRK_NONE)
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not read \"%s\": some unit holds values the leaf cannot store", leaf_name(c, m)),
							 errhint("docs/COL-DIRTY-UNITS.md")));
			return true;
		}
	return false;
}

/* the leaves the spec reads and the combos; false when the spec cannot be run. keys: the group expressions of root's query */
bool
spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys)
{
	if (c->other_group || types_guard_reads(c))
		return false;
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return false;
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return false;
	if (list_length(c->combos) > SIFT_AGG_MAX_COMBOS || list_length(c->joins) > SIFT_AGG_MAX_JOINS)
		return false;
	typeof_reads(c, s);
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	if (reads_overflow(c, s))
		return false;
	finish_leaves(c, s);
	finish_combos(c, s);
	s->est_groups = set_groups(c, root, s, keys, input_rel->rows);
	if (c->search)
		s->search = c->search->spec;

	spec_deps(root, s);
	return true;
}

/*
 * A set filter bound as one array parameter (`col = ANY($1)`) has no length in a generic plan: PostgreSQL assumes 10 elements and prices an
 * index or bitmap scan for them, while the scan of this node takes the whole array into a byte table when it starts and costs the same
 * for 10 or 100,000 elements. The paths that fetch the rows of such a set one by one are charged the elements sift.agg_param_set_size
 * assumes (their cost grows with the set), and only when this node can run the query, so that a query it cannot take keeps the plan it
 * has. A custom plan sees the array and needs none of this.
 */
int			sift_agg_param_set_size = 1000;
int			sift_agg_lookup_max_rows = 1000000;

static double
param_set_ratio(AggCtx *c)
{
	double		f = 1.0;

	if (sift_agg_param_set_size <= 0)
		return 1.0;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		ScalarArrayOpExpr *sa;
		Node	   *arr;
		double		n0;

		if (!IsA(ri->clause, ScalarArrayOpExpr))
			continue;
		sa = (ScalarArrayOpExpr *) ri->clause;
		if (!sa->useOr || list_length(sa->args) != 2)
			continue;
		arr = strip_relabel(lsecond(sa->args));
		if (!IsA(arr, Param) || ((Param *) arr)->paramkind != PARAM_EXTERN || !IsA(strip_relabel(linitial(sa->args)), Var) ||
			match_leaf(c, linitial(sa->args)) < 0)
			continue;
		n0 = estimate_array_length(c->root, arr);
		if (sift_agg_param_set_size > n0)
			f *= sift_agg_param_set_size / n0;
	}
	return Min(f, Max(c->rel->tuples / Max(c->rel->rows, 1.0), 1.0));
}

/* the scan of the rows of the table a plan reads them with, when it is an index or bitmap scan under aggregation, sorts and gathers */
static Path *
row_scan_of(Path *p)
{
	for (;;)
		switch (nodeTag(p))
		{
			case T_IndexPath:
			case T_BitmapHeapPath:
				return p;
			case T_AggPath:
				p = ((AggPath *) p)->subpath;
				break;
			case T_GroupPath:
				p = ((GroupPath *) p)->subpath;
				break;
			case T_SortPath:
				p = ((SortPath *) p)->subpath;
				break;
			case T_IncrementalSortPath:
				p = ((IncrementalSortPath *) p)->spath.subpath;
				break;
			case T_GatherPath:
				p = ((GatherPath *) p)->subpath;
				break;
			case T_GatherMergePath:
				p = ((GatherMergePath *) p)->subpath;
				break;
			case T_ProjectionPath:
				p = ((ProjectionPath *) p)->subpath;
				break;
			default:
				return NULL;
		}
}

static void
param_set_penalty(AggCtx *c, RelOptInfo *output_rel)
{
	double		f = param_set_ratio(c);

	if (f <= 1.0)
		return;
	foreach_ptr(Path, p, output_rel->pathlist)
		if (row_scan_of(p) != NULL)
		{
			p->startup_cost *= f;
			p->total_cost *= f;
		}
}

/* the cost; adds the paths to output_rel */
void
agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *keys, List *scan_tlist)
{
	int			nworkers;

	if (!spec_finish(c, s, root, input_rel, keys) || !first_fits(s) || !child_paths(c, s))
		return;
	param_set_penalty(c, output_rel);
	add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, 0);
	nworkers = preagg_share(c, s) >= AGG_PREAGG_EXACT ? 0 : parallel_workers(c, s, input_rel, output_rel);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, nworkers);
}

/* the index among the group columns of the one with the sortgroupref ref */
static int
key_of_ref(Query *parse, int ref)
{
	foreach_node(SortGroupClause, sgc, parse->groupClause)
		if (sgc->tleSortGroupRef == ref)
			return foreach_current_index(sgc);
	elog(ERROR, "sift: grouping column %d not found", ref);
}

/* GROUPING(columns): the keys of its arguments; the executor makes its bitmask of the set of each group */
bool
compile_grouping(Query *parse, GroupingFunc *g, SiftAggAgg *out)
{
	if (g->agglevelsup != 0 || parse->groupingSets == NIL)
		return false;
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

/* Does the grouping have a key that is not a constant? */
static bool
groupClause_has_nonconst(PlannerInfo *root, List *groupClause)
{
	foreach_node(SortGroupClause, sgc, groupClause)
		if (!IsA(get_sortgroupclause_expr(sgc, root->processed_tlist), Const))
			return true;
	return false;
}

/* ---------- DISTINCT ON: the first row of a group by the ORDER BY ---------- */

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */

/*
 * Do the sort columns hold the columns of a unique index of the table (plain columns, NOT NULL or with NULLs not distinct), so that no two
 * rows tie? The first row of a group by the ORDER BY is then the same in every plan: a tie would be PostgreSQL's sort's choice.
 */
static bool
sort_is_total(AggCtx *c, List *sortexprs)
{
	Bitmapset  *atts = NULL;
	Relation	rel;
	bool		total = false;

	foreach_ptr(Node, e, sortexprs)
	{
		Var		   *v = (Var *) strip_relabel(e);

		if (IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0)
			atts = bms_add_member(atts, v->varattno);
	}
	rel = table_open(c->heapoid, NoLock);
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		HeapTuple	it = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix = (Form_pg_index) GETSTRUCT(it);
		bool		all = ix->indisunique && ix->indimmediate && ix->indisvalid && ix->indnkeyatts > 0 &&
			heap_attisnull(it, Anum_pg_index_indpred, NULL) && heap_attisnull(it, Anum_pg_index_indexprs, NULL);

		for (int i = 0; all && i < ix->indnkeyatts; i++)
		{
			int			att = ix->indkey.values[i];
			HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(c->heapoid), Int16GetDatum(att));

			all = bms_is_member(att, atts) && (ix->indnullsnotdistinct || (HeapTupleIsValid(at) && ((Form_pg_attribute) GETSTRUCT(at))->attnotnull));
			if (HeapTupleIsValid(at))
				ReleaseSysCache(at);
		}
		ReleaseSysCache(it);
		total |= all;
	}
	table_close(rel, NoLock);
	return total;
}

/*
 * The value of e in the first row of a group by the sort columns `more`: an aggregate that only carries the argument and the sort keys of
 * the generic aggregates (its function is array_agg, whose catalog entry the compile of a generic aggregate reads); the executor keeps
 * the best row as the rows pass (SiftAggGen.first), and the group's value is e, of its own type.
 */
static Aggref *
first_aggref(AggCtx *c, Node *e, List *more)
{
	Aggref	   *a;
	Oid			t = exprType(e);
	List	   *args = list_make1(makeTargetEntry((Expr *) copyObject(e), 1, NULL, false)),
			   *order = NIL;

	if (type_is_array(t) || arg_joined(c, e))
		return NULL;
	foreach_node(SortGroupClause, sgc, more)
	{
		TargetEntry *k = makeTargetEntry((Expr *) copyObject(get_sortgroupclause_expr(sgc, c->root->processed_tlist)), list_length(args) + 1, NULL, true);
		SortGroupClause *o = copyObject(sgc);

		o->tleSortGroupRef = k->ressortgroupref = k->resno;
		args = lappend(args, k);
		order = lappend(order, o);
	}
	a = makeNode(Aggref);
	a->aggfnoid = F_ARRAY_AGG_ANYNONARRAY;
	a->aggtype = t;
	a->aggcollid = a->inputcollid = exprCollation(e);
	a->aggtranstype = INTERNALOID;
	a->aggargtypes = list_make1_oid(t);
	a->args = args;
	a->aggorder = order;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return a;
}

/*
 * The output of DISTINCT ON: the ON columns are the group keys; any other expression of the output (the select list, and the sort columns
 * the sort above reads) is the value of the first row of its group, one aggregate for each, in the scan tuple at the place of the expression.
 */
bool
first_aggs(AggCtx *c, PathTarget *target, List *keys, List *more, List **firsts)
{
	/* the normal plan evaluates a volatile function for every row, before it picks one */
	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	foreach_ptr(Node, e, target->exprs)
	{
		OutWalk		w = {keys, NIL, false};
		Aggref	   *a;
		bool		have = false;

		out_walker(e, &w);
		if (!w.bad)
			continue;
		if ((a = first_aggref(c, e, more)) == NULL)
			return false;
		foreach_ptr(Aggref, f, *firsts)
			have |= equal(f, a);
		if (!have)
			*firsts = lappend(*firsts, a);
	}
	return true;
}

/* a DISTINCT ON aggregate keeps the best row instead of sorting the rows of the group; the first of them compares the sort keys, the others follow its answer */
void
first_mode(SiftAggAgg *out, int nsort, bool leader)
{
	SiftAggGen *g = out->gen;

	g->sorted = false;
	g->first = true;
	g->parallel = true;
	g->cost = leader ? AGG_GEN_FIRST + nsort * AGG_GEN_FIRST_KEY : AGG_GEN_FOLLOW;
	out->desc = psprintf("first_row%s", out->desc + strlen("array_agg"));
}

/* do the best rows of the groups fit the memory a hash aggregate gets? */
bool
first_fits(SiftAggSpec *s)
{
	double		bytes = 0;

	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN && s->agg[i].gen->first)
		{
			const SiftAggGen *g = s->agg[i].gen;

			bytes += FIRST_GROUP_BYTES;
			for (int k = 0; k < g->nargs; k++)
				bytes += sizeof(Datum) + 1 + get_typavgwidth(g->arg[k].typid, -1);
		}
	return bytes == 0 || s->est_groups * bytes <= (double) get_hash_memory_limit();
}

/* the sort columns after the DISTINCT ON columns, when the ORDER BY is a total order of the rows */
bool
distinct_on_more(AggCtx *c, Query *parse, List **more)
{
	List	   *sortexprs = NIL;

	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		sortexprs = lappend(sortexprs, get_sortgroupclause_expr(sgc, c->root->processed_tlist));
		if (arg_joined(c, llast(sortexprs)))
			return false;
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS && sort_is_total(c, sortexprs);
}

/*
 * The rows of one scan grouped by `groupClause` (the GROUP BY of the query, or the columns of its SELECT DISTINCT, which is a grouping with no
 * aggregate): the SiftAgg path of output_rel, whose rows are those of `target` (the grouping target of the query, or its distinct target).
 */
static void
group_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, GroupPathExtraData *gextra, List *groupClause)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL,
			   *more = NIL,
			   *firsts = NIL;
	bool		lead = true;

	if (!agg_base_either(c, root, input_rel, &fs))
		return;
	s = agg_spec_new(c);
	if (c->nparts == 0)
		c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, false);
	if (!agg_quals(c, s, &fs))
		return;

	/* keys */
	if (list_length(groupClause) + (parse->groupingSets != NIL) > SIFT_AGG_MAX_KEYS)
		return;
	s->key = palloc0((list_length(groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		/* a constant adds no group: the output keeps it as the constant it is (not with grouping sets, whose empty set has no key, and not alone: an empty input has no group) */
		if (IsA(e, Const) && parse->groupingSets == NIL && groupClause_has_nonconst(root, groupClause))
			continue;
		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* DISTINCT ON: the other columns are the first row's */
	if (parse->hasDistinctOn)
	{
		if (!distinct_on_more(c, parse, &more) || !first_aggs(c, target, ow.keys, more, &firsts))
			return;
		ow.aggs = firsts;
	}
	else
	{
		/* aggregates, in the output and the HAVING clause */
		foreach_ptr(Node, e, target->exprs)
			out_walker(e, &ow);
	}
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		bool		first = list_member_ptr(firsts, a);

		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
			return;
		if (first)
		{
			first_mode(&s->agg[s->naggs], list_length(more), lead);
			lead = false;
		}
		scan_tlist = lappend(scan_tlist, makeTargetEntry(first ? (Expr *) linitial_node(TargetEntry, ((Aggref *) a)->args)->expr : (Expr *) a,
														 list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	agg_finish(c, s, root, input_rel, output_rel, target, ow.keys, scan_tlist);
}

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	Query	   *parse = root->parse;

	if (stage == UPPERREL_ORDERED)
	{
		topn_paths(root, input_rel, output_rel);
		return;
	}
	if (!sift_enable_agg || parse->commandType != CMD_SELECT || parse->hasTargetSRFs || root->rowMarks != NIL || parse->hasRecursive)
		return;
	/* SELECT DISTINCT of a plain scan is a group-by with no aggregate; over a grouped or windowed query it is not the scan's */
	if (stage == UPPERREL_DISTINCT)
	{
		if (parse->distinctClause != NIL && root->upper_targets[UPPERREL_DISTINCT] != NULL &&
			!(parse->hasAggs || parse->groupClause || parse->groupingSets || parse->hasWindowFuncs || parse->havingQual))
			group_paths(root, input_rel, output_rel, root->upper_targets[UPPERREL_DISTINCT], NULL, parse->distinctClause);
		return;
	}
	if (stage != UPPERREL_GROUP_AGG || parse->hasDistinctOn || !(parse->hasAggs || parse->groupClause || parse->groupingSets))
		return;
	/* a window function is planned above the grouping stage: this stage is the same with or without one */
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		if (!parse->hasWindowFuncs)
			sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
		return;
	}
	group_paths(root, input_rel, output_rel, output_rel->reltarget, (GroupPathExtraData *) extra, parse->groupClause);
}

/* ---------- top-N: ORDER BY <columns> LIMIT n ---------- */

/* The rows a plan assumes for a LIMIT it cannot know (a parameter of a generic plan): a page of a list */
#define TOPN_ASSUMED_ROWS	1000

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
 * LIMIT and OFFSET of the query: the rows the top-N must produce, as far as the planner knows them; 0 when there is no bound it can use.
 * A parameter of a generic plan is not known: the scan gets its value when it starts, and handles any (agg/topn.h), so the plan assumes
 * a few rows; a value the planner knows, past the cap, keeps the normal plan.
 */
static int64
topn_rows(Query *parse, PlannerInfo *root)
{
	int64		count,
				offset;

	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return 0;
	count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	if (count < 1 || offset < 0 || count > SIFT_TOPN_MAX_ROWS || offset > SIFT_TOPN_MAX_ROWS)
		return 0;
	return count + offset <= SIFT_TOPN_MAX_ROWS ? count + offset : 0;
}

/* The sort column i of the query as a leaf the top-N can order by, in the direction of its operator */
static bool
topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt)
{
	Node	   *e = (Node *) get_sortgroupclause_expr(sgc, c->root->processed_tlist);
	Oid			opfamily,
				opcintype;
#if PG_VERSION_NUM >= 180000
	CompareType strategy;
#define TOPN_DESC	COMPARE_GT
#else
	int16		strategy;
#define TOPN_DESC	BTGreaterStrategyNumber
#endif

	if (!get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
		return false;
	*leaf = match_leaf(c, normalize_expr(c, e));
	if (*leaf < 0 || !sift_topn_key_ok(leaf_of(c, *leaf)) || opcintype != leaf_of(c, *leaf)->typid ||
		opfamily != get_opclass_family(GetDefaultOpClass(opcintype, BTREE_AM_OID)))
		return false;
	*desc = strategy == TOPN_DESC;
	appendStringInfo(txt, "%s%s%s", txt->len ? ", " : "", leaf_name(c, *leaf), *desc ? " DESC" : "");
	if (sgc->nulls_first != *desc)
		appendStringInfoString(txt, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	use_leaf(c, *leaf);
	return true;
}

/*
 * A first sort column that follows the physical order of the table (pg_statistic correlation) lets the scan stop at the end its best values
 * are at: it reads a window of superblocks, and more only while the rows it needs are not in it. Otherwise it reads them all.
 */
static double
topn_fraction(AggCtx *c, Node *key, double matches, int64 limit)
{
	Node	   *x = strip_relabel(key);
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0,
				nsb = Max(c->stats.nunits, 1.0);

	if (!IsA(x, Var))
		return 1.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.nnumbers > 0 ? sslot.numbers[0] : 0;
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	if (fabs(corr) < 0.9)
		return 1.0;
	return Min(1.0, (8.0 + 2.0 * nsb * (double) limit / Max(matches, 1.0)) / nsb);
}

/*
 * The scan tuple of the top-N is the heap columns the output reads, in custom_scan_tlist: their attribute numbers go in the spec, and
 * the node's tlist (PostgreSQL's projection) computes the rest from them.
 */
static bool
topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno)
{
	List	   *vars;

	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	vars = pull_var_clause((Node *) target->exprs, PVC_RECURSE_PLACEHOLDERS);
	foreach_node(Var, v, vars)
	{
		if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
			return false;
		if (list_member(*scan_tlist, v) || tlist_member((Expr *) v, *scan_tlist))
			continue;
		if (list_length(*scan_tlist) >= MaxHeapAttributeNumber)
			return false;
		attno[list_length(*scan_tlist)] = v->varattno;
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) copyObject(v), list_length(*scan_tlist) + 1, NULL, false));
	}
	return true;
}

void
topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	CustomPath *cp;
	StringInfoData txt;
	List	   *scan_tlist = NIL;
	uint64		tlo,
				thi;
	int			leaf[SIFT_TOPN_MAX_KEYS];
	bool		desc[SIFT_TOPN_MAX_KEYS],
				nulls_first[SIFT_TOPN_MAX_KEYS];
	int		   *attno;
	int64		rows = topn_rows(parse, root);
	int			nkey = list_length(parse->sortClause);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (!sift_enable_agg || !sift_enable_topn || rows == 0 || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || nkey > SIFT_TOPN_MAX_KEYS ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause || parse->setOperations || parse->hasSubLinks || parse->hasRecursive || root->rowMarks != NIL)
		return;
	c->joinless = true;
	if (!agg_base(c, root, input_rel, &fs) || c->nparts > 0 || c->joins != NIL || c->func_rti != 0 || fs.lefts != NIL ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
		return;
	s = agg_spec_new(c);
	c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs) || s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
		return;
	initStringInfo(&txt);
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		if (!topn_key(c, sgc, &leaf[j], &desc[j], &txt))
			return;
		nulls_first[j] = sgc->nulls_first;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (!topn_columns(c, target, &scan_tlist, attno) || !spec_finish(c, s, root, input_rel, NIL))
		return;
	c->topn_frac = topn_fraction(c, (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist),
								 input_rel->rows, rows);
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min((double) rows, Max(input_rel->rows, 1.0)));
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
}

/* ---------- two level: an outer aggregate over aggregates grouped by row ---------- */

#define AGG_MAX_ARMS	4

typedef struct CteSub
{
	Index		rti;			/* 0: the arm reads the table itself */
	List	   *tlist;			/* the target list of the common table expression */
} CteSub;

static Node *
cte_sub_mutator(Node *n, CteSub *s)
{
	if (n == NULL)
		return NULL;
	if (s->rti != 0 && IsA(n, Var) && ((Var *) n)->varno == s->rti && ((Var *) n)->varlevelsup == 0)
	{
		TargetEntry *te = get_tle_by_resno(s->tlist, ((Var *) n)->varattno);

		return te ? (Node *) copyObject(te->expr) : n;
	}
	return expression_tree_mutator(n, cte_sub_mutator, s);
}

/* what an arm (a subquery of the outer query) is, in the terms of the table it reads */
typedef struct ArmIn
{
	CteSub		sub;
	List	   *cols;			/* the expressions of its output columns */
	List	   *group;			/* its group expressions */
	List	   *gsgc;			/* their SortGroupClauses */
	List	   *used;			/* the group expressions that are an outer key */
	List	   *quals;			/* its WHERE, when it reads a common table expression */
	Node	   *id;				/* the group expression that is unique per row */
	SiftAggArm *out;
} ArmIn;

/* the root of the common table expression a CTE scan of `root` reads; NULL if it is not one the scan can look through */
static PlannerInfo *
cte_root(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;
	CommonTableExpr *cte = NULL;

	for (Index up = rte->ctelevelsup; up > 0 && cteroot != NULL; up--)
		cteroot = cteroot->parent_root;
	if (cteroot == NULL)
		return NULL;
	foreach_node(CommonTableExpr, x, cteroot->parse->cteList)
	{
		if (strcmp(x->ctename, rte->ctename) == 0)
		{
			cte = x;
			break;
		}
		ndx++;
	}
	if (cte == NULL || cte->cterecursive || cte->search_clause || cte->cycle_clause || ndx >= list_length(cteroot->cte_plan_ids))
		return NULL;
	return list_nth(root->glob->subroots, list_nth_int(cteroot->cte_plan_ids, ndx) - 1);
}

/* the subqueries of a UNION ALL: a member that is itself a UNION ALL (a subquery of arms in an arm's place) gives its own */
static bool
append_arms(PlannerInfo *root, Index parent, List **arms)
{
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != parent)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL)
			return false;
		foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
			if (!rinfo->pseudoconstant)
				return false;
		foreach_ptr(Node, tv, ai->translated_vars)
			if (tv == NULL || !IsA(tv, Var) || ((Var *) tv)->varno != ai->child_relid || ((Var *) tv)->varattno != ++i)
				return false;
		if (rel->subroot != NULL)
			*arms = lappend(*arms, rel->subroot);
		else if (!rt_fetch(ai->child_relid, root->parse->rtable)->inh || !append_arms(root, ai->child_relid, arms))
			return false;
	}
	return true;
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
static List *
outer_arms(PlannerInfo *root, Index srti, List **gates)
{
	List	   *arms = NIL;
	RelOptInfo *parent = root->simple_rel_array[srti];

	if (parent == NULL)
		return NIL;
	/* the conditions without a column stay above the arms as PostgreSQL's one-time filter: the scan takes them as its own gate */
	foreach_node(RestrictInfo, rinfo, parent->baserestrictinfo)
		if (rinfo->pseudoconstant)
			*gates = lappend(*gates, rinfo->clause);
		else if (!rt_fetch(srti, root->parse->rtable)->inh)
			return NIL;
	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return parent->subroot ? list_make1(parent->subroot) : NIL;
	return append_arms(root, srti, &arms) ? arms : NIL;
}

/*
 * The table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root (its FROM is the scan itself:
 * a table with its unnests and lookups, which is what an arm over a common table expression that PostgreSQL inlined looks like); NULL if
 * the arm is not a simple aggregate
 */
static PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL)
		return NULL;
	in->sub.rti = 0;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return arm;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

/*
 * Two arms that read their own copy of one scan (a common table expression that PostgreSQL inlined into each): the same tables and the same
 * WHERE. The entries of pulled up subqueries and of the grouping belong to their arm, and the column names a plan prunes differ: none counts.
 */
static bool
arm_private_rte(RTEKind kind)
{
#if PG_VERSION_NUM >= 180000
	if (kind == RTE_GROUP)
		return true;
#endif
	return kind == RTE_SUBQUERY;
}

static bool
same_scan(PlannerInfo *a, PlannerInfo *b)
{
	ListCell   *la,
			   *lb;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) || !equal(a->parse->jointree, b->parse->jointree))
		return false;
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)))
			return false;
	}
	return true;
}

static bool
null_const(Node *e)
{
	return IsA(e, Const) && ((Const *) e)->constisnull;
}

/* `operand op constant` (either side) on an int8 or float8 operand: the operand, the comparison and the constant in *out */
static bool
cond_split(Node *q, Node **operand, SiftAggCond *out)
{
	Oid			lt,
				rt,
				vtype;
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *k;
	int			cmp;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	if ((cmp = builtin_cmp(op->opno, &lt, &rt)) < 0)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(r, Const))
	{
		*operand = l;
		k = (Const *) r;
	}
	else if (IsA(l, Const))
	{
		*operand = r;
		k = (Const *) l;
		cmp = commute(cmp);
	}
	else
		return false;
	vtype = exprType(*operand);
	out->op = cmp;
	if (k->constisnull)
		return false;
	if (vtype == FLOAT8OID && k->consttype == FLOAT8OID)
	{
		out->isint = false;
		out->cd = DatumGetFloat8(k->constvalue);
	}
	else if (vtype == INT8OID && (k->consttype == INT8OID || k->consttype == INT4OID || k->consttype == INT2OID))
	{
		out->isint = true;
		out->ci = k->consttype == INT8OID ? DatumGetInt64(k->constvalue) : k->consttype == INT4OID ?
			DatumGetInt32(k->constvalue) : DatumGetInt16(k->constvalue);
	}
	else
		return false;
	return true;
}

/* `column op constant` on an output column of the arms: the column in out->col */
static bool
outer_cond(Node *q, Index srti, SiftAggCond *out)
{
	Node	   *x;

	if (!cond_split(q, &x, out) || !IsA(x, Var) || ((Var *) x)->varno != srti || ((Var *) x)->varlevelsup != 0)
		return false;
	out->col = ((Var *) x)->varattno - 1;
	return true;
}

/* the inner aggregate e of the arm, added to its inner aggregates: its index; -1 if the arm cannot give it */
static int
arm_inner_agg(AggCtx *c, ArmIn *in, Node *e, Oid want_type)
{
	SiftAggArm *a = in->out;
	SiftAggAgg *ia = &a->inner[a->ninner];

	if (!IsA(e, Aggref) || exprType(e) != want_type || !compile_agg(c, (Aggref *) e, ia, -1))
		return -1;
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
			/* the expression is evaluated for every tuple of the scan: an arm's own WHERE or a FILTER would keep it off the tuples they drop */
			if (ia->restype != FLOAT8OID || in->quals != NIL || ia->filter != NULL)
				return -1;
			break;
		default:
			return -1;
	}
	return a->ninner++;
}

/* the inner aggregate that is the arm's column `col`, compiled once: its index in the arm; -1 if the arm cannot give it */
static int
arm_inner(AggCtx *c, ArmIn *in, int col, Oid want_type)
{
	SiftAggArm *a = in->out;

	if (a->colsrc[col] < 0)
		a->colsrc[col] = arm_inner_agg(c, in, list_nth(in->cols, col), want_type);
	return a->colsrc[col];
}

/* a conjunct of the arm's HAVING: `inner aggregate op constant` (in terms of the scanned table), with the aggregate's index as cond.col */
static bool
arm_having(AggCtx *c, ArmIn *in, Node *q, SiftAggCond *out)
{
	Node	   *x;

	return cond_split(q, &x, out) && (out->col = arm_inner_agg(c, in, x, exprType(x))) >= 0;
}

/* the conditions of the outer query without a column, which PostgreSQL left above the arms, as conjuncts of the scan */
static bool
arm_gates(AggCtx *c, SiftAggSpec *s, List *gates)
{
	s->pred = repalloc(s->pred, Max(s->npred + list_length(gates), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, gates)
	{
		SiftAggPred *p = compile_pred(c, normalize_expr(c, q));

		if (p == NULL || p->kind != SAP_GATE)
			return false;
		s->pred[s->npred++] = p;
		s->desc_quals = psprintf("%s%s%s", s->desc_quals, *s->desc_quals ? " AND " : "", p->desc);
	}
	return true;
}

typedef struct HavingSub
{
	AggCtx	   *c;
	ArmIn	   *in;
	List	   *src;			/* inner aggregate of each Param made */
	bool		bad;
} HavingSub;

/* the inner aggregates of a HAVING conjunct become Params; a column (a group expression) or a volatile function is not allowed */
static Node *
having_mutator(Node *n, HavingSub *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		int			ai = h->bad || list_length(h->src) >= SIFT_AGG_MAX_PARAMS ? -1 : arm_inner_agg(h->c, h->in, n, exprType(n));
		Param	   *p = makeNode(Param);

		if (ai < 0)
			h->bad = true;
		p->paramkind = PARAM_EXEC;
		p->paramid = list_length(h->src);
		p->paramtype = exprType(n);
		p->paramtypmod = -1;
		p->paramcollid = InvalidOid;
		p->location = -1;
		h->src = lappend_int(h->src, ai);
		return (Node *) p;
	}
	if (IsA(n, Var) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		h->bad = true;
	return expression_tree_mutator(n, having_mutator, h);
}

/* the conjuncts of the arm's HAVING that are not `inner aggregate op constant`: PostgreSQL evaluates them on the values of each cell */
static bool
arm_having_expr(AggCtx *c, ArmIn *in, List *quals)
{
	HavingSub	h = {c, in, NIL, false};
	SiftAggArm *a = in->out;

	if (contain_volatile_functions((Node *) quals))
		return false;
	a->hexpr = having_mutator((Node *) make_ands_explicit(quals), &h);
	a->nhp = list_length(h.src);
	a->hsrc = palloc(Max(a->nhp, 1) * sizeof(int));
	foreach_int(ai, h.src)
		a->hsrc[foreach_current_index(ai)] = ai;
	return !h.bad;
}

static bool
count_aggs_walker(Node *n, int *count)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		(*count)++;
	return expression_tree_walker(n, count_aggs_walker, count);
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id), count(*), with a FILTER of comparisons of columns */
static bool
outer_agg(AggCtx *c, SiftAggSpec *s, Aggref *oa, Index srti, ArmIn *arms, int narms, SiftAggAgg *out)
{
	Oid			fid = oa->aggfnoid;
	Node	   *arg = NULL;
	Var		   *v = NULL;
	List	   *conds = oa->aggfilter ? make_ands_implicit(oa->aggfilter) : NIL;
	int			nc = 0;

	if (oa->aggkind != AGGKIND_NORMAL || oa->aggorder != NIL || oa->agglevelsup != 0 || oa->aggvariadic || contain_volatile_functions((Node *) oa))
		return false;
	out->leaf = -1;
	out->col = -1;
	out->restype = oa->aggtype;
	out->cond = palloc0(Max(list_length(conds), 1) * sizeof(SiftAggCond));
	foreach_ptr(Node, q, conds)
	{
		if (!outer_cond(q, srti, &out->cond[nc++]))
			return false;
	}
	out->ncond = nc;
	for (int i = 0; i < nc; i++)
		if (out->cond[i].col < 0 || out->cond[i].col >= list_length(arms[0].cols))
			return false;
	if (fid != F_COUNT_)
	{
		if (list_length(oa->args) != 1)
			return false;
		arg = strip_relabel((Node *) linitial_node(TargetEntry, oa->args)->expr);
		if (!IsA(arg, Var) || ((Var *) arg)->varno != srti || ((Var *) arg)->varlevelsup != 0)
			return false;
		v = (Var *) arg;
		out->col = v->varattno - 1;
		if (out->col < 0 || out->col >= list_length(arms[0].cols))
			return false;
	}
	if (fid == F_COUNT_ && oa->aggdistinct == NIL)
		out->kind = SAA_O_COUNT_STAR;
	else if (fid == F_COUNT_ANY && oa->aggdistinct != NIL)
	{
		out->kind = SAA_O_COUNT_ROWS;
		for (int k = 0; k < narms; k++)
			if (!equal(list_nth(arms[k].cols, out->col), arms[k].id))
				return false;
	}
	else if (oa->aggdistinct == NIL && (fid == F_AVG_FLOAT8 || fid == F_SUM_FLOAT8) && v && v->vartype == FLOAT8OID)
		out->kind = fid == F_AVG_FLOAT8 ? SAA_O_AVG_F64 : SAA_O_SUM_F64;
	else if (oa->aggdistinct == NIL && fid == F_SUM_INT8 && v && v->vartype == INT8OID)
		out->kind = SAA_O_SUM_I64;
	else
		return false;
	for (int k = 0; k < narms; k++)
	{
		if (out->kind == SAA_O_AVG_F64 || out->kind == SAA_O_SUM_F64 || out->kind == SAA_O_SUM_I64)
			if (arm_inner(c, &arms[k], out->col, v->vartype) < 0)
				return false;
		for (int i = 0; i < out->ncond; i++)
			if (arm_inner(c, &arms[k], out->cond[i].col, out->cond[i].isint ? INT8OID : FLOAT8OID) < 0)
				return false;
	}
	out->desc = psprintf("%s", out->kind == SAA_O_COUNT_STAR ? "count(*)" : out->kind == SAA_O_COUNT_ROWS ? "count(DISTINCT row)" :
						 out->kind == SAA_O_AVG_F64 ? "avg(inner)" : out->kind == SAA_O_SUM_F64 ? "sum(inner)" : "sum(inner int8)");
	return true;
}

/*
 * A cell is an inner group, the share of one row in one group of an arm. When an arm groups by more than the outer keys and its id, one
 * row has several cells in an outer group, so the group expressions that are no outer key are keys of the cells: one key each, shared by
 * the arms that have an equal expression, after the outer keys and before the number of the set. They name no group (a group has id 0
 * for them, and the scan emits NULL).
 */
static bool
hidden_keys(AggCtx *c, SiftAggSpec *s, ArmIn *arms, int narms, List **scan_tlist, bool sets)
{
	int			nouter = s->nkeys;
	List	   *defs = NIL;

	for (int k = 0; k < narms; k++)
		for (int i = 0; i < list_length(arms[k].group); i++)
		{
			Node	   *g = list_nth(arms[k].group, i);
			SortGroupClause *sgc = list_nth_node(SortGroupClause, arms[k].gsgc, i);
			int			pos;
			SiftAggKey *key;

			if (equal(g, arms[k].id) || list_member(arms[k].used, g))
				continue;
			pos = -1;
			foreach_ptr(Node, d, defs)
				if (equal(d, g))
					pos = foreach_current_index(d);
			if (pos < 0)
			{
				if (s->nkeys + sets >= SIFT_AGG_MAX_KEYS)
					return false;
				key = &s->key[s->nkeys];
				if (!compile_key(c, sgc, g, key))
					return false;
				combo_set_key(c, key, s->nkeys);
				*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeNullConst(key->typid, key->typmod, key->collid),
																   list_length(*scan_tlist) + 1, NULL, false));
				defs = lappend(defs, g);
				pos = s->nkeys++ - nouter;
			}
			arms[k].out->hmask |= (uint32) 1 << (nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
	return true;
}

/*
 * The outer query aggregates the rows of its FROM item, the arms: a UNION ALL of aggregates (or one) of the same scan, each grouped by
 * a unique row id and by outer keys (a NULL constant where an arm has none of them): see SiftAggArm. The arms are read from their
 * planned roots, so that group expressions and constants are in the planner's final form; an arm that reads a common table
 * expression is looked through to it.
 */
void
sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	Index		srti = ((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex;
	List	   *roots;
	ArmIn		arms[AGG_MAX_ARMS] = {0};
	int			narms = 0;
	PlannerInfo *base = NULL;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;
	int			okey[SIFT_AGG_MAX_KEYS];
	int			ncols = 0;
	List	   *gates = NIL;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	if (list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS - 1)
		return;
	roots = outer_arms(root, srti, &gates);
	if (roots == NIL || list_length(roots) > AGG_MAX_ARMS)
		return;
	foreach_ptr(PlannerInfo, ar, roots)
	{
		PlannerInfo *b = arm_base(ar, &arms[narms]);

		if (b == NULL || (base != NULL && b != base && !(b == ar && same_scan(base, b))))
			return;
		if (base == NULL)
			base = b;
		arms[narms].sub.tlist = b->parse->targetList;
		narms++;
	}
	if (!agg_base(c, base, NULL, &fs) || c->nparts > 0)
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs) || !arm_gates(c, s, gates))
		return;

	/* the arms in the terms of the scanned table */
	for (int k = 0; k < narms; k++)
	{
		Query	   *aq = ((PlannerInfo *) list_nth(roots, k))->parse;
		ArmIn	   *in = &arms[k];
		int			nid = 0,
					nagg;
		List	   *rest = NIL;

		foreach_node(TargetEntry, te, aq->targetList)
			if (!te->resjunk)
				in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
		if (k == 0)
			ncols = list_length(in->cols);
		else if (ncols != list_length(in->cols))
			return;
		foreach_node(SortGroupClause, sgc, aq->groupClause)
		{
			Node	   *g = normalize_expr(c, cte_sub_mutator((Node *) get_sortgroupclause_expr(sgc, aq->targetList), &in->sub));

			in->group = lappend(in->group, g);
			in->gsgc = lappend(in->gsgc, sgc);
			if (unique_row_column(c, g))
			{
				in->id = g;
				nid++;
			}
		}
		if (nid != 1)
			return;
		if (in->sub.rti != 0 && aq->jointree->quals != NULL)
			foreach_ptr(Node, q, (List *) aq->jointree->quals)
				in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
		in->out = palloc0(sizeof(SiftAggArm));
		in->out->colsrc = palloc(Max(ncols, 1) * sizeof(int));
		for (int i = 0; i < ncols; i++)
			in->out->colsrc[i] = -1;
		nagg = ncols;
		count_aggs_walker(aq->havingQual, &nagg);
		in->out->inner = palloc0((nagg + 1) * sizeof(SiftAggAgg));
		in->out->having = palloc0(Max(list_length((List *) aq->havingQual), 1) * sizeof(SiftAggCond));
		foreach_ptr(Node, q, (List *) aq->havingQual)
		{
			Node	   *hq = normalize_expr(c, cte_sub_mutator(q, &in->sub));

			if (arm_having(c, in, hq, &in->out->having[in->out->nhaving]))
				in->out->nhaving++;
			else
				rest = lappend(rest, hq);
		}
		if (rest != NIL && !arm_having_expr(c, in, rest))
			return;
		in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
		foreach_ptr(Node, q, in->quals)
		{
			SiftAggPred *p = compile_combo_pred(c, q);

			if (p == NULL || unsupported_clause(c, q))
				return;
			in->out->filter[in->out->nfilter++] = p;
		}
	}

	/* the outer keys: columns that every arm groups by or has as NULL */
	for (int k = 0; k < narms; k++)
		nkeyslots += list_length(arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		Node	   *def = NULL;

		if (!IsA(e, Var) || ((Var *) e)->varno != srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > ncols)
			return;
		okey[s->nkeys] = ((Var *) e)->varattno - 1;
		for (int k = 0; k < narms; k++)
		{
			Node	   *x = list_nth(arms[k].cols, okey[s->nkeys]);

			if (null_const(x))
				continue;
			if (!list_member(arms[k].group, x) || equal(x, arms[k].id) || (def != NULL && !equal(x, def)))
				return;
			def = x;
			arms[k].used = lappend(arms[k].used, x);
			arms[k].out->keymask |= (uint32) 1 << s->nkeys;
		}
		if (def == NULL || !compile_key(c, sgc, def, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, arms, narms, &scan_tlist, parse->groupingSets != NIL))
		return;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* the outer aggregates */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(c, s, (Aggref *) a, srti, arms, narms, &s->agg[s->naggs])))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	s->narms = narms;
	s->arm = palloc0(narms * sizeof(SiftAggArm));
	for (int k = 0; k < narms; k++)
	{
		s->arm[k] = *arms[k].out;
		s->arm[k].ncol = ncols;
	}
	agg_finish(c, s, root, input_rel, output_rel, output_rel->reltarget, ow.keys, scan_tlist);
}

/* ---------- plan ---------- */

/* the native plans of the correlated children, made now that the plan has its place in the statement; true when the spec holds their IDs */
static bool
plan_correlated_children(PlannerInfo *root, CustomPath *best, SiftAggSpec *spec)
{
	bool		planned = false;

	for (int i = 0; i < spec->njoin; i++)
		if (spec->join[i].params != NIL)
		{
			Path	   *subpath = list_nth(((SiftAggPath *) best)->correlated, i);
			PlannerInfo *subroot = list_nth(((SiftAggPath *) best)->subroots, i);
			Plan       *plan = create_plan(subroot, subpath);

			root->glob->subplans = lappend(root->glob->subplans, plan);
			root->glob->subpaths = lappend(root->glob->subpaths, subpath);
			root->glob->subroots = lappend(root->glob->subroots, subroot);
			spec->join[i].subplan = list_length(root->glob->subplans);
			planned = true;
		}
	return planned;
}

static Plan *
agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *pack = linitial(best->custom_private);
	List	   *scan_tlist = lsecond(best->custom_private);
	SiftAggSpec *spec = sift_aggspec_unpack(pack);
	GroupPathExtraData *gextra = NULL;

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(scan_tlist);
	cs->custom_private = plan_correlated_children(root, best, spec) ? sift_aggspec_pack(spec) : pack;
	cs->methods = &sift_agg_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	(void) gextra;
	if (OidIsValid(spec->indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->indexoid);
	for (int i = 0; i < spec->nparts; i++)
		root->glob->relationOids = lappend_oid(lappend_oid(root->glob->relationOids, spec->partheap[i]), spec->partindex[i]);
	for (int i = 0; i < spec->njoin; i++)
		if (OidIsValid(spec->join[i].relid))
			root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->join[i].relid);
	return &cs->scan.plan;
}

const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
