/*
 * sift_analyze_sql.c - sift.tokenize(): the analyzer as seen from SQL.
 */
#include "postgres.h"

#include "access/genam.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "sift_analyze.h"
#include "sift_sql.h"

/* Required-index lookup; callers decide when to use the default analyzer. */
void
sift_analyzer_config_lookup_index(Oid indexoid, SiftAnalyzerConfig *cfg)
{
	Relation	index = index_open(indexoid, AccessShareLock);

	if (index->rd_rel->relam != sift_catalog()->am)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	sift_analyzer_config_from_index(index, cfg);
	index_close(index, AccessShareLock);
}

/* Config of a sift index, or the default when index is NULL */
static void
config_for(FunctionCallInfo fcinfo, SiftAnalyzerConfig *cfg)
{
	if (PG_ARGISNULL(1))
		return sift_analyzer_config_default(cfg);
	sift_analyzer_config_lookup_index(PG_GETARG_OID(1), cfg);
}

/*
 * sift.tokenize(doc, index, stemmer, case_folding, accent_folding): tokens
 * with positions and 0-based character offsets [start, end) into doc.
 * Non-NULL options override the index (or default) analyzer.
 */
Datum
sift_tokenize(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftAnalyzerConfig cfg;
	SiftAnalyzer *a;
	SiftToken  *toks;
	text	   *doc;
	const char *s;
	int			ntok,
				byte = 0,
				chr = 0;
	bool		keyword;

	InitMaterializedSRF(fcinfo, 0);
	if (PG_ARGISNULL(0))
		return (Datum) 0;
	config_for(fcinfo, &cfg);
	keyword = cfg.keyword;
	sift_analyzer_config_set(&cfg,
							 PG_ARGISNULL(3) ? cfg.case_fold : PG_GETARG_BOOL(3),
							 PG_ARGISNULL(4) ? cfg.accent_fold : PG_GETARG_BOOL(4),
							 PG_ARGISNULL(2) ? cfg.stemmer :
							 sift_analyze_stemmer(text_to_cstring(PG_GETARG_TEXT_PP(2))));
	sift_analyzer_config_set_keyword(&cfg, keyword);
	doc = PG_GETARG_TEXT_PP(0);
	s = VARDATA_ANY(doc);
	a = sift_analyzer_create(&cfg, CurrentMemoryContext);
	ntok = sift_analyze_tokens(a, s, VARSIZE_ANY_EXHDR(doc), &toks);
	for (int t = 0; t < ntok; t++)
	{
		Datum		values[4];
		bool		nulls[4] = {0};

		chr += pg_mbstrlen_with_len(s + byte, toks[t].start - byte);
		values[0] = PointerGetDatum(cstring_to_text_with_len(toks[t].str, toks[t].len));
		values[1] = Int32GetDatum(toks[t].pos);
		values[2] = Int32GetDatum(chr);
		chr += pg_mbstrlen_with_len(s + toks[t].start, toks[t].end - toks[t].start);
		values[3] = Int32GetDatum(chr);
		byte = toks[t].end;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}
