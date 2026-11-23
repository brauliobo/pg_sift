/*
 * bm25/score_fallback.c - SiftScoreScan: the scores of a relation that no sift index scan serves (no index, an index that does
 * not answer the match, sift.enable_scan off, a policy that keeps the index scan away).
 *
 * The scan reads the heap twice. The first pass counts the corpus (the rows that pass the security quals and have a text) and the
 * document frequency of every leaf of the queries, and keeps the rows that pass the other quals; the second weighs the leaves with
 * the formulas of the index and scores the kept rows. Nothing but rows that pass the security quals is counted, so the scores
 * of a role never depend on the rows that a policy hides from it.
 */
#include "postgres.h"

#include <math.h>

#include "access/tableam.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "nodes/makefuncs.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "bm25/analyze.h"
#include "bm25/cscan_int.h"
#include "bm25/kernels.h"
#include "bm25/planner_int.h"
#include "bm25/query.h"
#include "bm25/score_fallback.h"
#include "core/memtrack.h"
#include "core/sift.h"
#include "core/sql.h"
#include "storage/predicate.h"

/* A word the scores count: a term, or a leaf that matches several words, and the phrase that has to hold for it */
typedef struct FbLeaf
{
	const SiftQNode *node;
	const SiftQNode *gate;
	char	   *term;				/* a word of a phrase: its index term; NULL for a leaf of the query */
	int			len;
	float4		boost,
				w;
	uint64		df;
} FbLeaf;

/* One match clause: the text it reads, the query it asks, and the corpus of that text */
typedef struct FbClause
{
	ExprState  *doc,
			   *query;
	bool		any;				/* the query operand is an array, ORed */
	Oid			index;				/* the analyzer of this index; InvalidOid: the default one */
	SiftAnalyzer *analyzer;
	SiftQuery  *q;
	FbLeaf	   *leaf;
	int			nleaf,
				maxleaf;
	uint64		ndocs,
				sum_dl;
	SiftBm25	bm;
} FbClause;

typedef struct FbState
{
	SiftScanState ss;
	ExprState  *secure;
	int			nclause;
	FbClause   *cl;
	MemoryContext cxt;				/* what one pass builds; a rescan resets it */
	Tuplestorestate *rows;
	TupleTableSlot *rowslot,
			   *loadslot;
	bool		need_max;
} FbState;

static void
add_leaf(FbClause *c, FbLeaf leaf)
{
	if (c->nleaf == c->maxleaf)
	{
		c->maxleaf = Max(8, 2 * c->maxleaf);
		c->leaf = c->leaf ? repalloc(c->leaf, c->maxleaf * sizeof(FbLeaf)) : palloc(c->maxleaf * sizeof(FbLeaf));
	}
	c->leaf[c->nleaf++] = leaf;
}

static bool
same_term(const SiftQNode *n, int i, int j)
{
	return n->termlens[i] == n->termlens[j] && memcmp(n->terms[i], n->terms[j], n->termlens[i]) == 0;
}

/* The leaves that score: those a NOT does not hold; a phrase scores its distinct terms, once it holds */
static void
collect_leaves(FbClause *c, const SiftQNode *n)
{
	check_stack_depth();
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				collect_leaves(c, n->child[i]);
			break;
		case SQ_NOT:
		case SQ_NOTHING:
		case SQ_ALL:
			break;
		case SQ_PHRASE:
			for (int i = 0, j; i < n->nterms; i++)
			{
				int			skip = n->field ? 1 : 0;
				char	   *term = palloc(n->termlens[i] + skip);

				for (j = 0; j < i && !same_term(n, i, j); j++)
					;
				if (j < i)
					continue;
				if (skip)
					term[0] = SIFT_FIELD_BYTE(n->field);
				memcpy(term + skip, n->terms[i], n->termlens[i]);
				add_leaf(c, (FbLeaf) {.gate = n, .term = term, .len = n->termlens[i] + skip, .boost = n->boost});
			}
			break;
		default:
			add_leaf(c, (FbLeaf) {.node = n, .boost = n->boost});
	}
}

static int
leaf_tf(const FbLeaf *l, SiftDoc *d)
{
	return l->term ? sift_doc_term_tf(d, l->term, l->len) : sift_doc_leaf_tf(d, l->node);
}

/* The query an operand asks, parsed in the current context: the text, or the OR of the elements of an array; NULL when an array has none */
static SiftQuery *
parse_operand(Datum d, bool any)
{
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n,
				m = 0;
	const char **src;
	int		   *len;

	if (!any)
	{
		text	   *t = DatumGetTextPP(d);

		return sift_query_parse(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	}
	arr = DatumGetArrayTypeP(d);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), -1, false, TYPALIGN_INT, &elems, &nulls, &n);
	src = palloc(Max(n, 1) * sizeof(char *));
	len = palloc(Max(n, 1) * sizeof(int));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
		{
			text	   *t = DatumGetTextPP(elems[i]);

			src[m] = VARDATA_ANY(t);
			len[m++] = VARSIZE_ANY_EXHDR(t);
		}
	return m > 0 ? sift_query_parse_any(src, len, m) : NULL;
}

/* The query of a clause, analyzed with its leaves collected; false when the operator is strict about a NULL operand */
static bool
bind_clause(FbState *fb, FbClause *c, ExprContext *econtext)
{
	bool		isnull;
	Datum		d = ExecEvalExprSwitchContext(c->query, econtext, &isnull);
	SiftAnalyzerConfig cfg;
	SiftQuery  *raw;
	MemoryContext old;

	if (isnull)
		return false;
	old = MemoryContextSwitchTo(fb->cxt);
	if (OidIsValid(c->index))
		sift_analyzer_config_lookup_index(c->index, &cfg);
	else
		sift_analyzer_config_default(&cfg);
	c->analyzer = sift_analyzer_create(&cfg, fb->cxt);
	raw = parse_operand(d, c->any);
	if (raw != NULL)
	{
		c->q = sift_query_analyze(raw, c->analyzer);
		collect_leaves(c, c->q->root);
	}
	MemoryContextSwitchTo(old);
	return raw != NULL;
}

/* The document of a clause in the row, in the current memory context; NULL when the row has no text */
static SiftDoc *
clause_doc(const FbClause *c, ExprContext *econtext)
{
	bool		isnull;
	Datum		d = ExecEvalExprSwitchContext(c->doc, econtext, &isnull);
	text	   *t;

	if (isnull)
		return NULL;
	t = DatumGetTextPP(d);
	return sift_doc_open(c->analyzer, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
}

static void
count_row(FbState *fb, ExprContext *econtext)
{
	MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (int i = 0; i < fb->nclause; i++)
	{
		FbClause   *c = &fb->cl[i];
		SiftDoc    *d = clause_doc(c, econtext);

		if (d == NULL)
			continue;
		c->ndocs++;
		c->sum_dl += Min(sift_doc_ntokens(d), SIFT_MAX_DL);
		for (int j = 0; j < c->nleaf; j++)
			c->leaf[j].df += leaf_tf(&c->leaf[j], d) > 0;
	}
	MemoryContextSwitchTo(old);
}

static void
weigh_clause(FbClause *c)
{
	sift_kern_bm25_init(&c->bm, (float4) SIFT_DEFAULT_K1, (float4) SIFT_DEFAULT_B,
						c->sum_dl > 0 ? (float4) ((double) c->sum_dl / c->ndocs) : 1.0f);
	for (int i = 0; i < c->nleaf; i++)
		c->leaf[i].w = sift_kern_term_weight(c->leaf[i].boost, sift_kern_idf(c->ndocs, c->leaf[i].df), c->bm.k1);
}

/* The first pass; with keep, the rows that pass the other quals are put aside for the second */
static void
load_rows(FbState *fb, bool keep)
{
	SiftScanState *ss = &fb->ss;
	EState	   *estate = ss->css.ss.ps.state;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;
	TupleTableSlot *saved = ss->heapslot;
	TableScanDesc scan = sift_table_beginscan(ss->heap, estate->es_snapshot, 0, NULL);

	ss->heapslot = fb->loadslot;
	while (table_scan_getnextslot(scan, ForwardScanDirection, fb->loadslot))
	{
		CHECK_FOR_INTERRUPTS();
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
		if (!ExecQual(fb->secure, econtext))
			continue;
		count_row(fb, econtext);
		if (keep && ExecQual(ss->quals, econtext))
			tuplestore_puttupleslot(fb->rows, econtext->ecxt_scantuple);
	}
	table_endscan(scan);
	ss->heapslot = saved;
	ExecClearTuple(fb->loadslot);
	for (int i = 0; i < fb->nclause; i++)
		weigh_clause(&fb->cl[i]);
}

/* The score of the row in the scan tuple slot of the econtext */
static float4
row_score(FbState *fb, ExprContext *econtext)
{
	MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	float4		score = 0;

	for (int i = 0; i < fb->nclause; i++)
	{
		const FbClause *c = &fb->cl[i];
		SiftDoc    *d = clause_doc(c, econtext);
		float4		kn;

		if (d == NULL)
			continue;
		kn = sift_kern_knorm1(&c->bm, Min(sift_doc_ntokens(d), SIFT_MAX_DL));
		for (int j = 0; j < c->nleaf; j++)
		{
			const FbLeaf *l = &c->leaf[j];
			int			tf;

			if (l->gate && !sift_doc_match(d, l->gate))
				continue;
			tf = leaf_tf(l, d);
			if (tf > 0)
				score += sift_kern_bm25(l->w, Min(tf, SIFT_MAX_TF), kn);
		}
	}
	MemoryContextSwitchTo(old);
	return score;
}

static void
stamp_scores(const FbState *fb, TupleTableSlot *slot, float4 score)
{
	for (int i = 0; i < fb->ss.ncols; i++)
		if (fb->ss.col[i].kind == COL_SCORE)
			slot->tts_values[i] = Float4GetDatum(score);
		else if (fb->ss.col[i].kind == COL_MAX_SCORE)
			slot->tts_values[i] = Float4GetDatum(fb->ss.max_score);
}

/* The kept row in the scan tuple slot, scores not yet set; false at the end */
static bool
next_kept(FbState *fb)
{
	TupleTableSlot *slot = fb->ss.css.ss.ss_ScanTupleSlot;

	if (!tuplestore_gettupleslot(fb->rows, true, false, fb->rowslot))
		return false;
	ExecCopySlot(slot, fb->rowslot);
	return true;
}

/* The best score of the matches, which every row reads as its max_score */
static void
find_max(FbState *fb)
{
	ExprContext *econtext = fb->ss.css.ss.ps.ps_ExprContext;

	while (next_kept(fb))
	{
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = fb->ss.css.ss.ss_ScanTupleSlot;
		fb->ss.max_score = Max(fb->ss.max_score, row_score(fb, econtext));
	}
	tuplestore_rescan(fb->rows);
}

/* Binds the queries and reads the heap: the rows are kept for a scan, only counted for an EvalPlanQual recheck without max_score */
static void
prepare(FbState *fb, bool keep)
{
	SiftScanState *ss = &fb->ss;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;
	MemoryContext old;

	ss->started = true;
	ss->empty = false;
	for (int i = 0; i < fb->nclause && !ss->empty; i++)
		ss->empty = !bind_clause(fb, &fb->cl[i], econtext);
	if (ss->empty)
		return;
	old = MemoryContextSwitchTo(fb->cxt);
	if (keep)
		fb->rows = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(old);
	load_rows(fb, keep);
	if (keep && fb->need_max)
		find_max(fb);
}

static TupleTableSlot *
fb_next(ScanState *node)
{
	FbState    *fb = (FbState *) node;
	SiftScanState *ss = &fb->ss;
	ExprContext *econtext = node->ps.ps_ExprContext;

	if (!ss->started)
		prepare(fb, true);
	while (!ss->empty && next_kept(fb))
	{
		TupleTableSlot *slot = node->ss_ScanTupleSlot;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		stamp_scores(fb, slot, row_score(fb, econtext));
		if (ExecQual(ss->cuts, econtext))
			return slot;
	}
	return NULL;
}

static bool
fb_epq_score(SiftScanState *ss, float4 *score)
{
	FbState    *fb = (FbState *) ss;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	if (!ss->started)
		prepare(fb, fb->need_max);
	if (ss->empty)
		return false;
	econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
	*score = row_score(fb, econtext);
	return true;
}

static void
release(FbState *fb)
{
	if (fb->rows)
		tuplestore_end(fb->rows);
	fb->rows = NULL;
	MemoryContextReset(fb->cxt);
	for (int i = 0; i < fb->nclause; i++)
		fb->cl[i] = (FbClause) {.doc = fb->cl[i].doc, .query = fb->cl[i].query, .any = fb->cl[i].any, .index = fb->cl[i].index};
	fb->ss.started = false;
	fb->ss.max_score = 0;
}

static void
fb_begin(CustomScanState *node, EState *estate, int eflags)
{
	FbState    *fb = (FbState *) node;
	SiftScanState *ss = &fb->ss;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *docs = ExecInitExprList(list_nth_node(List, cs->custom_exprs, 0), &node->ss.ps),
			   *queries = ExecInitExprList(list_nth_node(List, cs->custom_exprs, 1), &node->ss.ps),
			   *secure = list_nth_node(List, cs->custom_exprs, 2);

	ss->heap = node->ss.ss_currentRelation;
	ss->heapslot = table_slot_create(ss->heap, &estate->es_tupleTable);
	fb->loadslot = table_slot_create(ss->heap, &estate->es_tupleTable);
	fb->rowslot = MakeSingleTupleTableSlot(node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
	fb->secure = ExecInitQual(secure, &node->ss.ps);
	ss->quals = ExecInitQual(list_concat_copy(secure, list_nth_node(List, cs->custom_exprs, 3)), &node->ss.ps);
	ss->cuts = ExecInitQual(list_nth_node(List, cs->custom_exprs, 4), &node->ss.ps);
	ss->epq_score = fb_epq_score;
	ss->cxt = fb->cxt = sift_mem_allocset_create(estate->es_query_cxt, "sift score scan");
	fb->nclause = list_length(docs);
	fb->cl = palloc0(Max(fb->nclause, 1) * sizeof(FbClause));
	for (int i = 0; i < fb->nclause; i++)
		fb->cl[i] = (FbClause) {.doc = list_nth(docs, i), .query = list_nth(queries, i),
								.any = list_nth_int(list_nth_node(List, cs->custom_private, 0), i),
								.index = list_nth_oid(list_nth_node(List, cs->custom_private, 1), i)};
	init_cols(ss, cs);
	for (int i = 0; i < ss->ncols; i++)
		fb->need_max |= ss->col[i].kind == COL_MAX_SCORE;
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		PredicateLockRelation(ss->heap, estate->es_snapshot);
}

static TupleTableSlot *
fb_exec(CustomScanState *node)
{
	SiftScanState *ss = (SiftScanState *) node;

	return epq_substituted(ss) ? epq_next(ss) : ExecScan(&node->ss, fb_next, scan_recheck);
}

static void
fb_end(CustomScanState *node)
{
	release((FbState *) node);
	ExecDropSingleTupleTableSlot(((FbState *) node)->rowslot);
}

static void
fb_rescan(CustomScanState *node)
{
	release((FbState *) node);
	ExecScanReScan(&node->ss);
}

static const CustomExecMethods fb_exec_methods = {
	.CustomName = "SiftScoreScan",
	.BeginCustomScan = fb_begin,
	.ExecCustomScan = fb_exec,
	.EndCustomScan = fb_end,
	.ReScanCustomScan = fb_rescan,
};

static Node *
fb_create_state(CustomScan *cscan)
{
	FbState    *fb = (FbState *) newNode(sizeof(FbState), T_CustomScanState);

	fb->ss.css.methods = &fb_exec_methods;
	fb->ss.css.flags = cscan->flags;
	return (Node *) fb;
}

/* `doc @@> query`, in the forms the matches take; the analyzer is that of sift.match_idx's index, else the default */
static bool
match_operands(Node *clause, Node **doc, Node **query, bool *any, Oid *index)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *args;

	*any = false;
	*index = InvalidOid;
	if (IsA(clause, OpExpr) && ((OpExpr *) clause)->opno == cat->match_op)
		args = ((OpExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_func)
		args = ((FuncExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_idx_func)
	{
		args = ((FuncExpr *) clause)->args;
		if (!IsA(lthird(args), Const) || ((Const *) lthird(args))->constisnull)
			return false;
		*index = DatumGetObjectId(((Const *) lthird(args))->constvalue);
	}
	else if (IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->useOr &&
			 ((ScalarArrayOpExpr *) clause)->opno == cat->match_op)
	{
		args = ((ScalarArrayOpExpr *) clause)->args;
		*any = true;
	}
	else
		return false;
	*doc = linitial(args);
	*query = lsecond(args);
	return !contain_var_clause(*query) && !contain_volatile_functions(*query) && !contain_volatile_functions(*doc);
}

/* The Vars of the rel the scan hands up or reads, then the scores the query and the cutoffs read */
static List *
fb_scan_tlist(PlannerInfo *root, RelOptInfo *rel, List *quals, List *cuts)
{
	List	   *vars = pull_var_clause((Node *) list_concat(list_concat_copy(rel->reltarget->exprs, quals), list_copy(cuts)),
									   PVC_RECURSE_PLACEHOLDERS),
			   *exprs = NIL,
			   *tlist = NIL;
	ScoreRefs	refs = {rel->relid, score_exprs(root, rel)};
	int			resno = 1;

	foreach_node(Var, v, vars)
		if (v->varno == rel->relid)
			exprs = list_append_unique(exprs, v);
	score_walker((Node *) cuts, &refs);
	exprs = list_concat(exprs, refs.found);
	foreach_ptr(Node, e, exprs)
		tlist = lappend(tlist, makeTargetEntry((Expr *) e, resno++, NULL, false));
	return tlist;
}

/* The scan outputs the scores the query reads: only a scan can compute them */
static List *
fb_score_outputs(PlannerInfo *root, RelOptInfo *rel, List *tlist)
{
	foreach_ptr(Node, e, score_exprs(root, rel))
		if (tlist_member((Expr *) e, tlist) == NULL)
			tlist = lappend(list_copy(tlist), makeTargetEntry((Expr *) e, list_length(tlist) + 1, NULL, true));
	return tlist;
}

static Plan *
fallback_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *secure = NIL,
			   *plain = NIL,
			   *cuts = NIL,
			   *docs = NIL,
			   *queries = NIL,
			   *any = NIL,
			   *indexes = NIL;

	foreach_node(RestrictInfo, rinfo, clauses)
	{
		Node	   *clause = (Node *) rinfo->clause,
				   *doc,
				   *query;
		bool		is_any;
		Oid			index;

		if (rinfo->pseudoconstant)
			continue;
		if (reads_score_of(clause, rel->relid))
			cuts = lappend(cuts, clause);
		else if (rinfo->security_level < root->qual_security_level)
			secure = lappend(secure, clause);
		else
		{
			plain = lappend(plain, clause);
			if (match_operands(clause, &doc, &query, &is_any, &index))
			{
				docs = lappend(docs, copyObject(doc));
				queries = lappend(queries, copyObject(query));
				any = lappend_int(any, is_any);
				indexes = lappend_oid(indexes, index);
			}
		}
	}
	cs->scan.plan.targetlist = fb_score_outputs(root, rel, tlist);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_scan_tlist = fb_scan_tlist(root, rel, list_concat_copy(secure, plain), cuts);
	cs->custom_private = list_make2(any, indexes);
	cs->custom_exprs = list_make5(docs, queries, secure, plain, cuts);
	cs->methods = &sift_fallback_plan_methods;
	return &cs->scan.plan;
}

const CustomPathMethods sift_fallback_path_methods = {"SiftScoreScan", fallback_plan, NULL};
const CustomScanMethods sift_fallback_plan_methods = {"SiftScoreScan", fb_create_state};

/* Two reads of the heap, and the work of the analyzer on every row of the first */
void
sift_fallback_publish(PlannerInfo *root, RelOptInfo *rel)
{
	CustomPath *cp = makeNode(CustomPath);
	Path		seq = {.parent = rel, .pathtarget = rel->reltarget};

	cost_seqscan(&seq, root, rel, NULL);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.rows = rel->rows;
	cp->path.startup_cost = 2 * seq.total_cost + rel->tuples * cpu_operator_cost * 100;
	cp->path.total_cost = cp->path.startup_cost + rel->rows * cpu_tuple_cost;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->methods = &sift_fallback_path_methods;
	rel->pathlist = list_make1(cp);
	rel->partial_pathlist = NIL;
}
