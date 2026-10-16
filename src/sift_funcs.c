/*
 * sift_funcs.c - SQL entry points for matching: sift.parse_query, the
 * seq-scan / recheck evaluation behind sift.match and sift.match_idx, and
 * the @@> selectivity estimator (DESIGN.md (f) 5, 6, (h)).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_index.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "sift.h"
#include "sift_eval.h"
#include "sift_planner.h"
#include "sift_query.h"
#include "sift_sql.h"

/* Analyzer config of a sift index, or the default one for InvalidOid */
static void
analyzer_config(Oid indexoid, SiftAnalyzerConfig *cfg)
{
	Relation	index;

	if (!OidIsValid(indexoid))
	{
		sift_analyzer_config_default(cfg);
		return;
	}
	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != sift_catalog()->am)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	sift_analyzer_config_from_index(index, cfg);
	index_close(index, AccessShareLock);
}

static SiftQuery *
query_from_text(text *q, SiftAnalyzer *a)
{
	return sift_query_analyze(sift_query_parse(VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q)), a);
}

/* sift.parse_query(query text, index regclass DEFAULT NULL): canonical form */
Datum
sift_parse_query(PG_FUNCTION_ARGS)
{
	SiftAnalyzerConfig cfg;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	analyzer_config(PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1), &cfg);
	PG_RETURN_TEXT_P(cstring_to_text(sift_query_deparse(
		query_from_text(PG_GETARG_TEXT_PP(0), sift_analyzer_create(&cfg, CurrentMemoryContext)))));
}

/* Analyzed query cached across calls while the query and index stay the same */
typedef struct MatchCache
{
	MemoryContext mcxt;			/* analyzer and query; reset on change */
	Oid			index;
	text	   *qtext;
	SiftAnalyzer *analyzer;
	SiftQuery  *query;
} MatchCache;

static MatchCache *
match_cache(FunctionCallInfo fcinfo, text *q, Oid index)
{
	MatchCache *c = fcinfo->flinfo->fn_extra;
	MemoryContext old;
	SiftAnalyzerConfig cfg;

	if (c == NULL)
	{
		c = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(MatchCache));
		c->mcxt = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt, "sift match", ALLOCSET_SMALL_SIZES);
		fcinfo->flinfo->fn_extra = c;
	}
	else if (c->query && c->index == index && VARSIZE_ANY_EXHDR(q) == VARSIZE_ANY_EXHDR(c->qtext) &&
			 memcmp(VARDATA_ANY(q), VARDATA_ANY(c->qtext), VARSIZE_ANY_EXHDR(q)) == 0)
		return c;

	c->query = NULL;
	MemoryContextReset(c->mcxt);
	old = MemoryContextSwitchTo(c->mcxt);
	analyzer_config(index, &cfg);
	c->analyzer = sift_analyzer_create(&cfg, c->mcxt);
	c->qtext = (text *) datumCopy(PointerGetDatum(q), false, -1);
	c->index = index;
	c->query = query_from_text(q, c->analyzer);
	MemoryContextSwitchTo(old);
	return c;
}

static bool
match(FunctionCallInfo fcinfo, Oid index)
{
	text	   *doc = PG_GETARG_TEXT_PP(0);
	MatchCache *c = match_cache(fcinfo, PG_GETARG_TEXT_PP(1), index);

	return sift_query_match_doc(c->query, c->analyzer, VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc));
}

/* sift.match(doc, query), the @@> operator: default analyzer */
Datum
sift_match(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(match(fcinfo, InvalidOid));
}

/* sift.match_idx(doc, query, index): the analyzer of that index */
Datum
sift_match_idx(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(match(fcinfo, PG_GETARG_OID(2)));
}

static Oid	bind_analyzer(List *indexes, bool *contested);

/* Matching documents over the heap rows, by the first full index on expr that answers as `@@>` does there */
static double
match_selectivity(PlannerInfo *root, RelOptInfo *rel, Node *expr, text *query)
{
	List	   *keyed = NIL;
	Oid			analyzer;

	if (rel->rtekind != RTE_RELATION)
		return DEFAULT_MATCH_SEL;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->indpred == NIL && !ii->hypothetical && sift_index_matches_key(ii, expr))
			keyed = lappend_oid(keyed, ii->indexoid);
	list_sort(keyed, list_oid_cmp);
	analyzer = bind_analyzer(keyed, NULL);
	foreach_oid(indexoid, keyed)
	{
		SiftEstimate est;
		Relation	index;

		if (!sift_index_serves(indexoid, analyzer))
			continue;
		index = index_open(indexoid, AccessShareLock);
		sift_eval_estimate(index, &query, 1, &est);
		index_close(index, AccessShareLock);
		return est.docs / Max(rel->tuples > 0 ? rel->tuples : est.ndocs, 1);
	}
	return DEFAULT_MATCH_SEL;
}

/* RESTRICT of @@>: the term statistics of the index on the left operand */
Datum
sift_matchsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	double		sel = DEFAULT_MATCH_SEL;

	if (get_restriction_variable(root, args, varRelid, &vardata, &other, &varonleft))
	{
		if (varonleft && vardata.rel && IsA(other, Const) && !((Const *) other)->constisnull)
			sel = match_selectivity(root, vardata.rel, vardata.var, DatumGetTextPP(((Const *) other)->constvalue));
		ReleaseVariableStats(vardata);
	}
	PG_RETURN_FLOAT8(Min(Max(sel, 0.0), 1.0));
}

/* ---------- planner support: sift.match / sift.match_idx ---------- */

static Node *
strip_relabel(Node *expr)
{
	while (IsA(expr, RelabelType))
		expr = (Node *) ((RelabelType *) expr)->arg;
	return expr;
}

/* Is expr (a column or expression of range table entry relid) the key of the sift index? */
static bool
index_key_is(Relation index, Index relid, Node *expr)
{
	Node	   *key;

	expr = strip_relabel(expr);
	if (index->rd_index->indkey.values[0] != 0)
		return IsA(expr, Var) && ((Var *) expr)->varno == relid && ((Var *) expr)->varlevelsup == 0 &&
			((Var *) expr)->varattno == index->rd_index->indkey.values[0];
	key = linitial(RelationGetIndexExpressions(index));
	ChangeVarNodes(key, 1, relid, 0);
	return equal(strip_relabel(key), expr);
}

/*
 * Does the index answer queries as the analyzer of index `analyzer` does (InvalidOid: the default
 * analyzer)? Indexes on one key with analyzers that answer differently, such as one with fields and
 * one without, must not serve each other's clauses: the result would depend on the plan.
 */
bool
sift_index_serves(Oid indexoid, Oid analyzer)
{
	SiftAnalyzerConfig have,
				want;

	if (indexoid == analyzer)
		return true;
	analyzer_config(indexoid, &have);
	analyzer_config(analyzer, &want);
	return sift_analyzer_config_agrees(&have, &want);
}

/*
 * What `key @@> q` means on a key with these sift indexes: the default analyzer unless none of them
 * agrees with it, else the first one's (the lowest OID). *contested is set when an index answers
 * differently from that analyzer: it must not serve the plain operator.
 */
static Oid
bind_analyzer(List *indexes, bool *contested)
{
	Oid			analyzer = InvalidOid;
	bool		dflt = false;

	foreach_oid(indexoid, indexes)
		dflt |= sift_index_serves(indexoid, InvalidOid);
	if (!dflt && indexes != NIL)
		analyzer = linitial_oid(indexes);
	if (contested)
		foreach_oid(indexoid, indexes)
			*contested |= !sift_index_serves(indexoid, analyzer);
	return analyzer;
}

/* The valid, full, single column sift indexes on expr, a column or expression of range table entry relid */
static List *
keyed_indexes(PlannerInfo *root, Node *expr)
{
	int			relid;
	Relation	heap;
	List	   *found = NIL;

	if (!bms_get_singleton_member(pull_varnos(root, expr), &relid) || rt_fetch(relid, root->parse->rtable)->rtekind != RTE_RELATION)
		return NIL;
	heap = relation_open(rt_fetch(relid, root->parse->rtable)->relid, AccessShareLock);
	foreach_oid(indexoid, RelationGetIndexList(heap))
	{
		Relation	index = index_open(indexoid, AccessShareLock);

		if (index->rd_rel->relam == sift_catalog()->am && index->rd_index->indisvalid && index->rd_index->indnatts == 1 &&
			heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL) && index_key_is(index, relid, expr))
			found = lappend_oid(found, indexoid);
		index_close(index, AccessShareLock);
	}
	relation_close(heap, AccessShareLock);
	return found;
}

/*
 * match(doc, q) on a key whose indexes do not all have the default analyzer is bound to the
 * analyzer `@@>` means there (match_idx; InvalidOid for the default one), which decides the
 * seq-scan and recheck evaluation and which indexes may serve the clause.
 */
static Node *
simplify_match(SupportRequestSimplify *req)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *indexes;
	bool		contested = false;
	Oid			analyzer;

	if (req->root == NULL || req->fcall->funcid != cat->match_func ||
		(indexes = keyed_indexes(req->root, linitial(req->fcall->args))) == NIL)
		return NULL;
	analyzer = bind_analyzer(indexes, &contested);
	if (!OidIsValid(analyzer) && !contested)
		return NULL;
	return (Node *) makeFuncExpr(cat->match_idx_func, BOOLOID,
								 lappend(list_copy(req->fcall->args),
										 makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(analyzer), false, true)),
								 InvalidOid, req->fcall->inputcollid, COERCE_EXPLICIT_CALL);
}

/* The function call is `key @@> q` for this index when it agrees on the analyzer */
static bool
index_serves(SupportRequestIndexCondition *req)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *args = ((FuncExpr *) req->node)->args;

	if (req->index->relam != cat->am || req->index->ncolumns != 1 || req->indexarg != 0)
		return false;
	if (req->funcid == cat->match_idx_func)
		return IsA(lthird(args), Const) && sift_index_serves(req->index->indexoid, DatumGetObjectId(((Const *) lthird(args))->constvalue));
	return req->funcid == cat->match_func && sift_index_serves(req->index->indexoid, InvalidOid);
}

static Node *
index_condition(SupportRequestIndexCondition *req)
{
	List	   *args = ((FuncExpr *) req->node)->args;

	if (!index_serves(req))
		return NULL;
	req->lossy = false;
	return (Node *) list_make1(make_opclause(sift_catalog()->match_op, BOOLOID, false, linitial(args),
											 lsecond(args), InvalidOid, req->indexcollation));
}

Datum
sift_match_support(PG_FUNCTION_ARGS)
{
	Node	   *req = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(req, SupportRequestSimplify))
		ret = simplify_match((SupportRequestSimplify *) req);
	else if (IsA(req, SupportRequestIndexCondition))
		ret = index_condition((SupportRequestIndexCondition *) req);
	PG_RETURN_POINTER(ret);
}

/* ---------- sift.score / sift.max_score ---------- */

static Datum
scan_only(const char *name)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift.%s() must be computed by a sift scan of the same relation", name),
			 errhint("Call it on the ctid of a table with a sift index, in the query level that scans the table, with a @@> condition on the indexed column.")));
	return (Datum) 0;
}

Datum
sift_score(PG_FUNCTION_ARGS)
{
	return scan_only("score");
}

Datum
sift_max_score(PG_FUNCTION_ARGS)
{
	return scan_only("max_score");
}

/* ---------- sift.highlight ---------- */

/* Does the token match a positive leaf of n? */
static bool
token_hit(const SiftQNode *n, const SiftToken *t)
{
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				if (token_hit(n->child[i], t))
					return true;
			return false;
		case SQ_PHRASE:
			for (int i = 0; i < n->nterms; i++)
				if (n->termlens[i] == t->len && memcmp(n->terms[i], t->str, t->len) == 0)
					return true;
			return false;
		case SQ_NOT:
		case SQ_NOTHING:
			return false;
		default:
			return sift_query_leaf_match(n, t->str, t->len);
	}
}

static const char *
tag_arg(FunctionCallInfo fcinfo, int argno, const char *dflt)
{
	return PG_ARGISNULL(argno) ? dflt : text_to_cstring(PG_GETARG_TEXT_PP(argno));
}

/* sift.highlight(doc, query, start_tag, stop_tag, index): wraps the tokens that satisfy the query */
Datum
sift_highlight(PG_FUNCTION_ARGS)
{
	SiftAnalyzerConfig cfg;
	SiftAnalyzer *a;
	SiftQuery  *q;
	SiftToken  *toks;
	text	   *doc;
	const char *s,
			   *start_tag = tag_arg(fcinfo, 2, "<b>"),
			   *stop_tag = tag_arg(fcinfo, 3, "</b>");
	StringInfoData buf;
	int			ntok,
				done = 0;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	analyzer_config(PG_ARGISNULL(4) ? InvalidOid : PG_GETARG_OID(4), &cfg);
	a = sift_analyzer_create(&cfg, CurrentMemoryContext);
	q = query_from_text(PG_GETARG_TEXT_PP(1), a);
	doc = PG_GETARG_TEXT_PP(0);
	s = VARDATA_ANY(doc);
	ntok = sift_analyze_tokens(a, s, VARSIZE_ANY_EXHDR(doc), &toks);
	initStringInfo(&buf);
	for (int i = 0; i < ntok; i++)
	{
		if (!token_hit(q->root, &toks[i]))
			continue;
		appendBinaryStringInfo(&buf, s + done, toks[i].start - done);
		appendStringInfo(&buf, "%s", start_tag);
		appendBinaryStringInfo(&buf, s + toks[i].start, toks[i].end - toks[i].start);
		appendStringInfo(&buf, "%s", stop_tag);
		done = toks[i].end;
	}
	appendBinaryStringInfo(&buf, s + done, VARSIZE_ANY_EXHDR(doc) - done);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}
