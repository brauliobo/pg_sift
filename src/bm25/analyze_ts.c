/*
 * bm25/analyze_ts.c - a PostgreSQL text search configuration as the stemmer of the analyzer (reloption stemmer = 'german').
 *
 * Our tokenizer and folding find the words; the dictionaries the configuration maps to word tokens (asciiword, word, numword of the
 * default parser) decide what is indexed, once per word as to_tsvector does (no stemming to a fixpoint): the first dictionary that knows the word answers; an empty answer is a stop word
 * (the token is dropped); a filtering dictionary (unaccent) hands its output to the next one; a word no dictionary knows is dropped, as
 * to_tsvector drops it. A dictionary that answers several lexemes (ispell variants, synonyms) contributes the first.
 */
#include "postgres.h"

#include "catalog/pg_ts_config.h"
#include "fmgr.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_public.h"
#include "nodes/miscnodes.h"
#include "utils/builtins.h"

#include "bm25/analyze.h"
#include "bm25/analyze_memo.h"

/* Token types of the default parser (wparser_def.c) */
#define TS_ASCIIWORD	1
#define TS_WORD			2
#define TS_NUMWORD		3

struct SiftTsStem
{
	Oid			cfg;
	StemMemo	memo;			/* of the words the dictionaries answered */
};

/*
 * A name without a schema is a pg_catalog configuration: CREATE INDEX runs with a restricted search_path and queries with the session's,
 * so no other schema may decide what the name means.
 */
static Oid
ts_config_oid(const char *name)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	Datum		oid;
	const char *qualified = strchr(name, '.') == NULL && name[0] != '"' ? psprintf("pg_catalog.%s", name) : name;

	if (!DirectInputFunctionCallSafe(regconfigin, (char *) qualified, InvalidOid, -1, (Node *) &escontext, &oid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown sift stemmer \"%s\"", name),
				 errhint("Valid stemmers are \"en\", \"none\" and the names of text search configurations (with their schema unless they are in pg_catalog).")));
	}
	return DatumGetObjectId(oid);
}

SiftTsStem *
sift_ts_open(const char *config, MemoryContext mcxt)
{
	SiftTsStem *ts = MemoryContextAlloc(mcxt, sizeof(SiftTsStem));

	*ts = (SiftTsStem) {.cfg = ts_config_oid(config), .memo = {.mcxt = mcxt}};
	return ts;
}

void
sift_analyze_validate_stemmer(const char *name)
{
	if (sift_analyze_stemmer(name) == SIFT_STEM_TS)
	{
		TSConfigCacheEntry *c = lookup_ts_config_cache(ts_config_oid(name));

		if (c->lenmap <= TS_NUMWORD)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text search configuration \"%s\" maps no word tokens", name)));
	}
}

#define LEX_OK		1
#define LEX_STOP	(-1)		/* a stop word */
#define LEX_UNKNOWN	0			/* no dictionary knows the word */

/*
 * The word through the dictionaries of its token type: LEX_OK with the lexeme in *out (palloc'd, *outlen bytes), or LEX_STOP, or LEX_UNKNOWN.
 */
static int
lexize(const TSConfigCacheEntry *c, int type, const char *w, int n, char **out, int *outlen)
{
	const ListDictionary *map = &c->map[type];
	char	   *cur = NULL;

	for (int d = 0; d < map->len; d++)
	{
		TSDictionaryCacheEntry *dict = lookup_ts_dictionary_cache(map->dictIds[d]);
		DictSubState state = {false, false, NULL};
		TSLexeme   *res = (TSLexeme *) DatumGetPointer(FunctionCall4(&dict->lexize,
																	 PointerGetDatum(dict->dictData),
																	 PointerGetDatum(w),
																	 Int32GetDatum(n),
																	 PointerGetDatum(&state)));
		char	   *lex;
		uint16		flags;

		if (res == NULL)
			continue;
		lex = res->lexeme ? pstrdup(res->lexeme) : NULL;
		flags = res->flags;
		for (TSLexeme *r = res; r->lexeme; r++)
			pfree(r->lexeme);
		pfree(res);
		if (cur)
			pfree(cur);
		cur = lex;
		if (!cur)
			return LEX_STOP;
		if (flags & TSL_FILTER)
		{
			w = cur;
			n = strlen(cur);
			continue;
		}
		*out = cur;
		*outlen = strlen(cur);
		return LEX_OK;
	}
	if (cur)
		pfree(cur);
	return LEX_UNKNOWN;
}

static int
ts_stem(SiftTsStem *ts, char *w, int n)
{
	TSConfigCacheEntry *c = lookup_ts_config_cache(ts->cfg);
	bool		digit = false,
				alpha = false,
				ascii = true;
	char	   *r;
	int			m,
				type,
				res;

	for (int i = 0; i < n; i++)
	{
		unsigned char b = w[i];

		if (b >= '0' && b <= '9')
			digit = true;
		else
			alpha = true;
		ascii &= b < 0x80;
	}
	if (!alpha)
		return n;
	type = digit ? TS_NUMWORD : ascii ? TS_ASCIIWORD : TS_WORD;
	if (c->lenmap <= type || (res = lexize(c, type, w, n, &r, &m)) != LEX_OK)
		return c->lenmap <= type ? LEX_UNKNOWN : res;
	if (m > SIFT_MAX_TERM_BYTES)
		m = n;
	else
		memcpy(w, r, m);
	pfree(r);
	return m;
}

static int
ts_memo_fn(void *ts, char *w, int n)
{
	return ts_stem((SiftTsStem *) ts, w, n);
}

int
sift_ts_stem(SiftTsStem *ts, char *w, int n)
{
	return memo_stem(&ts->memo, ts_memo_fn, ts, w, n);
}
