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
#include "agg/rows.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/*
 * The leaf of the index that a leaf after the source's takes its values from: the value column of a member of an object, the array whose length it
 * is; -1 for the type of a path (the tags of its leaf say it, whatever the leaf holds). Whatever that leaf cannot store is read as NULL.
 */
int
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
 * a missing key are the same NULL there; before PostgreSQL 18 the cast of a JSON null raises where the missing key does not, so the plan stays
 * normal while some unit may hold one (reads_json_null_cast).
 */
static const char *
left_each_key(LeftJoin *lj, Index rti, FuncKind kind)
{
	OpExpr	   *op;
	Node	   *a,
			   *b;

	if (kind != FK_EACH || lj->kind != SJ_LEFT || list_length(lj->quals) != 1 || !IsA(linitial(lj->quals), OpExpr))
		return NULL;
	op = linitial_node(OpExpr, lj->quals);
	set_opfuncid(op);
	/* a non-deterministic collation equates keys that differ in bytes (k and K): the join then matches several members */
	if (op->opfuncid != F_TEXTEQ || !get_collation_isdeterministic(op->inputcollid))
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
		return agg_refuse(FROM_SHAPE);
	/*
	 * A semi, anti or preserved join whose table has no relation entry was not removed as a useless LEFT JOIN: PostgreSQL 18 merged it into another
	 * scan of the same table by a unique key (self-join elimination), and moved its conditions to the one it kept, which the FROM tree does not say.
	 */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->kind != SJ_LEFT && root->simple_rel_array[lj->rti] == NULL)
			return agg_refuse(FROM_SHAPE);
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
				return agg_refuse(TABLE_KIND);
			if (c->relid == 0)
				c->relid = rti;
			else if (part || c->joinless)
				return agg_refuse(FROM_SHAPE);
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
					return agg_refuse(OUTER_JOIN);
		}
		else if (input_rel == NULL || c->joinless ||
				 (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION && e->rtekind != RTE_CTE))
			return agg_refuse(FROM_SHAPE);
		else
			joined = lappend_int(joined, rti);
	}
	if (c->relid == 0)
		return agg_refuse(FROM_SHAPE);
	/* a plan joined to the scan by an outer join keeps its own rows: the scanned table is never the nullable side */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == c->relid)
			return agg_refuse(OUTER_JOIN);
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	/* the joins come after the source: planning the sub-selects of a query with no column index to scan would be a waste */
	if (c->rel == NULL || !find_source(c, &ii))
		return agg_refuse(NO_SOURCE);
	c->nsiblings = list_length(joined) + (c->func_rti != 0);
	foreach_int(rti, joined)
		if (!add_join(c, fs, rti))
			return agg_refuse(JOIN_CHILD);
	if (input_rel != NULL)
	{
		if (!bms_is_member(c->relid, input_rel->relids))
			return agg_refuse(FROM_SHAPE);
		for (int m = -1; (m = bms_next_member(input_rel->relids, m)) >= 0;)
			if (rt_fetch(m, parse->rtable)->rtekind != RTE_JOIN)
			{
				if (!bms_is_member(m, allrtis))
					return agg_refuse(FROM_SHAPE);
				nin++;
			}
		if (nin + list_length(c->joins) < list_length(fs->rtis))
			return agg_refuse(FROM_SHAPE);
	}
	/* the rows of a preserved side that nothing matched are known at the end of a single scan of one join */
	if (fs->ojrels && (input_rel == NULL || c->nparts > 0))
		return agg_refuse(OUTER_JOIN);
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind) && jc != llast(c->joins))
			return agg_refuse(OUTER_JOIN);
	build_attexprs(c);
	if (c->func_rti)
	{
		int			leaf = group_leaf_of(c, linitial(((FuncExpr *) c->func->funcexpr)->args), func_leaf_kind(c->func_kind));

		if (leaf < 0)
			return agg_refuse(UNNEST_LEAF);
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
		return agg_refuse_on(FROM_SHAPE, "no other table of the FROM list can be the scanned one");
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
	s->partprune = c->partprune;
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

/* a conjunct over the joined rows: a boolean slot of a combo, or an EXISTS over the elements of an array */
static SiftAggPred *
post_pred(AggCtx *c, Node *q)
{
	return IsA(q, SubPlan) ? compile_exists_post(c, (SubPlan *) q) : compile_combo_pred(c, normalize_expr(c, q));
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
		return agg_refuse(JOIN_CONDITION);
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
			return agg_refuse(WHERE);
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	foreach_ptr(Node, q, scan_post)
	{
		SiftAggPred *p = post_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return agg_refuse(WHERE);
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->nprepost = preserved_join(c) ? s->npost : 0;
	foreach_ptr(Node, q, joined)
	{
		SiftAggPred *p = post_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return agg_refuse(WHERE);
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
		{
			record_deps(root, s->join[i].keyexpr[k]);
			record_deps(root, s->join[i].tkey[k]);
		}
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

/*
 * rows: what the groups are made of. By PostgreSQL's estimate of the groups (distinct false) they are no more than the rows its statistics
 * of the table expect after the filters; by the distinct values of the key columns they are no more than the rows given, whatever the
 * filters keep (a column's values under the filters on it alone, column_distinct).
 */
static double
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

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the whole
 * query is safe to run in a worker (the workers evaluate the spec's expressions) and the spec's tables can be merged
 * (sift_aggspec_parallel_ok).
 */
int
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
	/* the leader writes the lookup tables to the shared memory and each worker maps them: a scan of fewer rows than AGG_SHARE_RATIO for each of their rows does not pay for it */
	if (agg_share_rows(c) * AGG_SHARE_RATIO > Max(c->stats.nrows, c->rel->tuples))
		return 0;
	return compute_parallel_worker(c->rel, rel_pages(c), -1, max_parallel_workers_per_gather);
}

/* is leaf m the value column of an object whose members the spec reads only by key, all of them read again from the heap (member_recheck_ok)? */
static bool
member_source(AggCtx *c, const SiftAggSpec *s, int m)
{
	bool		found = false;

	for (int j = 0; j < s->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) == m)
		{
			if (s->vleaf[j].recheck == SRK_NONE)
				return false;
			found = true;
		}
	return found;
}

/*
 * Does the spec read a leaf that holds values the index cannot store? The scan raises an error when a row of its snapshot has one
 * (check_overflow), the normal plan reads them from the heap: the query keeps its plan. Unless the source reads the rows again as SQL reads
 * them (spec_recheck). A numeric leaf is not one: its only such value is NaN, which the scan evaluates (SiftSrcVec.ovf). Nor is the leaf of a
 * jsonb_typeof conjunct (SAP_TYPE): the source keeps the rows of the types and tells the type of each value it cannot store. Unless the query
 * reads the leaf too and the conjunct keeps a type the leaf cannot store (typeof_keeps_unstorable): such a row would be an error.
 */
static bool
reads_overflow(AggCtx *c, const SiftAggSpec *s)
{
	Bitmapset  *typed = NULL,
			   *leaves;
	int			m = -1;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE && !typeof_keeps_unstorable(c, s->pred[i]))
			typed = bms_add_member(typed, s->pred[i]->leaf);
	leaves = bms_union(c->used, c->func_ovf);
	for (int j = 0; j < c->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) >= 0)
			leaves = bms_add_member(leaves, vleaf_source(c, j));
	while ((m = bms_next_member(leaves, m)) >= 0)
		if (c->stats.ovf[m] && !bms_is_member(m, typed) && spec_recheck(c, s, m) == SRK_NONE && !member_source(c, s, m))
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not read \"%s\": some unit holds values the leaf cannot store", leaf_name(c, m)),
							 errhint("docs/COL-DIRTY-UNITS.md")));
			agg_refused_on(DIRTY_UNITS, leaf_name(c, m));
			return true;
		}
	return false;
}

/*
 * Does the spec cast the value of a leaf that may hold a JSON null with the cast function of jsonb? Before PostgreSQL 18 the cast raises for it
 * (NULL since), as the scan, which reads the NULL slot, would not: the normal plan keeps the query. A leaf of a unit without JSON nulls keeps the scan.
 */
static bool
reads_json_null_cast(AggCtx *c)
{
	Bitmapset  *casts = bms_intersect(c->jcast, c->read);
	int			m = -1,
				first = c->lay->nleaves - c->nvleaf;

	while ((m = bms_next_member(casts, m)) >= 0)
	{
		int			src = m < first ? m : vleaf_source(c, m - first);

		if (src >= 0 && c->stats.jnull[src])
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not cast \"%s\": some unit may hold a JSON null there, which the cast raises for before PostgreSQL 18",
									leaf_name(c, src)),
							 errhint("docs/COL.md 2.2")));
			agg_refused_on(ERROR_PARITY, leaf_name(c, src));
			return true;
		}
	}
	return false;
}

/* the leaves the spec reads and the combos; false when the spec cannot be run. keys: the group expressions of root's query */
bool
spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys)
{
	double		lists;

	if (c->other_group)
		return agg_refuse_on(FROM_SHAPE, "the query reads the elements of a second list besides the one it unnests");
	if (types_guard_reads(c))
		return agg_refuse_on(ERROR_PARITY, "a jsonb_typeof guard (FILTER or CASE) wraps a read of the same path: the leaf cannot hold the values the guard skips");
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return agg_refuse_f(FROM_SHAPE, "%s is an element of a list the query does not unnest", leaf_name(c, i));
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return agg_refuse_f(FROM_SHAPE, "the unnested list %s is read by no leaf", leaf_name(c, c->group));
	if (list_length(c->combos) > sift_limit(combos, SIFT_AGG_MAX_COMBOS))
		return agg_refuse_limit(SHAPE_WIDTH, combos, sift_limit(combos, SIFT_AGG_MAX_COMBOS), "%d value combinations", list_length(c->combos));
	if (list_length(c->joins) > sift_limit(joins, SIFT_AGG_MAX_JOINS))
		return agg_refuse_limit(SHAPE_WIDTH, joins, sift_limit(joins, SIFT_AGG_MAX_JOINS), "%d lookup-joined tables", list_length(c->joins));
	typeof_reads(c, s);
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	/* the columns a recordset declares and the query does not read are leaves of the spec too: one the source cannot read again keeps the plan */
	finish_leaves(c, s);
	if (reads_overflow(c, s) || reads_json_null_cast(c))
		return agg_refuse(DIRTY_UNITS);
	finish_combos(c, s);
	s->est_groups = set_groups(c, root, s, keys, input_rel->rows, false);
	/*
	 * Where PostgreSQL's count of a lookup is a default guess (the lists are the output of a plan), the path reports the groups of all the rows
	 * the lists can pass: a plan above that takes a CTE of one row for one nests loops on a null-safe key and runs the aggregate under it
	 * again for each row, while a count that is too large only makes it hash or materialize. A filter of the table that has no statistics
	 * keeps a guess of the rows as well (filter_rows_ratio).
	 */
	lists = lookup_set_ratio(c, s, input_rel, true) * filter_rows_ratio(c);
	c->path_rows = lists > 1.0 ? set_groups(c, root, s, keys, input_rel->rows * lists, true) : 0;
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
int			sift_agg_lookup_max_rows = -1;
int			sift_agg_lookup_max_kb = 256 * 1024;

/* the bytes of a row of a lookup table besides its columns: the hash entry and the row's header (about 48 in all for an integer key and a text column, measured) */
#define AGG_LOOKUP_ROW_OVERHEAD	64

/*
 * Do `rows` rows of `width` bytes fit the lookup tables the scan loads when it starts? A rows limit that was set (the older form of
 * sift.agg_lookup_max_kb) is compared with the rows alone.
 */
bool
agg_lookup_fits(double rows, int width)
{
	if (sift_agg_lookup_max_rows >= 0)
		return rows <= sift_agg_lookup_max_rows;
	return rows * (width + AGG_LOOKUP_ROW_OVERHEAD) <= (double) sift_agg_lookup_max_kb * 1024;
}

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

/* the scan of the rows of the table a plan reads them with, when it is an index or bitmap scan under aggregation, sorts and gathers, or the probes a nested loop makes of one per key */
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
			case T_NestPath:
				{
					/* the keys of a lookup drive the probes of an index or bitmap scan of the table: the plan costs a probe a key */
					Path	   *inner = ((NestPath *) p)->jpath.innerjoinpath;

					if (IsA(inner, MemoizePath))
						inner = ((MemoizePath *) inner)->subpath;
					return (IsA(inner, IndexPath) || IsA(inner, BitmapHeapPath)) && inner->param_info != NULL ? inner : NULL;
				}
			default:
				return NULL;
		}
}

/* the paths that fetch the rows one set element or key at a time are charged the elements of the array parameters and the keys of the lookups that sift.agg_param_set_size and lookup_set_ratio assume */
static void
param_set_penalty(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	double		f = param_set_ratio(c) * lookup_set_ratio(c, s, input_rel, false);

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

	if (!spec_finish(c, s, root, input_rel, keys))
		return;
	if (!first_fits(s))
	{
		agg_refused(FIRST_MEMORY);
		return;
	}
	if (!child_paths(c, s))
		return;
	param_set_penalty(c, s, input_rel, output_rel);
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

/* Does the grouping have a key that is not a constant? */
static bool
groupClause_has_nonconst(PlannerInfo *root, List *groupClause)
{
	foreach_node(SortGroupClause, sgc, groupClause)
		if (!IsA(get_sortgroupclause_expr(sgc, root->processed_tlist), Const))
			return true;
	return false;
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
	if (list_length(groupClause) + (parse->groupingSets != NIL) > sift_limit(group_keys, SIFT_AGG_MAX_KEYS))
	{
		agg_refused_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS), "%d group keys (grouping sets count as one more)",
						  list_length(groupClause) + (parse->groupingSets != NIL));
		return;
	}
	s->key = palloc0((list_length(groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		/* a constant adds no group: the output keeps it as the constant it is (not with grouping sets, whose empty set has no key, and not alone: an empty input has no group) */
		if (IsA(e, Const) && parse->groupingSets == NIL && groupClause_has_nonconst(root, groupClause))
			continue;
		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
		{
			agg_refused_f(GROUP_KEY, "the group key %s is not a leaf, or an expression of leaves, the scan reads", expr_text(c, e));
			return;
		}
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
		{
			agg_refused(DISTINCT_ON);
			return;
		}
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
	{
		agg_refused(OUTPUT_EXPR);
		return;
	}
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		bool		first = list_member_ptr(firsts, a);

		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
		{
			agg_refused_f(AGGREGATE, "%s is not an aggregate the scan computes over its arguments, filter and ordering", IsA(a, Aggref) ? get_func_name(((Aggref *) a)->aggfnoid) : "GROUPING()");
			return;
		}
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

int			agg_index_pick;
int			agg_index_usable;

static void
upper_paths_on(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	Query	   *parse = root->parse;

	if (stage == UPPERREL_ORDERED)
	{
		agg_arm_begin("topn", false);
		topn_paths(root, input_rel, output_rel);
		agg_arm_end();
		return;
	}
	if (!sift_enable_agg || parse->commandType != CMD_SELECT || parse->hasTargetSRFs || root->rowMarks != NIL || parse->hasRecursive)
	{
		if (stage == UPPERREL_GROUP_AGG && parse->commandType == CMD_SELECT)
		{
			agg_arm_begin("single", true);
			agg_refuse_note(sift_enable_agg ? AGG_REFUSE_QUERY_KIND : AGG_REFUSE_DISABLED, NULL);
			agg_arm_end();
		}
		return;
	}
	/* SELECT DISTINCT of a plain scan is a group-by with no aggregate; over a grouped or windowed query it is not the scan's */
	if (stage == UPPERREL_DISTINCT)
	{
		if (parse->distinctClause != NIL && root->upper_targets[UPPERREL_DISTINCT] != NULL &&
			!(parse->hasAggs || parse->groupClause || parse->groupingSets || parse->hasWindowFuncs || parse->havingQual))
		{
			agg_arm_begin("distinct", true);
			group_paths(root, input_rel, output_rel, root->upper_targets[UPPERREL_DISTINCT], NULL, parse->distinctClause);
			agg_arm_end();
		}
		return;
	}
	if (stage != UPPERREL_GROUP_AGG || parse->hasDistinctOn || !(parse->hasAggs || parse->groupClause || parse->groupingSets))
		return;
	/* a window function is planned above the grouping stage: this stage is the same with or without one */
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		if (!parse->hasWindowFuncs && sift_agg_two_level_on)
		{
			agg_arm_begin("two_level", false);
			sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
			agg_arm_end();
		}
		return;
	}
	/* a join of a spine and the arms of a two level aggregate; any other join is the single level's */
	if (!parse->hasWindowFuncs && sift_agg_two_level_on && list_length(parse->jointree->fromlist) == 1 &&
		(IsA(linitial(parse->jointree->fromlist), JoinExpr) || IsA(linitial(parse->jointree->fromlist), FromExpr)))
	{
		agg_arm_begin("two_level", false);
		sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
		agg_arm_end();
	}
	agg_arm_begin("single", true);
	group_paths(root, input_rel, output_rel, output_rel->reltarget, (GroupPathExtraData *) extra, parse->groupClause);
	agg_arm_end();
	agg_arm_begin("rows", false);
	sift_agg_rows_paths(root, input_rel, output_rel, extra);
	agg_arm_end();
}

static bool
has_agg_path(RelOptInfo *rel)
{
	foreach_ptr(Path, p, rel->pathlist)
		if (IsA(p, CustomPath) && ((CustomPath *) p)->methods == &agg_path_methods)
			return true;
	return false;
}

/* a table with several column indexes is scanned through the first of them that gives a plan: each may hold other columns and paths */
void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	int			outer_pick = agg_index_pick,
				outer_usable = agg_index_usable;
	AggStage	st;

	agg_stage_begin(&st, root->query_level);
	agg_index_usable = 0;
	for (agg_index_pick = 0;; agg_index_pick++)
	{
		upper_paths_on(root, stage, input_rel, output_rel, extra);
		if (has_agg_path(output_rel) || agg_index_pick + 1 >= agg_index_usable)
			break;
	}
	agg_stage_end(&st);
	agg_index_pick = outer_pick;
	agg_index_usable = outer_usable;
}

/* the native plans of the correlated children, made now that the plan has its place in the statement; true when the spec holds their IDs */
static bool
plan_correlated_children(PlannerInfo *root, CustomPath *best, SiftAggSpec *spec)
{
	bool		planned = false;

	for (int i = 0; i < spec->njoin; i++)
	{
		Path	   *subpath = list_nth(((SiftAggPath *) best)->correlated, i);

		if (subpath != NULL)
		{
			PlannerInfo *subroot = list_nth(((SiftAggPath *) best)->subroots, i);
			List	   *tlist = subroot->processed_tlist;
			Plan       *plan;

			/* the plan of the rows of a plain child is labelled with its columns, not with those of the level's query */
			if (spec->join[i].params == NIL)
				subroot->processed_tlist = make_tlist_from_pathtarget(subpath->pathtarget);
			plan = create_plan(subroot, subpath);
			subroot->processed_tlist = tlist;
			root->glob->subplans = lappend(root->glob->subplans, plan);
			root->glob->subpaths = lappend(root->glob->subpaths, subpath);
			root->glob->subroots = lappend(root->glob->subroots, subroot);
			spec->join[i].subplan = list_length(root->glob->subplans);
			planned = true;
		}
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
