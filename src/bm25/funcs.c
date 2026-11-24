/*
 * bm25/funcs.c - SQL entry points for matching: sift.parse_query, the
 * seq-scan / recheck evaluation behind sift.match and sift.match_idx, and
 * the @@> selectivity estimator.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_index.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "core/sift.h"
#include "bm25/analyze.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "core/ops.h"
#include "bm25/planner.h"
#include "bm25/query.h"
#include "core/quarantine.h"
#include "core/sql.h"

#ifdef SIFT_HAVE_LZ4
#include <lz4.h>
#endif
#ifdef SIFT_HAVE_ZSTD
#include <zstd.h>
#endif

/* Analyzer config of a sift index, or the default one for InvalidOid */
static void
analyzer_config(Oid indexoid, SiftAnalyzerConfig *cfg)
{
	if (!OidIsValid(indexoid))
	{
		sift_analyzer_config_default(cfg);
		return;
	}
	sift_analyzer_config_lookup_index(indexoid, cfg);
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

/* "title^3, body, tags^0.5": the fields and weights of sift.field_query, in the order written; a field without ^ weighs 1 */
static int
parse_weights(const char *spec, char ***names, double **weights)
{
	List	   *parts = NIL;
	char	   *copy = pstrdup(spec);
	StringInfoData joined;
	int			n = 0;

	initStringInfo(&joined);
	for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ","))
		parts = lappend(parts, tok);
	*names = palloc(Max(list_length(parts), 1) * sizeof(char *));
	*weights = palloc(Max(list_length(parts), 1) * sizeof(double));
	foreach_ptr(char, tok, parts)
	{
		char	   *caret = strchr(tok, '^');
		char	   *end;
		double		w = 1.0;

		if (caret)
		{
			*caret = '\0';
			w = strtod(caret + 1, &end);
			while (*end == ' ' || *end == '\t')
				end++;
			if (end == caret + 1 || *end || !(w > 0) || w > SIFT_MAX_BOOST)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid weight for field \"%s\"", tok),
						 errhint("A weight is a number above 0 and up to %g: \"title^3, body\".", SIFT_MAX_BOOST)));
		}
		while (*tok == ' ' || *tok == '\t')
			tok++;
		for (end = tok + strlen(tok); end > tok && (end[-1] == ' ' || end[-1] == '\t'); end--)
			;
		*end = '\0';
		(*names)[n] = tok;
		(*weights)[n++] = w;
		appendStringInfo(&joined, "%s%s", joined.len ? "," : "", tok);
	}
	if (n == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("no field to weigh"),
						errhint("Write the fields with their weights: \"title^3, body\".")));
	sift_analyze_validate_fields(joined.data);
	return n;
}

/*
 * sift.field_query(query text, fields text, index regclass DEFAULT NULL): the query with per-field weights (docs/FIELDS-FACETS.md): every
 * word, prefix, pattern and fuzzy leaf becomes the OR of itself in each field, boosted by the field's weight, so that a document scores the
 * sum of its fields' scores. With an index the fields are checked against its own.
 */
Datum
sift_field_query(PG_FUNCTION_ARGS)
{
	char	  **names;
	double	   *weights;
	int			n;
	SiftQuery  *q;
	char	   *out;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	if (PG_ARGISNULL(1))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("the fields to weigh are required")));
	n = parse_weights(text_to_cstring(PG_GETARG_TEXT_PP(1)), &names, &weights);
	q = sift_query_weigh(sift_query_parse(VARDATA_ANY(PG_GETARG_TEXT_PP(0)), VARSIZE_ANY_EXHDR(PG_GETARG_TEXT_PP(0))),
						 n, (const char *const *) names, weights);
	out = sift_query_deparse(q);
	if (!PG_ARGISNULL(2))
	{
		SiftAnalyzerConfig cfg;
		SiftAnalyzer *a;

		analyzer_config(PG_GETARG_OID(2), &cfg);
		if (cfg.nfields == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("index \"%s\" has no fields", get_rel_name(PG_GETARG_OID(2))),
					 errhint("Create it with WITH (fields = 'title,body') on a key made by sift.sections(title, body).")));
		a = sift_analyzer_create(&cfg, CurrentMemoryContext);
		(void) query_from_text(cstring_to_text(out), a);
	}
	PG_RETURN_TEXT_P(cstring_to_text(out));
}

/* Analyzed query cached across calls while the query and index stay the same */
typedef struct MatchCache
{
	MemoryContext mcxt;			/* analyzer and query; reset on change */
	Oid			index;
	text	   *qtext;
	SiftAnalyzer *analyzer;
	SiftQuery  *query;
	struct HighlightIndex *hl;	/* the positive leaves of the query by kind, made by the first sift.highlight */
	FmgrInfo	outfn;			/* sift.match_any: the output function of the type of its first argument */
	bool		outfn_set;
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
	c->hl = NULL;
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

PG_FUNCTION_INFO_V1(sift_match_any);

/* sift.match_any(doc anyelement, query), the @@> operator on a column of any type: the text its output function writes, the default analyzer */
Datum
sift_match_any(PG_FUNCTION_ARGS)
{
	MatchCache *c = match_cache(fcinfo, PG_GETARG_TEXT_PP(1), InvalidOid);
	char	   *doc;
	bool		hit;

	if (!c->outfn_set)
	{
		Oid			outfunc;
		bool		isvarlena;

		getTypeOutputInfo(get_fn_expr_argtype(fcinfo->flinfo, 0), &outfunc, &isvarlena);
		fmgr_info_cxt(outfunc, &c->outfn, c->mcxt);
		c->outfn_set = true;
	}
	doc = OutputFunctionCall(&c->outfn, PG_GETARG_DATUM(0));
	hit = sift_query_match_doc(c->query, c->analyzer, doc, strlen(doc));
	pfree(doc);
	PG_RETURN_BOOL(hit);
}

PG_FUNCTION_INFO_V1(sift_key_equal);

/* sift.===, the equality of the key columns of a UNIQUE index: strategy 3 of its operator classes, which the pre-check of INSERT ... ON CONFLICT is built with; no query uses it */
Datum
sift_key_equal(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	TypeCacheEntry *tc;

	if (!OidIsValid(type))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.=== is the equality of a UNIQUE sift key, for INSERT ... ON CONFLICT: it is not a query operator"),
				 errhint("Compare the column with =, or use INSERT ... ON CONFLICT on the UNIQUE index.")));
	tc = lookup_type_cache(type, TYPECACHE_EQ_OPR_FINFO);
	if (!OidIsValid(tc->eq_opr))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for type %s", format_type_be(type))));
	PG_RETURN_BOOL(DatumGetBool(FunctionCall2Coll(&tc->eq_opr_finfo, PG_GET_COLLATION(), PG_GETARG_DATUM(0), PG_GETARG_DATUM(1))));
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

/*
 * Matching documents over the heap rows, by the first full index on expr that answers as the analyzer
 * does: the one `@@>` means on that key, or the one sift.match_idx names (named)
 */
static double
match_selectivity(PlannerInfo *root, RelOptInfo *rel, Node *expr, text *query, const Const *named)
{
	List	   *keyed = NIL;
	Oid			analyzer;

	/* the match is not leakproof: the index's document frequencies include the rows a policy or security barrier hides */
	if (rel->rtekind != RTE_RELATION || planner_rt_fetch(rel->relid, root)->securityQuals != NIL || rel_secured(root, rel))
		return DEFAULT_MATCH_SEL;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->indpred == NIL && !ii->hypothetical && sift_index_matches_key(ii, expr))
			keyed = lappend_oid(keyed, ii->indexoid);
	list_sort(keyed, list_oid_cmp);
	analyzer = named ? DatumGetObjectId(named->constvalue) : bind_analyzer(keyed, NULL);
	foreach_oid(indexoid, keyed)
	{
		SiftEstimate est;
		Relation	index;

		if (!sift_index_serves(indexoid, analyzer))
			continue;
		index = index_open(indexoid, AccessShareLock);
		sift_estimate_cached(index, &query, 1, &est);
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
			sel = match_selectivity(root, vardata.rel, vardata.var, DatumGetTextPP(((Const *) other)->constvalue), NULL);
		ReleaseVariableStats(vardata);
	}
	PG_RETURN_FLOAT8(Min(Max(sel, 0.0), 1.0));
}

/*
 * The same for the call sift.match_idx(key, query, index) that `@@>` becomes on a key whose indexes
 * do not all have the default analyzer: a function call has no RESTRICT, so without this it would
 * keep the planner's default for any boolean function (a third of the rows) whatever the query
 */
static Node *
call_selectivity(SupportRequestSelectivity *req)
{
	VariableStatData vardata;
	Node	   *query = lsecond(req->args);

	if (req->is_join || req->funcid != sift_catalog()->match_idx_func || !IsA(query, Const) ||
		((Const *) query)->constisnull || !IsA(lthird(req->args), Const))
		return NULL;
	examine_variable(req->root, linitial(req->args), req->varRelid, &vardata);
	if (vardata.rel)
		req->selectivity = Min(Max(match_selectivity(req->root, vardata.rel, vardata.var, DatumGetTextPP(((Const *) query)->constvalue),
													 lthird_node(Const, req->args)), 0.0), 1.0);
	ReleaseVariableStats(vardata);
	return vardata.rel ? (Node *) req : NULL;
}

/* Is expr (a column or expression of range table entry relid) the text of the sift index? */
static bool
index_key_is(Relation index, Index relid, Node *expr)
{
	int			nkey = index->rd_index->indnkeyatts,
				keys[INDEX_MAX_KEYS];
	List	   *exprs = RelationGetIndexExpressions(index);

	for (int k = 0; k < nkey; k++)
		keys[k] = index->rd_index->indkey.values[k];
	ChangeVarNodes((Node *) exprs, 1, relid, 0);
	return sift_key_is_whole(nkey, keys, exprs, relid, expr);
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

		if (index->rd_rel->relam == sift_catalog()->am && index->rd_index->indisvalid &&
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

	if (req->index->relam != cat->am || req->index->nkeycolumns != 1 || req->indexarg != 0)
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
	else if (IsA(req, SupportRequestSelectivity))
		ret = call_selectivity((SupportRequestSelectivity *) req);
	PG_RETURN_POINTER(ret);
}

/* sift.version(): the version of the loaded library, to compare with the installed extension's */
Datum
sift_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(SIFT_MODULE_VERSION));
}

/* sift.build_info(): what this build of the library was made with, to compare two servers: a codec a build lacks cannot open chunks another wrote with it */
Datum
sift_build_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const char *lz4 = "no";
	const char *zstd = "no";
	const char *rows[][2] = {{"version", SIFT_MODULE_VERSION}, {"postgres", PG_VERSION}, {"lz4", NULL}, {"zstd", NULL},
#ifdef USE_ASSERT_CHECKING
							 {"assertions", "on"}
#else
							 {"assertions", "off"}
#endif
	};

#ifdef SIFT_HAVE_LZ4
	lz4 = LZ4_versionString();
#endif
#ifdef SIFT_HAVE_ZSTD
	zstd = ZSTD_versionString();
#endif
	rows[2][1] = lz4;
	rows[3][1] = zstd;
	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < lengthof(rows); i++)
	{
		Datum		values[2] = {CStringGetTextDatum(rows[i][0]), CStringGetTextDatum(rows[i][1])};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* A score is computed by the scan of its relation; a ctid typed in (no relation) has none, and scores 0 */
static Datum
scan_only(FunctionCallInfo fcinfo, const char *name)
{
	Node	   *arg = linitial(((FuncExpr *) fcinfo->flinfo->fn_expr)->args);

	if (IsA(arg, Const))
		PG_RETURN_FLOAT4(0.0f);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift.%s() must be computed by a sift scan of the same relation", name),
			 errdetail("The score exists only inside the scan of the relation the ctid belongs to: it is not computed above a join, in a sublink, or for the ctid of a subquery."),
			 errhint("Call it on the ctid of a table in the FROM clause of the query level that scans the table.")));
	return (Datum) 0;
}

Datum
sift_score(PG_FUNCTION_ARGS)
{
	return scan_only(fcinfo, "score");
}

Datum
sift_max_score(PG_FUNCTION_ARGS)
{
	return scan_only(fcinfo, "max_score");
}

/*
 * The positive leaves of a query as a highlight sees them: the words (SQ_TERM, and the terms of a phrase) in a table, so a token is
 * looked up and not compared with every leaf, and the leaves that match many words (prefixes, patterns, fuzzy terms) in a list
 */
typedef struct HighlightIndex
{
	const SiftQNode **slots;	/* [mask + 1]: a node whose text, or the term of a phrase at tlen, is the word; NULL when free */
	int		   *tix;			/* the term of the phrase at the slot, -1 for a SQ_TERM */
	uint32		mask;
	const SiftQNode **wild;
	int			nwild;
	int			nexact;
} HighlightIndex;

static void
hl_count(const SiftQNode *n, int *nexact, int *nwild)
{
	check_stack_depth();
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				hl_count(n->child[i], nexact, nwild);
			return;
		case SQ_PHRASE:
			*nexact += n->nterms;
			return;
		case SQ_NOT:
		case SQ_NOTHING:
		case SQ_ALL:
			return;
		case SQ_TERM:
			(*nexact)++;
			return;
		default:
			(*nwild)++;
	}
}

static void
hl_word(const SiftQNode *n, int ti, const char **s, int *len)
{
	*s = ti < 0 ? n->text : n->terms[ti];
	*len = ti < 0 ? n->textlen : n->termlens[ti];
}

static void
hl_add(HighlightIndex *hl, const SiftQNode *n)
{
	check_stack_depth();
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				hl_add(hl, n->child[i]);
			return;
		case SQ_PHRASE:
		case SQ_TERM:
			for (int i = 0; i < (n->kind == SQ_TERM ? 1 : n->nterms); i++)
			{
				const char *s;
				int			len;
				uint32		h;

				hl_word(n, n->kind == SQ_TERM ? -1 : i, &s, &len);
				for (h = (uint32) sift_term_hash(s, len) & hl->mask; hl->slots[h] != NULL; h = (h + 1) & hl->mask)
				{
					const char *o;
					int			olen;

					hl_word(hl->slots[h], hl->tix[h], &o, &olen);
					if (olen == len && memcmp(o, s, len) == 0)
						break;
				}
				if (hl->slots[h] == NULL)
				{
					hl->slots[h] = n;
					hl->tix[h] = n->kind == SQ_TERM ? -1 : i;
				}
			}
			return;
		case SQ_NOT:
		case SQ_NOTHING:
		case SQ_ALL:
			return;
		default:
			hl->wild[hl->nwild++] = n;
	}
}

static HighlightIndex *
highlight_index(const SiftQNode *root, MemoryContext cx)
{
	HighlightIndex *hl = MemoryContextAllocZero(cx, sizeof(HighlightIndex));
	int			nexact = 0,
				nwild = 0;
	uint32		size = 8;

	hl_count(root, &nexact, &nwild);
	while (size < 2 * (uint32) nexact)
		size *= 2;
	hl->mask = size - 1;
	hl->nexact = nexact;
	hl->slots = MemoryContextAllocZero(cx, size * sizeof(SiftQNode *));
	hl->tix = MemoryContextAlloc(cx, size * sizeof(int));
	hl->wild = MemoryContextAlloc(cx, Max(nwild, 1) * sizeof(SiftQNode *));
	hl_add(hl, root);
	return hl;
}

/* Does the token match a positive leaf of the query? */
static bool
token_hit(const HighlightIndex *hl, const SiftToken *t)
{
	if (hl->nexact > 0)
		for (uint32 h = (uint32) sift_term_hash(t->str, t->len) & hl->mask; hl->slots[h] != NULL; h = (h + 1) & hl->mask)
		{
			const char *o;
			int			olen;

			hl_word(hl->slots[h], hl->tix[h], &o, &olen);
			if (olen == t->len && memcmp(o, t->str, t->len) == 0)
				return true;
		}
	for (int i = 0; i < hl->nwild; i++)
		if (sift_query_leaf_match(hl->wild[i], t->str, t->len))
			return true;
	return false;
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
	MatchCache *c;
	SiftAnalyzer *a;
	SiftToken  *toks;
	text	   *doc,
			   *res;
	const char *s,
			   *start_tag = tag_arg(fcinfo, 2, "<b>"),
			   *stop_tag = tag_arg(fcinfo, 3, "</b>");
	StringInfoData buf;
	int			ntok,
				done = 0;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	c = match_cache(fcinfo, PG_GETARG_TEXT_PP(1), PG_ARGISNULL(4) ? InvalidOid : PG_GETARG_OID(4));
	a = c->analyzer;
	if (c->hl == NULL)
		c->hl = highlight_index(c->query->root, c->mcxt);
	doc = PG_GETARG_TEXT_PP(0);
	s = VARDATA_ANY(doc);
	ntok = sift_analyze_tokens(a, s, VARSIZE_ANY_EXHDR(doc), &toks);
	initStringInfo(&buf);
	for (int i = 0; i < ntok; i++)
	{
		if (!token_hit(c->hl, &toks[i]))
			continue;
		appendBinaryStringInfo(&buf, s + done, toks[i].start - done);
		appendStringInfo(&buf, "%s", start_tag);
		appendBinaryStringInfo(&buf, s + toks[i].start, toks[i].end - toks[i].start);
		appendStringInfo(&buf, "%s", stop_tag);
		done = toks[i].end;
	}
	appendBinaryStringInfo(&buf, s + done, VARSIZE_ANY_EXHDR(doc) - done);
	sift_analyzer_reset(a);
	/* MEM[MEM-BM25-07]: sift_highlight StringInfo buf + tag cstrings (2x doc_len unfreed in caller cx) */
	res = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);
	if (!PG_ARGISNULL(2))
		pfree((char *) start_tag);
	if (!PG_ARGISNULL(3))
		pfree((char *) stop_tag);
	PG_FREE_IF_COPY(doc, 0);
	PG_RETURN_TEXT_P(res);
}
