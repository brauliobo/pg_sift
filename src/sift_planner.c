/*
 * sift_planner.c - planner side of the index: amcostestimate, the matching of
 * expressions to index keys, and the hooks that add SiftScan (heap-order or
 * ranked) and SiftCount paths. Execution is in sift_cscan.c.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "sift_am.h"
#include "sift_eval.h"
#include "sift_planner.h"
#include "sift_pushdown.h"

static set_rel_pathlist_hook_type prev_pathlist_hook;
static create_upper_paths_hook_type prev_upper_hook;

/* Dictionary pages a term lookup reads: root array hit, DICTIDX, DICT block */
#define DICT_PAGES_PER_TERM		2.0
/* A posting costs about a byte of codec blob, tf sidecar included */
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

bool
sift_match_clause(Node *clause, IndexOptInfo *index, Expr **query)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *args;
	Node	   *rhs;

	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (IsA(clause, OpExpr) && ((OpExpr *) clause)->opno == cat->match_op)
		args = ((OpExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_func)
		args = ((FuncExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_idx_func)
	{
		args = ((FuncExpr *) clause)->args;
		if (!IsA(lthird(args), Const) || DatumGetObjectId(((Const *) lthird(args))->constvalue) != index->indexoid)
			return false;
	}
	else
		return false;
	rhs = lsecond(args);
	if (!sift_index_matches_key(index, linitial(args)) || contain_var_clause(rhs) || contain_volatile_functions(rhs))
		return false;
	*query = (Expr *) rhs;
	return true;
}

bool
sift_is_score_expr(Node *expr, Index relid, bool *is_max)
{
	const SiftCatalog *cat = sift_catalog();
	FuncExpr   *f;
	Node	   *arg;

	if (!IsA(expr, FuncExpr))
		return false;
	f = (FuncExpr *) expr;
	if (f->funcid != cat->score_func && f->funcid != cat->max_score_func)
		return false;
	arg = linitial(f->args);
	while (IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (!IsA(arg, Var) || ((Var *) arg)->varno != relid || ((Var *) arg)->varlevelsup != 0 ||
		((Var *) arg)->varattno != SelfItemPointerAttributeNumber)
		return false;
	if (is_max)
		*is_max = f->funcid == cat->max_score_func;
	return true;
}

/* Query texts of the constant queries; false when one is not a constant */
static bool
const_queries(List *queries, text ***texts, int *ntexts)
{
	*texts = palloc(list_length(queries) * sizeof(text *));
	*ntexts = 0;
	foreach_node(Node, arg, queries)
	{
		if (!IsA(arg, Const) || ((Const *) arg)->constisnull)
			return false;
		(*texts)[(*ntexts)++] = DatumGetTextPP(((Const *) arg)->constvalue);
	}
	return true;
}

/* The query operands of the index quals; the clauses may be the operator or its support-function form */
static List *
qual_query_exprs(IndexPath *path)
{
	List	   *queries = NIL;

	foreach_node(IndexClause, ic, path->indexclauses)
		foreach_node(RestrictInfo, rinfo, ic->indexquals)
			queries = lappend(queries, lsecond(castNode(OpExpr, rinfo->clause)->args));
	return queries;
}

/* What reading a query's postings costs; est.docs and est.ndocs are the index's view of the matches */
typedef struct IndexWork
{
	Cost		cost;
	double		pages;
	SiftEstimate est;
	bool		known;
} IndexWork;

static void
index_work(Oid indexoid, Oid tablespace, List *queries, IndexWork *w)
{
	text	  **texts;
	int			ntexts;
	double		random_cost,
				seq_cost,
				dict_pages;
	Relation	index;

	w->known = const_queries(queries, &texts, &ntexts);
	if (!w->known)
		return;
	index = index_open(indexoid, AccessShareLock);
	sift_eval_estimate(index, texts, ntexts, &w->est);
	index_close(index, AccessShareLock);
	get_tablespace_page_costs(tablespace, &random_cost, &seq_cost);
	dict_pages = w->est.terms * DICT_PAGES_PER_TERM;
	w->pages = dict_pages + ceil(w->est.postings * BYTES_PER_POSTING / BLCKSZ);
	w->cost = dict_pages * random_cost + (w->pages - dict_pages) * seq_cost +
		w->est.postings * cpu_operator_cost * POSTING_CPU_FRACTION;
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
	IndexWork	w;

	genericcostestimate(root, path, loop_count, &costs);
	index_work(path->indexinfo->indexoid, path->indexinfo->reltablespace, qual_query_exprs(path), &w);
	if (w.known)
	{
		costs.numIndexPages = w.pages;
		costs.indexTotalCost = w.cost;
		costs.indexStartupCost = w.cost;
	}
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = costs.numIndexPages;
}

/* ---------- paths ---------- */

typedef struct ScanCand
{
	IndexOptInfo *index;
	List	   *queries;			/* operands of the sift quals */
	List	   *residual;			/* RestrictInfos the scan evaluates itself */
} ScanCand;

/* Splits the rel's restrictions for one sift index; false when none is a sift qual of it */
static bool
split_quals(RelOptInfo *rel, IndexOptInfo *ii, ScanCand *c)
{
	c->index = ii;
	c->queries = c->residual = NIL;
	if (ii->relam != sift_catalog()->am || ii->ncolumns != 1 || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
		return false;
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		Expr	   *q;

		if (sift_match_clause((Node *) rinfo, ii, &q))
			c->queries = lappend(c->queries, q);
		else
			c->residual = lappend(c->residual, rinfo);
	}
	return c->queries != NIL;
}

typedef struct ScoreRefs
{
	Index		relid;
	List	   *found;
} ScoreRefs;

static bool
score_walker(Node *node, ScoreRefs *refs)
{
	if (node == NULL)
		return false;
	if (sift_is_score_expr(node, refs->relid, NULL))
	{
		refs->found = list_append_unique(refs->found, node);
		return false;
	}
	return expression_tree_walker(node, score_walker, refs);
}

/* sift.score / max_score of rel in the final target list and in the query's first sort key */
static List *
score_exprs(PlannerInfo *root, RelOptInfo *rel)
{
	ScoreRefs	refs = {rel->relid, NIL};

	score_walker((Node *) root->processed_tlist, &refs);
	if (root->query_pathkeys != NIL)
		foreach_node(EquivalenceMember, em, linitial_node(PathKey, root->query_pathkeys)->pk_eclass->ec_members)
			score_walker((Node *) em->em_expr, &refs);
	return refs.found;
}

/* Is the first sort key sift.score(rel.ctid) DESC? */
static bool
sorted_by_score(PlannerInfo *root, RelOptInfo *rel, PathKey **pk)
{
	bool		is_max;

	if (root->query_pathkeys == NIL)
		return false;
	*pk = linitial_node(PathKey, root->query_pathkeys);
	if ((*pk)->pk_cmptype != COMPARE_GT)
		return false;
	foreach_node(EquivalenceMember, em, (*pk)->pk_eclass->ec_members)
		if (sift_is_score_expr((Node *) em->em_expr, rel->relid, &is_max) && !is_max)
			return true;
	return false;
}

/* Matching documents, the index work, and what verifying a candidate costs per heap tuple */
typedef struct ScanCost
{
	IndexWork	idx;
	double		docs;			/* tuples the scan fetches from the heap */
	Cost		tuple;			/* heap access and residual quals per fetched tuple */
	Cost		page;			/* per page of a fetch in heap order */
} ScanCost;

static void
scan_cost(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, ScanCost *sc)
{
	QualCost	qual;
	double		random_cost,
				seq_cost,
				pages = Max(rel->pages, 1),
				fetched;

	index_work(c->index->indexoid, c->index->reltablespace, c->queries, &sc->idx);
	sc->docs = sc->idx.known ? sc->idx.est.docs : rel->rows;
	sc->docs = Min(Max(sc->docs, rel->rows), Max(rel->tuples, 1));
	cost_qual_eval(&qual, c->residual, root);
	sc->tuple = cpu_tuple_cost + qual.per_tuple;
	get_tablespace_page_costs(rel->reltablespace, &random_cost, &seq_cost);
	fetched = Min(index_pages_fetched(sc->docs, rel->pages, c->index->pages, root), pages);
	sc->page = random_cost - (random_cost - seq_cost) * sqrt(fetched / pages);
	sc->docs = Max(sc->docs, 1);
	sc->tuple += fetched / sc->docs * sc->page;
}

static CustomPath *
make_path(Oid heapoid, RelOptInfo *parent, ScanCand *c, SiftScanMode mode, int64 k, uint32 flags,
		  const CustomPathMethods *methods)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = parent;
	cp->path.pathtarget = parent->reltarget;
	cp->flags = flags;
	cp->methods = methods;
	cp->custom_private = list_make5(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(c->index->indexoid), false, true),
									makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(mode), false, true),
									makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(k), false, FLOAT8PASSBYVAL),
									makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(heapoid), false, true),
									c->queries);
	return cp;
}

/*
 * Candidates a ranked scan verifies before it holds k that pass the quals, when the fraction pass of
 * the cands pass: the k-th best passing score is about k / pass candidates down, and the bound order
 * overshoots it by a logarithmic factor. Without a limit it verifies every one.
 */
static double
rank_verified(double cands, double pass, int64 k)
{
	return k > 0 ? Min(cands, k / pass * (1 + log(Max(cands * pass / k, 1)))) : cands;
}

/*
 * With a filter the ranker only meets the matches its bitmap keeps (the fraction fe.sel of them, at least
 * those that pass the quals), at the price of building the bitmap first.
 */
static CustomPath *
scan_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, SiftScanMode mode, int64 k, List *pathkeys,
		  BitmapHeapPath *filter)
{
	CustomPath *cp = make_path(root->simple_rte_array[rel->relid]->relid, rel, c, mode, k,
							   CUSTOMPATH_SUPPORT_PROJECTION, &sift_scan_path_methods);
	ScanCost	sc;
	SiftFilterEst fe = {0, 1};
	Cost		startup,
				verify;
	double		pass;

	scan_cost(root, rel, c, &sc);
	cp->path.pathkeys = pathkeys;
	cp->path.rows = rel->rows;
	startup = sc.idx.known ? sc.idx.cost : 0;
	if (mode == SIFT_MODE_RANKED)
	{
		pass = Min(rel->rows / sc.docs, 1.0);
		if (filter)
		{
			sift_filter_estimate(root, rel, filter, &fe);
			fe.sel = Max(fe.sel, pass);
			cp->custom_paths = list_make1(filter);
			startup += fe.cost;
		}
		verify = rank_verified(sc.docs * fe.sel, pass / fe.sel, k) * sc.tuple;
		cp->path.startup_cost = k > 0 ? startup + verify : startup;
		cp->path.total_cost = k > 0 ? cp->path.startup_cost + rel->rows * cpu_tuple_cost : startup + verify;
	}
	else
	{
		cp->path.startup_cost = startup;
		cp->path.total_cost = startup + sc.docs * sc.tuple;
	}
	return cp;
}

/*
 * A score the query reads can only come from a ranked scan, so that path
 * replaces the others; without one the unordered scan competes with them.
 */
static void
sift_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ScanCand	c;
	PathKey    *pk = NULL;
	List	   *scores;
	bool		ranked,
				filter_looked = false;
	BitmapHeapPath *filter = NULL;
	CustomPath *best = NULL;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (!sift_enable_scan || rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->inh ||
		rte->tablesample || root->parse->commandType != CMD_SELECT || root->rowMarks != NIL ||
		(rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		sift_catalog()->am == InvalidOid)
		return;
	scores = score_exprs(root, rel);
	ranked = sorted_by_score(root, rel, &pk);
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		ScoreRefs	refs = {rti, NIL};

		score_walker((Node *) rinfo->clause, &refs);
		if (refs.found != NIL)
			return;
	}
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		CustomPath *cps[2];
		int			ncps = 0;
		int64		k = root->limit_tuples > 0 ? (int64) ceil(root->limit_tuples) : 0;

		if (!split_quals(rel, ii, &c))
			continue;
		if (ranked || scores != NIL)
		{
			List	   *pathkeys = ranked ? list_make1(pk) : NIL;

			if (!filter_looked)
				filter = sift_filter_bitmap_path(root, rel);
			filter_looked = true;
			if (filter == NULL || sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
				cps[ncps++] = scan_path(root, rel, &c, SIFT_MODE_RANKED, ranked ? k : 0, pathkeys, NULL);
			if (filter)
				cps[ncps++] = scan_path(root, rel, &c, SIFT_MODE_RANKED, ranked ? k : 0, pathkeys, filter);
		}
		else
			cps[ncps++] = scan_path(root, rel, &c, SIFT_MODE_UNORDERED, 0, NIL, NULL);
		for (int i = 0; i < ncps; i++)
		{
			if (best == NULL || compare_path_costs(&cps[i]->path, &best->path, TOTAL_COST) < 0)
				best = cps[i];
			add_path(rel, &cps[i]->path);
		}
	}
	if (best && (ranked || scores != NIL))
	{
		rel->pathlist = list_make1(best);
		rel->partial_pathlist = NIL;
	}
}

/* count(*) and count(<constant>) without DISTINCT, FILTER or ORDER BY */
static bool
plain_count(Node *expr)
{
	Aggref	   *agg;

	if (!IsA(expr, Aggref))
		return false;
	agg = (Aggref *) expr;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggkind != AGGKIND_NORMAL ||
		agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;
	if (agg->aggfnoid == F_COUNT_)
		return true;
	if (agg->aggfnoid != F_COUNT_ANY || list_length(agg->args) != 1)
		return false;
	expr = (Node *) linitial_node(TargetEntry, agg->args)->expr;
	return IsA(expr, Const) && !((Const *) expr)->constisnull;
}

/* The index every restriction of the rel is a sift qual of */
static bool
count_cand(RelOptInfo *rel, ScanCand *c)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (split_quals(rel, ii, c) && c->residual == NIL)
			return true;
	return false;
}

static void
sift_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
				 void *extra)
{
	Query	   *parse = root->parse;
	ScanCand	c;
	ScanCost	sc;
	CustomPath *cp;
	RangeTblEntry *rte;

	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	if (stage != UPPERREL_GROUP_AGG || !sift_enable_count || sift_catalog()->am == InvalidOid ||
		!parse->hasAggs || parse->groupClause != NIL || parse->groupingSets != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->havingQual != NULL || parse->commandType != CMD_SELECT ||
		root->rowMarks != NIL || input_rel->reloptkind != RELOPT_BASEREL ||
		bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rte = root->simple_rte_array[input_rel->relid];
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample || rte->inh)
		return;
	foreach_node(Node, e, output_rel->reltarget->exprs)
		if (!plain_count(e))
			return;
	if (!count_cand(input_rel, &c))
		return;
	scan_cost(root, input_rel, &c, &sc);
	cp = make_path(rte->relid, output_rel, &c, SIFT_MODE_COUNT, 0, 0, &sift_count_path_methods);
	cp->path.rows = 1;
	/* only matches on pages that are not all-visible are checked in the heap, a page at a time */
	cp->path.startup_cost = (sc.idx.known ? sc.idx.cost : 0) +
		(1 - input_rel->allvisfrac) * (Min(sc.docs, Max(input_rel->pages, 1)) * sc.page + sc.docs * cpu_tuple_cost) +
		sc.docs * cpu_operator_cost * POSTING_CPU_FRACTION;
	cp->path.total_cost = cp->path.startup_cost + cpu_tuple_cost;
	add_path(output_rel, &cp->path);
}

/* ---------- plans ---------- */

static IndexOptInfo *
rel_index(RelOptInfo *rel, Oid indexoid)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->indexoid == indexoid)
			return ii;
	elog(ERROR, "sift: index %u is not an index of the planned relation", indexoid);
	return NULL;
}

/* Vars of the rel the scan hands to its parent or its own quals, then the sift.score expressions */
static List *
scan_tlist(PlannerInfo *root, RelOptInfo *rel, List *residual)
{
	List	   *vars = pull_var_clause((Node *) list_concat_copy(rel->reltarget->exprs, residual),
									   PVC_RECURSE_PLACEHOLDERS),
			   *exprs = NIL,
			   *tlist = NIL;
	int			resno = 1;

	foreach_node(Var, v, vars)
		if (v->varno == rel->relid)
			exprs = list_append_unique(exprs, v);
	exprs = list_concat(exprs, score_exprs(root, rel));
	foreach_node(Node, e, exprs)
		tlist = lappend(tlist, makeTargetEntry((Expr *) e, resno++, NULL, false));
	return tlist;
}

/*
 * The scan also outputs the scores the query reads: PG evaluates an expression above a join in the
 * join's own target unless a child already outputs it, and only the scan can compute sift.score
 */
static List *
score_outputs(PlannerInfo *root, RelOptInfo *rel, List *tlist)
{
	foreach_node(Node, e, score_exprs(root, rel))
		if (tlist_member((Expr *) e, tlist) == NULL)
			tlist = lappend(list_copy(tlist), makeTargetEntry((Expr *) e, list_length(tlist) + 1, NULL, true));
	return tlist;
}

static Plan *
sift_plan_scan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses,
			   List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	IndexOptInfo *ii = rel_index(rel, DatumGetObjectId(((Const *) linitial(best->custom_private))->constvalue));
	List	   *residual = NIL;

	foreach_node(RestrictInfo, rinfo, clauses)
	{
		Expr	   *q;

		if (!sift_match_clause((Node *) rinfo, ii, &q))
			residual = lappend(residual, rinfo);
	}
	residual = extract_actual_clauses(residual, false);
	cs->scan.plan.targetlist = score_outputs(root, rel, tlist);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_scan_tlist = scan_tlist(root, rel, residual);
	cs->custom_private = list_copy_head(best->custom_private, SIFT_NPRIV);
	cs->custom_exprs = list_make2(copyObject(list_nth(best->custom_private, SIFT_NPRIV)), residual);
	cs->methods = &sift_scan_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, ii->indexoid);
	if (custom_plans != NIL)
	{
		/* the bitmap heap scan was planned for its bitmap: the child is the bitmap plan under it */
		cs->custom_plans = list_make1(outerPlan(linitial(custom_plans)));
		sift_filter_plan_deps(root, linitial(cs->custom_plans));
	}
	return &cs->scan.plan;
}

static Plan *
sift_plan_count(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses,
				List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_scan_tlist = tlist;
	cs->custom_private = list_copy_head(best->custom_private, SIFT_NPRIV);
	cs->custom_exprs = list_make2(copyObject(list_nth(best->custom_private, SIFT_NPRIV)), NIL);
	cs->methods = &sift_count_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids,
										   DatumGetObjectId(((Const *) linitial(best->custom_private))->constvalue));
	return &cs->scan.plan;
}

const CustomPathMethods sift_scan_path_methods = {"SiftScan", sift_plan_scan, NULL};
const CustomPathMethods sift_count_path_methods = {"SiftCount", sift_plan_count, NULL};
const CustomScanMethods sift_scan_plan_methods = {"SiftScan", sift_create_scan_state};
const CustomScanMethods sift_count_plan_methods = {"SiftCount", sift_create_scan_state};

void
sift_planner_init(void)
{
	RegisterCustomScanMethods(&sift_scan_plan_methods);
	RegisterCustomScanMethods(&sift_count_plan_methods);
	sift_pushdown_init();
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sift_set_rel_pathlist;
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = sift_upper_paths;
}
