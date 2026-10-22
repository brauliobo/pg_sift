/*
 * bm25/planner_path.c - the costs of an index scan and the SiftScan and SiftCount paths: heap-order, ranked, keyed,
 * parallel and gated scans, and the counts.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
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
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "agg/agg.h"
#include "bm25/am.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/pushdown.h"

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

/* Does the operand hold an array of queries (an ANY qual) rather than one query text? */
static bool
is_query_array(Node *arg)
{
	return OidIsValid(get_element_type(exprType(arg)));
}

/* The queries of the constant operands, an array's non-NULL elements as one source; false when one is not a constant or an array has no element */
static bool
const_queries(List *queries, SiftQuerySrc **srcs, int *nsrcs)
{
	*srcs = palloc(list_length(queries) * sizeof(SiftQuerySrc));
	*nsrcs = 0;
	foreach_ptr(Node, arg, queries)
	{
		SiftQuerySrc *src = &(*srcs)[(*nsrcs)++];

		while (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (!IsA(arg, Const) || ((Const *) arg)->constisnull)
			return false;
		if (is_query_array(arg))
		{
			ArrayType  *arr = DatumGetArrayTypeP(((Const *) arg)->constvalue);
			Datum	   *elems;
			bool	   *nulls;
			int			n;

			deconstruct_array(arr, ARR_ELEMTYPE(arr), -1, false, TYPALIGN_INT, &elems, &nulls, &n);
			src->texts = palloc(n * sizeof(text *));
			src->ntexts = 0;
			src->any = true;
			for (int i = 0; i < n; i++)
				if (!nulls[i])
					src->texts[src->ntexts++] = DatumGetTextPP(elems[i]);
			if (src->ntexts == 0)
				return false;
			continue;
		}
		src->texts = palloc(sizeof(text *));
		src->texts[0] = DatumGetTextPP(((Const *) arg)->constvalue);
		src->ntexts = 1;
		src->any = false;
	}
	return true;
}

/* The query operands of the index quals; NIL when one is an array qual (ANY): the bitmap scan runs it as one scan per element, which this estimate does not cover */
static List *
qual_query_exprs(IndexPath *path)
{
	List	   *queries = NIL;

	foreach_node(IndexClause, ic, path->indexclauses)
		foreach_node(RestrictInfo, rinfo, ic->indexquals)
		{
			if (!IsA(rinfo->clause, OpExpr))
				return NIL;
			queries = lappend(queries, lsecond(castNode(OpExpr, rinfo->clause)->args));
		}
	return queries;
}

static void
index_work(Oid indexoid, Oid tablespace, List *queries, IndexWork *w)
{
	SiftQuerySrc *srcs;
	int			nsrcs;
	double		random_cost,
				seq_cost,
				dict_pages;
	Relation	index;

	w->known = queries != NIL && const_queries(queries, &srcs, &nsrcs);
	w->cost = w->dict_cost = 0;
	if (!w->known)
		return;
	index = index_open(indexoid, AccessShareLock);
	sift_estimate_cached_src(index, srcs, nsrcs, &w->est);
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

/*
 * Matches of a ranked query the planner cannot read (a parameter of a generic plan), as a fraction of
 * the rows, for placing the filter's entries in the heap: the queries a ranker is asked for hold common
 * words (26% to 78% of the articles in the benchmark's ranked classes)
 */
#define UNKNOWN_QUERY_SEL		0.1

/* What fetching docs matches costs per heap tuple; docs is clamped to the rows the rel keeps and has */
static void
scan_cost_docs(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, double docs, ScanCost *sc)
{
	double		random_cost,
				seq_cost,
				pages = Max(rel->pages, 1),
				fetched;

	sc->docs = Min(Max(docs, rel->rows), Max(rel->tuples, 1));
	get_tablespace_page_costs(rel->reltablespace, &random_cost, &seq_cost);
	fetched = Min(index_pages_fetched(sc->docs, rel->pages, c->index->pages, root), pages);
	sc->page = random_cost - (random_cost - seq_cost) * sqrt(fetched / pages);
	sc->docs = Max(sc->docs, 1);
	sc->tuple = cpu_tuple_cost + sc->qual + fetched / sc->docs * sc->page;
}

static void
scan_cost(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, bool ranked, ScanCost *sc)
{
	QualCost	qual;

	index_work(c->index->indexoid, c->index->reltablespace, c->queries, &sc->idx);
	cost_qual_eval(&qual, c->residual, root);
	sc->qual = qual.per_tuple;
	scan_cost_docs(root, rel, c, sc->idx.known ? sc->idx.est.docs : ranked ? rel->tuples * UNKNOWN_QUERY_SEL : rel->rows, sc);
	/* rows holds the default match fraction when the query is unknown: the quals' own selectivity is what passes */
	sc->pass = sc->idx.known || !ranked ? Min(rel->rows / sc->docs, 1.0) : clauselist_selectivity(root, c->residual, rel->relid, JOIN_INNER, NULL);
}

/* Does an index qual hold an array of queries (an ANY qual)? */
static bool
has_query_array(ScanCand *c)
{
	foreach_ptr(Node, q, c->queries)
		if (is_query_array(q))
			return true;
	return false;
}

/*
 * The most rows of an unscored match list of an ANY qual that the unordered scan takes. Past it PostgreSQL's bitmap heap scan, which
 * its parallel workers share, is the faster plan (a full list of 133,000 to 637,000 of 1,000,000 narrow rows took the scan 1.3 to 1.45
 * times as long, serial; up to 28,000 matches, or 45,000 of 50,000 wide rows, it is as fast or faster: CHANGELOG.md), so the scan is
 * not offered, as before. A LIMIT that stops it earlier counts, and then the scan is the plan (10 to 60 times faster: the bitmap
 * is built whole before its first row), as is the count, which never reads the rows.
 */
#define ANY_LIST_MAX_ROWS		100000

bool
any_list_too_long(PlannerInfo *root, ScanCand *c, const ScanCost *sc)
{
	double		rows = sc->docs;
	int64		k = scan_limit(root, false);

	if (!has_query_array(c))
		return false;
	return (k > 0 ? Min(rows, (double) k) : rows) > ANY_LIST_MAX_ROWS;
}

/* ko: the leading sort keys the scan sorts by, NULL for none; the path's private list is SIFT_NPRIV fields, then the queries, then the keys' expressions */
static CustomPath *
make_path(Oid heapoid, RelOptInfo *parent, ScanCand *c, SiftScanMode mode, int64 k, uint32 flags,
		  const CustomPathMethods *methods, const KeyedOrder *ko)
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
									makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8), Float8GetDatum(0), false, FLOAT8PASSBYVAL));
	cp->custom_private = lappend(cp->custom_private, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8), Float8GetDatum(get_float8_infinity()), false, FLOAT8PASSBYVAL));
	cp->custom_private = lappend(cp->custom_private, ko ? ko->keyops : NIL);
	cp->custom_private = lappend(cp->custom_private, c->queries);
	cp->custom_private = lappend(cp->custom_private, ko ? ko->exprs : NIL);
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
 * The filter's entries a post-filter ranker has to meet to hold k rows that pass: k matches at the
 * fraction of the rows the query matches
 */
static double
filter_entries(RelOptInfo *rel, const ScanCost *sc, int64 k)
{
	return k > 0 ? k * Max(rel->tuples, 1) / sc->docs : 0;
}

void
scan_est(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, SiftScanMode mode, int64 k, BitmapHeapPath *filter, ScanEst *est)
{
	est->fe = (SiftFilterEst) {0, 1, 0, 1};
	scan_cost(root, rel, c, mode == SIFT_MODE_RANKED, &est->sc);
	if (mode == SIFT_MODE_RANKED && filter)
		sift_filter_estimate(root, rel, filter, filter_entries(rel, &est->sc, k), &est->fe);
}

/*
 * Every participant of a parallel scan pays for the dictionary lookups; the postings, the heap and
 * the verification are shared out. With a pushed down filter the ranker only meets the matches its
 * bitmap keeps (the fraction fe.sel of them, at least those that pass the quals), at the price of
 * building the bitmap first; the bitmap is built by one backend, so a pushed down scan is never
 * parallel (nworkers 0). Without the push the bitmap only tells where the rows that pass lie.
 */
static CustomPath *
scan_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, SiftScanMode mode, int64 k, List *pathkeys,
		  BitmapHeapPath *filter, bool push, int nworkers)
{
	CustomPath *cp = make_path(root->simple_rte_array[rel->relid]->relid, rel, c, mode, k,
							   CUSTOMPATH_SUPPORT_PROJECTION, &sift_scan_path_methods, NULL);
	ScanCost	sc = est->sc;
	SiftFilterEst fe = est->fe;
	double		divisor = parallel_divisor(nworkers),
				pass,
				verify;
	bool		limited;
	Cost		dict,
				postings,
				startup;

	Assert(!push || nworkers == 0);
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
		pass = sc.pass;
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
	/*
	 * A ranked scan with a limit hint costs what its k rows take up to its startup cost. Drained past
	 * them (under a join that keeps few of its rows, a hash join, a sort) it ranks again for four
	 * times as many, to the last match (sift_rank_next): that is its total cost, the one a consumer
	 * that stops after a fraction of the rows interpolates from
	 */
	cp->path.total_cost = limited ? Max(dict + (postings + sc.docs * (push ? fe.sel : 1) * sc.tuple) / divisor, startup) :
		startup + verify / divisor;
	return cp;
}

/* What the scan costs to hand out the rows its limit hint asks for (its total cost is the one of draining it) */
static Cost
pick_cost(CustomPath *cp)
{
	bool		hinted = DatumGetInt64(list_nth_node(Const, cp->custom_private, SIFT_PRIV_K)->constvalue) > 0;

	return hinted && cp->path.parallel_workers == 0 ? cp->path.startup_cost + cp->path.rows * cpu_tuple_cost : cp->path.total_cost;
}

/*
 * The scan is the only path of its rel, so the pick between a pushed down filter and none, and between
 * indexes, is by what the limit asks of it: what a join above drains cannot tell them apart
 */
bool
cheaper(CustomPath *a, CustomPath *b)
{
	return b == NULL || pick_cost(a) < pick_cost(b);
}

/*
 * Adds the serial path, and a parallel one next to it unless the filter is pushed down: the bitmap of a
 * filter is built by one backend, so a pushed down scan stays serial and competes with the parallel one
 * on cost. The filter bitmap, when there is one, also prices the scan that does not push it down. With
 * only_best the paths are not added: the caller replaces the rel's paths with the best ones, and
 * add_path would free a path it rejects or dominates (a seq scan of an empty table) that best still
 * points to.
 */
void
add_scan_paths(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, SiftScanMode mode, int64 k, List *pathkeys,
			   BitmapHeapPath *filter, bool push, bool parallel, bool only_best, CustomPath **best, CustomPath **best_partial)
{
	CustomPath *cp = scan_path(root, rel, c, est, mode, k, pathkeys, filter, push, 0);
	int			nworkers = !push && parallel ? scan_workers(rel, c) : 0;

	if (cheaper(cp, *best))
		*best = cp;
	if (!only_best)
		add_path(rel, &cp->path);
	if (nworkers > 0)
	{
		cp = scan_path(root, rel, c, est, mode, k, pathkeys, filter, false, nworkers);
		if (cheaper(cp, *best_partial))
			*best_partial = cp;
		if (!only_best)
			add_partial_path(rel, &cp->path);
	}
}

/* Rows a round of a keyed scan keeps: the k of the limit, or what the planner assumes of the limit of a generic plan */
static double
keyed_rows(PlannerInfo *root, const ScanCost *sc, int64 k)
{
	double		fraction = root->tuple_fraction;

	return Min(k > 0 ? k : Max(fraction >= 1 ? fraction : fraction * sc->docs, 1), sc->docs);
}

/* Growth of the rows a keyed scan keeps in its next round, as the ranker grows its search (sift_rank_next_k) */
#define KEYED_GROWTH	16

/* What ranking `rows` of the matches costs a keyed scan: their blocks, a verification and a fetch each, and the sort of them */
static Cost
keyed_rank_cost(RelOptInfo *rel, const ScanCost *sc, int nkeys, double rows)
{
	return blocks_with_match(rel, rows, 1) * SCORE_BLOCK_COST +
		rows * (2 * sc->tuple + cpu_tuple_cost + log2(rows + 2) * nkeys * cpu_operator_cost);
}

/*
 * ORDER BY keys, score DESC LIMIT k: a pass over the matches in heap order keeps the rows of the k best keys and
 * those tied with the k-th, and the ranker scores only them. A consumer that drops rows and asks for more makes
 * it pass again for 16 times as many (the total cost, which a consumer that stops early interpolates away).
 */
CustomPath *
keyed_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, const KeyedOrder *ko, int64 k)
{
	CustomPath *cp = make_path(root->simple_rte_array[rel->relid]->relid, rel, c, SIFT_MODE_RANKED, k,
							   CUSTOMPATH_SUPPORT_PROJECTION, &sift_scan_path_methods, ko);
	const ScanCost *sc = &est->sc;
	int			nkeys = list_length(ko->exprs);
	double		held = keyed_rows(root, sc, k),
				rounds = 1 + ceil(log(Max(sc->docs / held, 1)) / log(KEYED_GROWTH));
	Cost		pass = sc->idx.cost + sc->docs * (sc->tuple + nkeys * cpu_operator_cost) +
		held * log(sc->docs / held + 1) * log2(held + 2) * nkeys * cpu_operator_cost;

	cp->path.pathkeys = ko->pathkeys;
	cp->path.rows = rel->rows;
	cp->path.parallel_safe = false;
	cp->path.startup_cost = pass + keyed_rank_cost(rel, sc, nkeys, held);
	cp->path.total_cost = rounds * pass + keyed_rank_cost(rel, sc, nkeys, sc->docs);
	return cp;
}

/*
 * A query the planner cannot read (a parameter of a generic plan) with a filter that can be pushed down
 * has no plan that is right for every query: the common words of a ranker's queries want the bitmap, a
 * rare query pays more for it than it saves. The plan keeps the bitmap as a child that the scan builds
 * only when the bound query matches a number of documents inside a window, found on the costs of both
 * paths. The window opens where the pushed down plan is expected to cost a quarter of the other (the costs
 * are estimates, and a bitmap that is not clearly worth its build is a loss on data in memory, where
 * checking a candidate costs less than the planner charges) and stays open while it is not dearer. The plan
 * is priced as the post-filter scan of the operator's default match fraction, whatever the query turns
 * out to be, so the plan cache keeps reusing it as it reuses a plan without a filter.
 */
#define GATE_STEP		4
#define GATE_REFINE		6
#define GATE_OPEN		0.25
#define GATE_CLOSE		1.0

/* What the pushed down plan costs per cost of checking the filter after the ranker, for docs matches */
static double
push_ratio(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, int64 k, List *pathkeys, BitmapHeapPath *filter,
		   double docs)
{
	ScanEst		e = *est;

	scan_cost_docs(root, rel, c, docs, &e.sc);
	return pick_cost(scan_path(root, rel, c, &e, SIFT_MODE_RANKED, k, pathkeys, filter, true, 0)) /
		Max(pick_cost(scan_path(root, rel, c, &e, SIFT_MODE_RANKED, k, pathkeys, filter, false, 0)), 1e-9);
}

/* Where the ratio crosses `limit` between the match counts `below` (under it) and `above` (over it): the side under */
static double
push_edge(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, int64 k, List *pathkeys, BitmapHeapPath *filter,
		  double limit, double below, double above)
{
	for (int j = 0; j < GATE_REFINE; j++)
	{
		double		mid = sqrt(below * above);

		if (push_ratio(root, rel, c, est, k, pathkeys, filter, mid) < limit)
			below = mid;
		else
			above = mid;
	}
	return below;
}

/* The match counts between which the pushed down filter is worth building: false when there are none */
static bool
push_window(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, int64 k, List *pathkeys, BitmapHeapPath *filter,
			double *from, double *to)
{
	double		top = Max(rel->tuples, 1),
				prev = 0,
				first = 0;

	for (double docs = 1;; docs = Min(docs * GATE_STEP, top))
	{
		double		ratio = push_ratio(root, rel, c, est, k, pathkeys, filter, docs);

		if (first == 0 && ratio < GATE_OPEN)
		{
			first = docs;
			*from = prev > 0 ? push_edge(root, rel, c, est, k, pathkeys, filter, GATE_OPEN, first, prev) : 0;
		}
		else if (first > 0 && ratio >= GATE_CLOSE)
		{
			*to = push_edge(root, rel, c, est, k, pathkeys, filter, GATE_CLOSE, prev, docs);
			return true;
		}
		if (docs >= top)
			break;
		prev = docs;
	}
	*to = get_float8_infinity();
	return first > 0;
}

void
unknown_query_path(PlannerInfo *root, RelOptInfo *rel, ScanCand *c, const ScanEst *est, int64 k, List *pathkeys, BitmapHeapPath *filter,
				   bool parallel, CustomPath **best, CustomPath **best_partial)
{
	ScanEst		def = {.fe = {0, 1, 0, 1}};
	double		from = 0,
				to = 0;
	CustomPath *cp;

	scan_cost(root, rel, c, false, &def.sc);
	if (!filter || !push_window(root, rel, c, est, k, pathkeys, filter, &from, &to))
	{
		add_scan_paths(root, rel, c, &def, SIFT_MODE_RANKED, k, pathkeys, filter, false, parallel, true, best, best_partial);
		return;
	}
	cp = scan_path(root, rel, c, &def, SIFT_MODE_RANKED, k, pathkeys, filter, false, 0);
	cp->custom_paths = list_make1(filter);
	list_nth_node(Const, cp->custom_private, SIFT_PRIV_GATE)->constvalue = Float8GetDatum(from);
	list_nth_node(Const, cp->custom_private, SIFT_PRIV_GATE_TO)->constvalue = Float8GetDatum(to);
	if (cheaper(cp, *best))
		*best = cp;
}

/* The count of the participants of nworkers workers (0: one process) */
CustomPath *
count_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid, int nworkers)
{
	CustomPath *cp = make_path(heapoid, output_rel, c, SIFT_MODE_COUNT, 0, 0, &sift_count_path_methods, NULL);
	ScanCost	sc;
	double		divisor = parallel_divisor(nworkers);
	Cost		dict,
				shared;

	scan_cost(root, input_rel, c, false, &sc);
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
void
add_parallel_count(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, ScanCand *c, Oid heapoid)
{
	int			nworkers = scan_workers(input_rel, c);
	PathTarget *partial_target = create_empty_pathtarget();
	CustomPath *cp;
	GatherPath *gather;
	AggClauseCosts final_costs = {0};
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
