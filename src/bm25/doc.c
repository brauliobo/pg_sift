/*
 * bm25/doc.c - the document of one row of a sift index (bm25/doc.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

#include "bm25/doc.h"
#include "core/sift.h"

#define UNIQUE_KEY_BYTES	9

static Oid
key_type(Relation index, int k)
{
	return TupleDescAttr(RelationGetDescr(index), k)->atttypid;
}

/* The key column k as a text: text and varchar as they are, any other type as its output function writes it */
static text *
column_text(Relation index, int k, Datum d, MemoryContext cx)
{
	Oid			type = key_type(index, k),
				outfunc;
	bool		isvarlena;
	char	   *s;
	text	   *t;
	MemoryContext old;

	if (type == TEXTOID || type == VARCHAROID)
		return DatumGetTextPP(d);
	getTypeOutputInfo(type, &outfunc, &isvarlena);
	s = OidOutputFunctionCall(outfunc, d);
	old = MemoryContextSwitchTo(cx);
	t = cstring_to_text(s);
	MemoryContextSwitchTo(old);
	pfree(s);
	return t;
}

static bool
keeps_null_key(Relation index)
{
	return index->rd_index->indisunique && index->rd_index->indnullsnotdistinct;
}

text *
sift_bm25_assemble_doc(Relation index, const Datum *values, const bool *isnull, MemoryContext cx)
{
	int			nkey = IndexRelationGetNumberOfKeyAttributes(index);
	StringInfoData buf;
	MemoryContext old;
	bool		any = false;

	for (int k = 0; k < nkey; k++)
		any |= !isnull[k];
	if (!any)
		return keeps_null_key(index) ? cstring_to_text_with_len("", 0) : NULL;
	if (nkey == 1)
		return column_text(index, 0, values[0], cx);
	old = MemoryContextSwitchTo(cx);
	initStringInfo(&buf);
	for (int k = 0; k < nkey; k++)
	{
		if (k > 0)
			appendStringInfoChar(&buf, SIFT_FIELD_SEP);
		if (!isnull[k])
		{
			text	   *t = column_text(index, k, values[k], cx);

			appendBinaryStringInfo(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
		}
	}
	MemoryContextSwitchTo(old);
	return cstring_to_text_with_len(buf.data, buf.len);
}

bool
sift_bm25_unique_key(Relation index, const Datum *values, const bool *isnull, uint64 *key)
{
	int			nkey = IndexRelationGetNumberOfKeyAttributes(index);
	uint64		h = 0;

	if (!index->rd_index->indisunique)
		return false;
	for (int k = 0; k < nkey; k++)
	{
		TypeCacheEntry *tc;

		if (isnull[k])
		{
			if (!index->rd_index->indnullsnotdistinct)
				return false;
			h = h * UINT64CONST(0x9E3779B97F4A7C15) + 1;
			continue;
		}
		tc = lookup_type_cache(key_type(index, k), TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(tc->hash_extended_proc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s", format_type_be(key_type(index, k))),
					 errhint("A UNIQUE sift index needs key columns of types that have a hash operator class.")));
		h = DatumGetUInt64(FunctionCall2Coll(&tc->hash_extended_proc_finfo, index->rd_indcollation[k], values[k], UInt64GetDatum(h)));
	}
	*key = h;
	return true;
}

/* The bytes of the term of the key, one per backend: the terms of a doc and this one are the analyzer's spare terms */
static char unique_key[UNIQUE_KEY_BYTES];

int
sift_bm25_unique_terms(SiftAnalyzer *an, const SiftTermFreq *terms, int nterms, uint64 key, uint32 *dl, SiftTermFreq **out)
{
	SiftTermFreq k = {.term = unique_key, .len = UNIQUE_KEY_BYTES, .tf = 1};
	SiftTermFreq *merged = sift_analyzer_spare_terms(an, nterms + 1);
	int			at = 0;

	unique_key[0] = '\0';
	for (int i = 0; i < 8; i++)
		unique_key[1 + i] = (char) (key >> (56 - 8 * i));
	k.hash = sift_term_hash(k.term, k.len);
	while (at < nterms && (terms[at].hash < k.hash || (terms[at].hash == k.hash && memcmp(terms[at].term, k.term, Min(terms[at].len, k.len)) < 0)))
		at++;
	memcpy(merged, terms, at * sizeof(SiftTermFreq));
	merged[at] = k;
	memcpy(merged + at + 1, terms + at, (nterms - at) * sizeof(SiftTermFreq));
	*dl = Max(*dl, 1);
	*out = merged;
	return nterms + 1;
}

bool
sift_bm25_row_terms(SiftAnalyzer *an, Relation index, const Datum *values, const bool *isnull,
					SiftTermFreq **terms, int *nterms, uint32 *dl, int *len)
{
	text	   *t = sift_bm25_assemble_doc(index, values, isnull, CurrentMemoryContext);
	uint64		key;

	if (t == NULL)
		return false;
	*len = VARSIZE_ANY_EXHDR(t);
	*nterms = sift_analyze_doc(an, VARDATA_ANY(t), *len, terms, dl);
	if (sift_bm25_unique_key(index, values, isnull, &key))
		*nterms = sift_bm25_unique_terms(an, *terms, *nterms, key, dl, terms);
	return true;
}
