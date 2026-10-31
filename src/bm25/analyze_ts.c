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

/* Token types of the default parser (wparser_def.c) */
#define TS_ASCIIWORD	1
#define TS_WORD			2
#define TS_NUMWORD		3

/*
 * Memo of every word the dictionaries answered (a lexize costs thousands of instructions, words follow Zipf's law and the vocabulary
 * of a corpus is small): entries [uint16 inlen][uint16 outlen][in][out] in an arena, found through an open-addressing table of
 * offsets (+ 1). outlen 0 is a word no dictionary knows, TS_MEMO_STOP a stop word. Both are dropped when the arena passes TS_MEMO_MAX, so a vocabulary without end stays bounded.
 */
#define TS_MEMO_MAX		(8 * 1024 * 1024)
#define TS_MEMO_STOP	0xFFFF	/* outlen of a stop word */
#define TS_MEMO_TAB0	4096

struct SiftTsStem
{
	Oid			cfg;
	MemoryContext mcxt;
	char	   *arena;
	uint32		used;
	uint32		cap;
	uint32	   *tab;
	uint32		tabmask;
	uint32		nent;
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

	*ts = (SiftTsStem) {.cfg = ts_config_oid(config), .mcxt = mcxt};
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

static void
memo_clear(SiftTsStem *ts)
{
	ts->used = 0;
	ts->nent = 0;
	if (ts->tab)
		memset(ts->tab, 0, (ts->tabmask + 1) * sizeof(uint32));
}

static void
memo_grow_tab(SiftTsStem *ts)
{
	uint32		size = ts->tab ? (ts->tabmask + 1) * 2 : TS_MEMO_TAB0;

	if (ts->tab)
		pfree(ts->tab);
	ts->tab = MemoryContextAllocZero(ts->mcxt, size * sizeof(uint32));
	ts->tabmask = size - 1;
	for (uint32 off = 0; off < ts->used;)
	{
		const uint16 *h = (const uint16 *) (ts->arena + off);
		uint32		i = (uint32) sift_term_hash((const char *) (h + 2), h[0]) & ts->tabmask;

		while (ts->tab[i])
			i = (i + 1) & ts->tabmask;
		ts->tab[i] = off + 1;
		off += 4 + h[0] + (h[1] == TS_MEMO_STOP ? 0 : h[1]);
	}
}

int
sift_ts_stem(SiftTsStem *ts, char *w, int n)
{
	uint32		i;
	int			m;

	if (!ts->tab)
		memo_grow_tab(ts);
	i = (uint32) sift_term_hash(w, n) & ts->tabmask;
	for (; ts->tab[i]; i = (i + 1) & ts->tabmask)
	{
		const uint16 *h = (const uint16 *) (ts->arena + ts->tab[i] - 1);

		if (h[0] == n && memcmp(h + 2, w, n) == 0)
		{
			if (h[1] == TS_MEMO_STOP)
				return LEX_STOP;
			memcpy(w, (const char *) (h + 2) + n, h[1]);
			return h[1];
		}
	}
	{
		char		in[SIFT_MAX_TERM_BYTES + 16];
		uint16		hd[2];

		memcpy(in, w, n);
		m = ts_stem(ts, w, n);
		if (ts->used + 4 + n + Max(m, 0) > ts->cap)
		{
			if (ts->cap >= TS_MEMO_MAX)
			{
				memo_clear(ts);
				i = (uint32) sift_term_hash(in, n) & ts->tabmask;
				while (ts->tab[i])
					i = (i + 1) & ts->tabmask;
			}
			else
			{
				ts->cap = ts->cap ? ts->cap * 2 : 64 * 1024;
				ts->arena = ts->arena ? repalloc(ts->arena, ts->cap) : MemoryContextAlloc(ts->mcxt, ts->cap);
			}
		}
		hd[0] = n;
		hd[1] = m < 0 ? TS_MEMO_STOP : m;
		memcpy(ts->arena + ts->used, hd, 4);
		memcpy(ts->arena + ts->used + 4, in, n);
		if (m > 0)
			memcpy(ts->arena + ts->used + 4 + n, w, m);
		ts->tab[i] = ts->used + 1;
		ts->used += 4 + n + Max(m, 0);
		if (++ts->nent * 2 > ts->tabmask + 1)
			memo_grow_tab(ts);
	}
	return m;
}
