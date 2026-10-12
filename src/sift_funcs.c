/*
 * sift_query_sql.c - SQL entry points of the query module: sift.parse_query
 * and the seq-scan / recheck evaluation behind sift.match and
 * sift.match_idx (DESIGN.md (f) 5, (h)).
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift.h"
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
