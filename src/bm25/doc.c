/*
 * bm25/doc.c - the document of one row of a sift index (bm25/doc.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
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

/* The output functions of the key columns that are no text or varchar (taken as they are) */
static void
keys_out(Relation index, SiftDocKeys *keys, MemoryContext cx)
{
	keys->index = index;
	for (int k = 0; k < IndexRelationGetNumberOfKeyAttributes(index); k++)
	{
		Oid			type = key_type(index, k),
					outfunc;
		bool		isvarlena;

		keys->out[k].fn_oid = InvalidOid;
		if (type == TEXTOID || type == VARCHAROID)
			continue;
		getTypeOutputInfo(type, &outfunc, &isvarlena);
		fmgr_info_cxt(outfunc, &keys->out[k], cx);
	}
}

/* The extended hash functions of the key columns of a UNIQUE index (none for a type without one) */
static void
keys_hash(Relation index, SiftDocKeys *keys, MemoryContext cx)
{
	keys->index = index;
	for (int k = 0; k < IndexRelationGetNumberOfKeyAttributes(index); k++)
	{
		TypeCacheEntry *tc = lookup_type_cache(key_type(index, k), TYPECACHE_HASH_EXTENDED_PROC_FINFO);

		keys->hash[k].fn_oid = InvalidOid;
		if (OidIsValid(tc->hash_extended_proc))
			fmgr_info_copy(&keys->hash[k], &tc->hash_extended_proc_finfo, cx);
	}
}

void
sift_bm25_doc_keys(Relation index, SiftDocKeys *keys, MemoryContext cx)
{
	keys_out(index, keys, cx);
	if (index->rd_index->indisunique)
		keys_hash(index, keys, cx);
}

/* The key column k as a text in the current context: text and varchar as they are (detoasted), any other type as its output function writes it */
static text *
column_text(SiftDocKeys *keys, int k, Datum d)
{
	char	   *s;
	text	   *t;

	if (!OidIsValid(keys->out[k].fn_oid))
		return DatumGetTextPP(d);
	s = OutputFunctionCall(&keys->out[k], d);
	t = cstring_to_text(s);
	pfree(s);
	return t;
}

static bool
keeps_null_key(Relation index)
{
	return index->rd_index->indisunique && index->rd_index->indnullsnotdistinct;
}

/* The sections of several key columns: each column's text is freed once it is copied, so a row holds its columns and its doc at most */
static text *
sections_doc(SiftDocKeys *keys, const Datum *values, const bool *isnull, int nkey)
{
	text	   *cols[INDEX_MAX_KEYS] = {0};
	Size		len = nkey - 1;
	text	   *doc;
	char	   *p;

	for (int k = 0; k < nkey; k++)
		if (!isnull[k])
		{
			cols[k] = column_text(keys, k, values[k]);
			len += VARSIZE_ANY_EXHDR(cols[k]);
		}
	if (len > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the document of a row of index \"%s\" needs %zu kB, more than a text holds (%zu kB)",
						RelationGetRelationName(keys->index), (size_t) (len / 1024), (size_t) ((MaxAllocSize - VARHDRSZ) / 1024)),
				 errhint("Index fewer or shorter columns.")));
	doc = palloc(VARHDRSZ + len);
	SET_VARSIZE(doc, VARHDRSZ + len);
	p = VARDATA(doc);
	for (int k = 0; k < nkey; k++)
	{
		if (k > 0)
			*p++ = SIFT_FIELD_SEP;
		if (cols[k] == NULL)
			continue;
		memcpy(p, VARDATA_ANY(cols[k]), VARSIZE_ANY_EXHDR(cols[k]));
		p += VARSIZE_ANY_EXHDR(cols[k]);
		if ((Pointer) cols[k] != DatumGetPointer(values[k]))
			pfree(cols[k]);
	}
	return doc;
}

text *
sift_bm25_keys_doc(SiftDocKeys *keys, const Datum *values, const bool *isnull, MemoryContext cx)
{
	int			nkey = IndexRelationGetNumberOfKeyAttributes(keys->index);
	MemoryContext old;
	text	   *doc;
	bool		any = false;

	for (int k = 0; k < nkey; k++)
		any |= !isnull[k];
	old = MemoryContextSwitchTo(cx);
	if (!any)
		doc = keeps_null_key(keys->index) ? cstring_to_text_with_len("", 0) : NULL;
	else if (nkey == 1)
		doc = column_text(keys, 0, values[0]);
	else
		doc = sections_doc(keys, values, isnull, nkey);
	MemoryContextSwitchTo(old);
	return doc;
}

text *
sift_bm25_assemble_doc(Relation index, const Datum *values, const bool *isnull, MemoryContext cx)
{
	SiftDocKeys keys;

	keys_out(index, &keys, CurrentMemoryContext);
	return sift_bm25_keys_doc(&keys, values, isnull, cx);
}

bool
sift_bm25_keys_unique(SiftDocKeys *keys, const Datum *values, const bool *isnull, uint64 *key)
{
	Relation	index = keys->index;
	uint64		h = 0;

	if (!index->rd_index->indisunique)
		return false;
	for (int k = 0; k < IndexRelationGetNumberOfKeyAttributes(index); k++)
	{
		if (isnull[k])
		{
			if (!index->rd_index->indnullsnotdistinct)
				return false;
			h = h * UINT64CONST(0x9E3779B97F4A7C15) + 1;
			continue;
		}
		if (!OidIsValid(keys->hash[k].fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s", format_type_be(key_type(index, k))),
					 errhint("A UNIQUE sift index needs key columns of types that have a hash operator class.")));
		h = DatumGetUInt64(FunctionCall2Coll(&keys->hash[k], index->rd_indcollation[k], values[k], UInt64GetDatum(h)));
	}
	*key = h;
	return true;
}

bool
sift_bm25_unique_key(Relation index, const Datum *values, const bool *isnull, uint64 *key)
{
	SiftDocKeys keys;

	if (!index->rd_index->indisunique)
		return false;
	keys_hash(index, &keys, CurrentMemoryContext);
	return sift_bm25_keys_unique(&keys, values, isnull, key);
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
	if (nterms > 0)
	{
		memcpy(merged, terms, at * sizeof(SiftTermFreq));
		memcpy(merged + at + 1, terms + at, (nterms - at) * sizeof(SiftTermFreq));
	}
	merged[at] = k;
	*dl = Max(*dl, 1);
	*out = merged;
	return nterms + 1;
}

bool
sift_bm25_row_terms(SiftAnalyzer *an, SiftDocKeys *keys, const Datum *values, const bool *isnull,
					SiftTermFreq **terms, int *nterms, uint32 *dl, int *len)
{
	text	   *t = sift_bm25_keys_doc(keys, values, isnull, CurrentMemoryContext);
	uint64		key;

	if (t == NULL)
		return false;
	*len = VARSIZE_ANY_EXHDR(t);
	*nterms = sift_analyze_doc(an, VARDATA_ANY(t), *len, terms, dl);
	if (sift_bm25_keys_unique(keys, values, isnull, &key))
		*nterms = sift_bm25_unique_terms(an, *terms, *nterms, key, dl, terms);
	return true;
}
