/*
 * sift_planner.c - planner side of the index: amcostestimate, the matching of
 * expressions to index keys, and the hooks that add SiftScan (heap-order or
 * ranked) and SiftCount paths (DESIGN.md (f)). Execution is in sift_cscan.c.
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
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "sift_am.h"
#include "sift_eval.h"
#include "sift_planner.h"
#include "sift_pushdown.h"

static set_rel_pathlist_hook_type prev_pathlist_hook;
static set_join_pathlist_hook_type prev_join_hook;
static create_upper_paths_hook_type prev_upper_hook;

/* Dictionary pages a term lookup reads: root array hit, DICTIDX, DICT block */
#define DICT_PAGES_PER_TERM		2.0
/* A posting costs about a byte of codec blob, tf sidecar included (docs/CODEC.md) */
#define BYTES_PER_POSTING		1.0
/* Decoding and combining postings, per posting in operator costs */
#define POSTING_CPU_FRACTION	(1.0 / 16)
/* Opening and scoring a block of heap pages in the ranker: its terms' cursors, bounds and kernels (measured, ~3 us) */
#define SCORE_BLOCK_COST		(170 * cpu_operator_cost)
/* The share of the blocks of the superblocks it visits that a ranker with a threshold still opens when their bounds do not tie (measured 0.3 to 0.55) */
#define SCORED_BLOCK_SHARE		0.4

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
		if (!IsA(lthird(args), Const) || !sift_index_serves(index->indexoid, DatumGetObjectId(((Const *) lthird(args))->constvalue)))
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
	foreach_ptr(Node, arg, queries)
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
	Cost		dict_cost;			/* the part of cost that every parallel participant pays: the dictionary lookups */
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
	w->cost = w->dict_cost = 0;
	if (!w->known)
		return;
	index = index_open(indexoid, AccessShareLock);
	sift_eval_estimate(index, texts, ntexts, &w->est);
	index_close(index, AccessShareLock);
	get_tablespace_page_costs(tablespace, &random_cost, &seq_cost);
	dict_pages = w->est.terms * DICT_PAGES_PER_TERM;
	w->pages = dict_pages + ceil(w->est.postings * BYTES_PER_POSTING / BLCKSZ);
	w->dict_cost = dict_pages * random_cost;
	w->cost = w->dict_cost + (w->pages - dict_pages) * seq_cost +
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

/* The expressions of the sort key's class that are about rel: its own, and for a partition the ones derived from its parent's */
static List *
pathkey_exprs(PathKey *pk, RelOptInfo *rel)
{
	EquivalenceMemberIterator it;
	EquivalenceMember *em;
	List	   *exprs = NIL;

	setup_eclass_member_iterator(&it, pk->pk_eclass, rel->relids);
	while ((em = eclass_member_iterator_next(&it)) != NULL)
		exprs = lappend(exprs, em->em_expr);
	return exprs;
}

/* A partition or inheritance child: the query is written for the top parent, so its score expressions are the parent's translated to the child */
static bool
is_partition(RelOptInfo *rel)
{
	return rel->reloptkind == RELOPT_OTHER_MEMBER_REL;
}

/* sift.score / max_score of rel in the final target list and in the query's first sort key */
static List *
score_exprs(PlannerInfo *root, RelOptInfo *rel)
{
	ScoreRefs	refs = {is_partition(rel) ? bms_singleton_member(rel->top_parent_relids) : rel->relid, NIL};
	List	   *own = NIL;

	score_walker((Node *) root->processed_tlist, &refs);
	foreach_ptr(Node, e, refs.found)
		own = lappend(own, is_partition(rel) ? adjust_appendrel_attrs_multilevel(root, e, rel, rel->top_parent) : e);
	refs = (ScoreRefs) {rel->relid, own};
	if (root->query_pathkeys != NIL)
		score_walker((Node *) pathkey_exprs(linitial_node(PathKey, root->query_pathkeys), rel), &refs);
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
	if (!sift_pathkey_descending(*pk))
		return false;
	foreach_ptr(Expr, e, pathkey_exprs(*pk, rel))
		if (sift_is_score_expr((Node *) e, rel->relid, &is_max) && !is_max)
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

/* Parallel paths exist only with sift.parallel_scan; without it the planner sees the serial paths alone */
static bool
parallel_ok(RelOptInfo *rel)
{
	return sift_parallel_scan && rel->consider_parallel;
}

/* The part of the index work every participant of a parallel scan pays (none for the serial paths) */
static Cost
parallel_dict_cost(IndexWork *w, int nworkers)
{
	return nworkers > 0 && w->known ? w->dict_cost : 0;
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

/* Matches per superblock at which the bounds of the superblocks tie with probability 1 - 1/e (measured: 185 and 800 do not, 2300 do) */
#define TIED_BOUND_MATCHES		3000

/* The probability that the superblocks' bounds tie: some match of each is at the top of the bound */
static double
bounds_tie(RelOptInfo *rel, double docs)
{
	double		per_sb = docs * SIFT_SB_PAGES / Max(rel->pages, 1) / TIED_BOUND_MATCHES;

	return 1 - exp(-per_sb * per_sb);
}

/*
 * The same when the quals are checked after scoring and a bitmap of them says where in the heap their
 * rows lie. The ranker takes the superblocks by descending bound, and where the bounds tie, which they
 * do when every superblock holds a match at the top of the bound (the more matches per superblock, the
 * likelier), in heap order. Then it has no threshold until it holds k passing rows: it verifies every
 * match before the first row the quals can keep (fe->start), ranks as above within the span of those
 * rows, where they are denser (pass / fe->span), and past the span still verifies the matches that beat
 * the k-th best of all the rows that pass (a fraction k / (cands * pass)). Otherwise the rows that pass
 * are met in no order of the heap: the plain estimate.
 */
static double
rank_verified_after(double cands, double pass, int64 k, const SiftFilterEst *fe, double ties)
{
	double		in_heap_order;

	if (k <= 0)
		return cands;
	in_heap_order = cands * fe->start + rank_verified(cands * fe->span, Min(pass / fe->span, 1.0), k) +
		cands * Max(1 - fe->start - fe->span, 0) * Min(k / Max(cands * pass, 1), 1.0);
	/* a span that covers much of the heap holds the first superblocks of any order with its rows */
	ties *= 1 - fe->span;
	return ties * in_heap_order + (1 - ties) * rank_verified(cands, pass, k);
}

/* The blocks of SIFT_BLK_PAGES heap pages that hold a match, when matches are spread over the fraction span of the heap */
static double
blocks_with_match(RelOptInfo *rel, double matches, double span)
{
	double		blocks = Max(ceil(rel->pages / (double) SIFT_BLK_PAGES), 1) * Max(span, 1e-9);

	return blocks * (1 - exp(-matches / blocks));
}

/* The blocks the ranker scores for matches in blocks: all of them while the bounds tie, the share with a threshold otherwise */
static double
blocks_scored(RelOptInfo *rel, double blocks, double matches)
{
	double		ties = bounds_tie(rel, matches);

	return blocks * (ties + (1 - ties) * SCORED_BLOCK_SHARE);
}

/*
 * Workers a parallel scan of the rel can use: the planner's rule for an index scan (reloption
 * parallel_workers, min_parallel_index_scan_size, max_parallel_workers_per_gather), and no more
 * participants than there are superblocks to hand out. 0: no parallel path.
 */
static int
scan_workers(RelOptInfo *rel, ScanCand *c)
{
	int			nworkers,
				sbs = ((int64) rel->pages + SIFT_SB_PAGES - 1) >> SIFT_SB_SHIFT;

	if (!parallel_ok(rel) || rel->reloptkind != RELOPT_BASEREL || max_parallel_workers_per_gather <= 0)
		return 0;
	nworkers = compute_parallel_worker(rel, -1, c->index->pages, max_parallel_workers_per_gather);
	if (rel->rel_parallel_workers == -1)
		nworkers = Min(nworkers, sbs - 1);
	return Max(nworkers, 0);
}

/* How many times faster the participants of nworkers workers do the shared work (cost_seqscan's divisor) */
static double
parallel_divisor(int nworkers)
{
	double		divisor = Max(nworkers, 1);

	if (nworkers > 0 && parallel_leader_participation)
		divisor = nworkers + Max(1.0 - 0.3 * nworkers, 0.0);
	return divisor;
}

/*
 * Every participant of a parallel scan pays for the dictionary lookups; the postings, the heap and
 * the verification are shared out. With a pushed down filter the ranker only meets the matches its
 * bitmap keeps (the fraction fe.sel of them, at least those that pass the quals), at the price of
 * building the bitmap first; the bitmap is built by one backend, so a pushed down scan is never
 * parallel (nworkers 0). Without the push the bitmap only tells where the rows that pass lie.
 */
static CustomPath *
scan_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, SiftScanMode mode, int64 k, List *pathkeys,
		  BitmapHeapPath *filter, bool push, int nworkers)
{
	CustomPath *cp = make_path(root->simple_rte_array[rel->relid]->relid, rel, c, mode, k,
							   CUSTOMPATH_SUPPORT_PROJECTION, &sift_scan_path_methods);
	ScanCost	sc;
	SiftFilterEst fe = {0, 1, 0, 1};
	double		divisor = parallel_divisor(nworkers),
				pass,
				verify;
	bool		limited;
	Cost		dict,
				postings,
				startup;

	Assert(!push || nworkers == 0);
	scan_cost(root, rel, c, &sc);
	dict = parallel_dict_cost(&sc.idx, nworkers);
	postings = sc.idx.known ? sc.idx.cost - dict : 0;
	cp->path.pathkeys = pathkeys;
	cp->path.rows = rel->rows / divisor;
	/*
	 * a ranked partition under a Parallel Append would reach a Sort that loses the scan's tie order:
	 * MergeAppend keeps it; and a ranked scan in a join under a Gather would not pass its score on
	 */
	cp->path.parallel_safe = parallel_ok(rel) &&
		!(mode == SIFT_MODE_RANKED && (is_partition(rel) || bms_membership(root->all_baserels) != BMS_SINGLETON));
	cp->path.parallel_aware = nworkers > 0;
	cp->path.parallel_workers = nworkers;
	verify = sc.docs * sc.tuple;
	if (mode == SIFT_MODE_RANKED)
	{
		pass = Min(rel->rows / sc.docs, 1.0);
		if (filter)
			sift_filter_estimate(root, rel, filter, &fe);
		if (push)
		{
			fe.sel = Max(fe.sel, pass);
			cp->custom_paths = list_make1(filter);
			dict += fe.cost;
			verify = rank_verified(sc.docs * fe.sel, pass / fe.sel, k) * sc.tuple;
			postings += blocks_scored(rel, blocks_with_match(rel, sc.docs * fe.sel, fe.span), sc.docs * fe.sel) * SCORE_BLOCK_COST;
		}
		else
		{
			verify = rank_verified_after(sc.docs, pass, k, &fe, bounds_tie(rel, sc.docs)) * sc.tuple;
			postings += blocks_scored(rel, blocks_with_match(rel, sc.docs, 1), sc.docs) * SCORE_BLOCK_COST;
		}
	}
	limited = mode == SIFT_MODE_RANKED && k > 0;
	startup = dict + (postings + (limited ? verify : 0)) / divisor;
	cp->path.startup_cost = startup;
	cp->path.total_cost = limited ? startup + cp->path.rows * cpu_tuple_cost : startup + verify / divisor;
	/*
	 * Drained past its k rows (a Sort over a plain Gather, which also loses the scan's tie order)
	 * a parallel ranked scan ranks every match: that is its total cost
	 */
	if (limited && nworkers > 0)
		cp->path.total_cost = dict + (postings + sc.docs * sc.tuple) / divisor;
	return cp;
}

static bool
cheaper(CustomPath *a, CustomPath *b)
{
	return b == NULL || compare_path_costs(&a->path, &b->path, TOTAL_COST) < 0;
}

/*
 * Adds the serial path, and a parallel one next to it unless the filter is pushed down: the bitmap of a
 * filter is built by one backend, so a pushed down scan stays serial and competes with the parallel one
 * on cost. The filter bitmap, when there is one, also prices the scan that does not push it down. With
 * only_best the paths are not added: the caller replaces the rel's paths with the best ones, and
 * add_path would free a path it rejects or dominates (a seq scan of an empty table) that best still
 * points to.
 */
static void
add_scan_paths(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, SiftScanMode mode, int64 k, List *pathkeys,
			   BitmapHeapPath *filter, bool push, bool parallel, bool only_best, CustomPath **best, CustomPath **best_partial)
{
	CustomPath *cp = scan_path(root, rel, c, mode, k, pathkeys, filter, push, 0);
	int			nworkers = !push && parallel ? scan_workers(rel, c) : 0;

	if (cheaper(cp, *best))
		*best = cp;
	if (!only_best)
		add_path(rel, &cp->path);
	if (nworkers > 0)
	{
		cp = scan_path(root, rel, c, mode, k, pathkeys, filter, false, nworkers);
		if (cheaper(cp, *best_partial))
			*best_partial = cp;
		if (!only_best)
			add_partial_path(rel, &cp->path);
	}
}

/* Does the rel have a restriction that is a sift match, of whichever index? */
static bool
has_match_clause(RelOptInfo *rel)
{
	const SiftCatalog *cat = sift_catalog();

	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		Node	   *clause = (Node *) rinfo->clause;

		if ((IsA(clause, OpExpr) && ((OpExpr *) clause)->opno == cat->match_op) ||
			(IsA(clause, FuncExpr) && (((FuncExpr *) clause)->funcid == cat->match_func ||
									   ((FuncExpr *) clause)->funcid == cat->match_idx_func)))
			return true;
	}
	return false;
}

/*
 * The max_score of a query is one value for all its rows, but each partition's scan only knows its own
 * (and ranks with its own index statistics), so it is not available through a partitioned parent.
 */
static void
check_partition_scores(RelOptInfo *rel, List *scores)
{
	bool		is_max;

	foreach_ptr(Node, e, scores)
		if (sift_is_score_expr(e, rel->relid, &is_max) && is_max)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("sift.max_score() is not available through a partitioned table"),
					 errhint("Call it on one partition, or use sift.score().")));
}

/* Is the path a SiftScan, or an Append of them, projected or not? */
static bool
scans_only(Path *path)
{
	List	   *subpaths;

	while (IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	if (IsA(path, CustomPath))
		return ((CustomPath *) path)->methods == &sift_scan_path_methods;
	if (IsA(path, AppendPath))
		subpaths = ((AppendPath *) path)->subpaths;
	else if (IsA(path, MergeAppendPath))
		subpaths = ((MergeAppendPath *) path)->subpaths;
	else
		return false;
	foreach_ptr(Path, sub, subpaths)
		if (!scans_only(sub))
			return false;
	return true;
}

static bool
is_append(Path *path)
{
	return IsA(path, AppendPath) || IsA(path, MergeAppendPath);
}

static bool
has_scan_path(RelOptInfo *rel)
{
	foreach_ptr(Path, p, rel->pathlist)
		if (scans_only(p))
			return true;
	return false;
}

/* The scores the query reads of the rels under a join that a SiftScan produces */
static List *
join_scores(PlannerInfo *root, RelOptInfo *joinrel)
{
	List	   *scores = NIL;
	int			relid = -1;

	while ((relid = bms_next_member(joinrel->relids, relid)) >= 0)
	{
		RelOptInfo *rel = root->simple_rel_array[relid];

		if (rel != NULL && rel->reloptkind == RELOPT_BASEREL && has_scan_path(rel))
			scores = list_concat_unique(scores, score_exprs(root, rel));
	}
	return scores;
}

/*
 * The path's own target also outputs the scores: the rel's target holds only Vars, so a join above would
 * call sift.score itself. A path of the rel that holds every base rel gets its target from the query's
 * scan/join target instead (and its sort and group references), which has the scores it needs.
 */
static void
carry_scores(Path *path, List *scores)
{
	foreach_ptr(Node, e, scores)
	{
		if (list_member(path->pathtarget->exprs, e))
			continue;
		if (path->pathtarget == path->parent->reltarget)
			path->pathtarget = copy_pathtarget(path->pathtarget);
		add_column_to_pathtarget(path->pathtarget, (Expr *) e, 0);
	}
}

static bool
holds_all_rels(PlannerInfo *root, RelOptInfo *rel)
{
	return bms_is_subset(root->all_baserels, rel->relids);
}

/*
 * The Append of a partitioned parent outputs the scores of its partitions' scans, which come last in
 * their target lists (score_outputs), so that the joins and aggregates above it find them. An Append
 * takes its columns by position: only one of scans is extended.
 */
static void
carry_parent_scores(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *scores = score_exprs(root, rel);

	foreach_ptr(Path, p, rel->pathlist)
		if (is_append(p) && scans_only(p))
			carry_scores(p, scores);
}

/* The Append of a partitionwise join takes the columns of the joins under it, which do not carry the scores */
static void
carry_join_scores(PlannerInfo *root, RelOptInfo *joinrel)
{
	List	   *scores = join_scores(root, joinrel);

	foreach_ptr(Path, p, joinrel->pathlist)
		if (!is_append(p))
			carry_scores(p, scores);
	foreach_ptr(Path, p, joinrel->partial_pathlist)
		if (!is_append(p))
			carry_scores(p, scores);
}

/*
 * A join passes on the columns the joins above it need, and those are Vars: the score a join higher up
 * reads is not one, and would be evaluated there from the ctid. The join paths pass the scan's score on.
 */
static void
sift_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel,
					   JoinType jointype, JoinPathExtraData *extra)
{
	if (prev_join_hook)
		prev_join_hook(root, joinrel, outerrel, innerrel, jointype, extra);
	if (!holds_all_rels(root, joinrel))
		carry_join_scores(root, joinrel);
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
				only_best,
				filter_looked = false;
	BitmapHeapPath *filter = NULL;
	CustomPath *best = NULL,
			   *best_partial = NULL;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (!sift_enable_scan || rte->rtekind != RTE_RELATION || rte->tablesample ||
		root->parse->commandType != CMD_SELECT || root->rowMarks != NIL ||
		(rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		sift_catalog()->am == InvalidOid)
		return;
	if (rte->inh)
	{
		if (!holds_all_rels(root, rel))
			carry_parent_scores(root, rel);
		return;
	}
	if (rte->relkind != RELKIND_RELATION)
		return;
	scores = score_exprs(root, rel);
	if (is_partition(rel))
		check_partition_scores(rel, scores);
	ranked = sorted_by_score(root, rel, &pk);
	only_best = ranked || scores != NIL;
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		ScoreRefs	refs = {rti, NIL};

		score_walker((Node *) rinfo->clause, &refs);
		if (refs.found != NIL)
			return;
	}
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		int64		k = root->limit_tuples > 0 ? (int64) ceil(root->limit_tuples) : 0;
		SiftScanMode mode = only_best ? SIFT_MODE_RANKED : SIFT_MODE_UNORDERED;
		List	   *pathkeys = ranked ? list_make1(pk) : NIL;
		/*
		 * A ranked scan shares its threshold, which needs the number of rows it is asked for, and
		 * only the scan can compute the score, so nothing may sit between it and the query's
		 * target list: not a Gather below a join, which would not pass the score on
		 */
		bool		parallel = mode == SIFT_MODE_UNORDERED ||
			(ranked && k > 0 && bms_membership(root->all_baserels) == BMS_SINGLETON);

		if (!split_quals(rel, ii, &c))
			continue;
		if (mode == SIFT_MODE_RANKED && !filter_looked)
		{
			filter = sift_filter_bitmap_path(root, rel);
			filter_looked = true;
		}
		if (filter == NULL || sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
			add_scan_paths(root, rel, &c, mode, ranked ? k : 0, pathkeys, filter, false, parallel, only_best, &best, &best_partial);
		if (filter)
			add_scan_paths(root, rel, &c, mode, ranked ? k : 0, pathkeys, filter, true, false, only_best, &best, &best_partial);
	}
	if (best && only_best)
	{
		rel->pathlist = list_make1(best);
		rel->partial_pathlist = best_partial ? list_make1(best_partial) : NIL;
	}
	else if (only_best && is_partition(rel) && has_match_clause(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.score() needs a sift index on every partition the query reads"),
				 errdetail("\"%s\" has no sift index that answers the @@> condition.", get_rel_name(rte->relid))));
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

/* The count of the participants of nworkers workers (0: one process) */
static CustomPath *
count_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid, int nworkers)
{
	CustomPath *cp = make_path(heapoid, output_rel, c, SIFT_MODE_COUNT, 0, 0, &sift_count_path_methods);
	ScanCost	sc;
	double		divisor = parallel_divisor(nworkers);
	Cost		dict,
				shared;

	scan_cost(root, input_rel, c, &sc);
	dict = parallel_dict_cost(&sc.idx, nworkers);
	/* only matches on pages that are not all-visible are checked in the heap, a page at a time */
	shared = (sc.idx.known ? sc.idx.cost - dict : 0) +
		(1 - input_rel->allvisfrac) * (Min(sc.docs, Max(input_rel->pages, 1)) * sc.page + sc.docs * cpu_tuple_cost) +
		sc.docs * cpu_operator_cost * POSTING_CPU_FRACTION;
	cp->path.rows = 1;
	cp->path.parallel_safe = parallel_ok(output_rel);
	cp->path.parallel_aware = nworkers > 0;
	cp->path.parallel_workers = nworkers;
	cp->path.startup_cost = dict + shared / divisor;
	cp->path.total_cost = cp->path.startup_cost + cpu_tuple_cost;
	return cp;
}

/*
 * Finalize Aggregate over Gather over the participants' partial counts, each a row of the count's
 * partial aggregate state (an int8 that the final aggregate adds up).
 */
static void
add_parallel_count(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid)
{
	int			nworkers = scan_workers(input_rel, c);
	PathTarget *partial_target = create_empty_pathtarget();
	CustomPath *cp;
	GatherPath *gather;
	AggClauseCosts final_costs;
	double		rows = nworkers + 1;

	if (nworkers == 0)
		return;
	foreach_node(Aggref, agg, output_rel->reltarget->exprs)
	{
		Aggref	   *partial = copyObject(agg);

		mark_partial_aggref(partial, AGGSPLIT_INITIAL_SERIAL);
		add_column_to_pathtarget(partial_target, (Expr *) partial, 0);
	}
	set_pathtarget_cost_width(root, partial_target);
	cp = count_path(root, input_rel, output_rel, c, heapoid, nworkers);
	cp->path.pathtarget = partial_target;
	gather = create_gather_path(root, output_rel, &cp->path, partial_target, NULL, &rows);
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &final_costs);
	add_path(output_rel, (Path *) create_agg_path(root, output_rel, &gather->path, output_rel->reltarget, AGG_PLAIN,
												  AGGSPLIT_FINAL_DESERIAL, NIL, NIL, &final_costs, 1));
}

/* A Sort over a plain Gather of a parallel ranked scan: it drains the scan and breaks its tie order */
static bool
sorted_gather_of_ranked(Path *path)
{
	Path	   *sub;

	if (!IsA(path, SortPath) || !IsA(((SortPath *) path)->subpath, GatherPath))
		return false;
	sub = ((GatherPath *) ((SortPath *) path)->subpath)->subpath;
	while (IsA(sub, ProjectionPath))
		sub = ((ProjectionPath *) sub)->subpath;
	return IsA(sub, CustomPath) && ((CustomPath *) sub)->methods == &sift_scan_path_methods && sub->parallel_aware &&
		DatumGetInt32(lsecond_node(Const, ((CustomPath *) sub)->custom_private)->constvalue) == SIFT_MODE_RANKED;
}

static void
sift_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
				 void *extra)
{
	Query	   *parse = root->parse;
	ScanCand	c;
	CustomPath *cp;
	RangeTblEntry *rte;

	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	/* the aggregates read the scores out of the scan/join target, which holds only the Vars they use */
	if (stage == UPPERREL_GROUP_AGG && input_rel->reloptkind == RELOPT_JOINREL)
		carry_join_scores(root, input_rel);
	else if (stage == UPPERREL_GROUP_AGG && input_rel->reloptkind == RELOPT_BASEREL &&
			 planner_rt_fetch(input_rel->relid, root)->inh)
		carry_parent_scores(root, input_rel);
	if (stage == UPPERREL_ORDERED)
		foreach_ptr(Path, p, list_copy(output_rel->pathlist))
			if (sorted_gather_of_ranked(p))
				output_rel->pathlist = list_delete_ptr(output_rel->pathlist, p);
	if (stage != UPPERREL_GROUP_AGG || !sift_enable_count || sift_catalog()->am == InvalidOid ||
		!parse->hasAggs || parse->groupClause != NIL || parse->groupingSets != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->havingQual != NULL || parse->commandType != CMD_SELECT ||
		root->rowMarks != NIL || input_rel->reloptkind != RELOPT_BASEREL ||
		bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rte = root->simple_rte_array[input_rel->relid];
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample || rte->inh)
		return;
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		if (!plain_count(e))
			return;
	if (!count_cand(input_rel, &c))
		return;
	cp = count_path(root, input_rel, output_rel, &c, rte->relid, 0);
	add_path(output_rel, &cp->path);
	add_parallel_count(root, input_rel, output_rel, &c, rte->relid);
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
	foreach_ptr(Node, e, exprs)
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
	foreach_ptr(Node, e, score_exprs(root, rel))
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
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sift_set_rel_pathlist;
	prev_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = sift_set_join_pathlist;
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = sift_upper_paths;
}
