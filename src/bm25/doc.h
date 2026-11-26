/*
 * bm25/doc.h - the document of one row of a sift index: the text its key columns make (each column through its output function,
 * several columns as the sections of one text), and the exact key of a UNIQUE index, a synthetic term of the document's postings.
 */
#ifndef SIFT_BM25_DOC_H
#define SIFT_BM25_DOC_H

#include "access/genam.h"
#include "utils/rel.h"

#include "bm25/analyze/analyze.h"

/*
 * The text the key columns of the index hold in values/isnull, in cx when it is built and not the column's own: the columns apart by
 * SIFT_FIELD_SEP, a NULL column an empty section. NULL for a row that has no document: every key column is NULL, unless the index is
 * UNIQUE NULLS NOT DISTINCT, whose NULL key is a key.
 */
extern text *sift_bm25_assemble_doc(Relation index, const Datum *values, const bool *isnull, MemoryContext cx);

/* The functions the docs of the index read their key columns with, looked up once in cx for the docs of a pass (sift_bm25_assemble_doc looks them up for each doc) */
typedef struct SiftDocKeys
{
	Relation	index;
	FmgrInfo	out[INDEX_MAX_KEYS];	/* the output function of a column that is no text or varchar; fn_oid InvalidOid for those */
	FmgrInfo	hash[INDEX_MAX_KEYS];	/* UNIQUE: the extended hash of the column's type, fn_oid InvalidOid when it has none */
} SiftDocKeys;

extern void sift_bm25_doc_keys(Relation index, SiftDocKeys *keys, MemoryContext cx);
extern text *sift_bm25_keys_doc(SiftDocKeys *keys, const Datum *values, const bool *isnull, MemoryContext cx);
extern bool sift_bm25_keys_unique(SiftDocKeys *keys, const Datum *values, const bool *isnull, uint64 *key);

/* The key of a UNIQUE index for the row: false when the row has none (no UNIQUE index, or a NULL key column of NULLS DISTINCT) */
extern bool sift_bm25_unique_key(Relation index, const Datum *values, const bool *isnull, uint64 *key);

/*
 * The terms of a document with the term of its unique key added. A NUL byte is in no text, so no word is the key's term, and it does not count in
 * the document's length; but a length of 0 is no document (a live posting has one), so a text without a token has length 1 to have its key.
 * The result is valid until the next call.
 */
extern int	sift_bm25_unique_terms(SiftAnalyzer *an, const SiftTermFreq *terms, int nterms, uint64 key, uint32 *dl, SiftTermFreq **out);

/* The terms of the document of one row (with its unique key's term); false for a row that has no document. *len is the bytes of the text, which is in CurrentMemoryContext */
extern bool sift_bm25_row_terms(SiftAnalyzer *an, SiftDocKeys *keys, const Datum *values, const bool *isnull,
								SiftTermFreq **terms, int *nterms, uint32 *dl, int *len);

/*
 * The insert of a row with a unique key (bm25/unique.c): looks for a live row with the same key, an error (ERRCODE_UNIQUE_VIOLATION) when there is
 * one, false when one may be (UNIQUE_CHECK_PARTIAL: the check is made again at commit). Returns holding the lock of the key's bucket, which
 * serializes the inserts of one key until the row's own posting is written, and sift_bm25_unique_release lets go; UNIQUE_CHECK_EXISTING, the
 * recheck of a row already inserted, writes nothing and does not hold it.
 */
extern bool sift_bm25_unique_check(Relation index, Relation heap, const Datum *values, const bool *isnull,
								   ItemPointer tid, IndexUniqueCheck check, uint64 key);
extern void sift_bm25_unique_release(Relation index, uint64 key);

/* The pre-check of INSERT ... ON CONFLICT: the rows with the key the scan keys (one per key column, as the executor builds them) hold; they are rows of the key's term whose columns are equal */
extern List *sift_bm25_unique_twins(Relation index, Relation heap, const ScanKeyData *keys, int nkeys);

/* The keys of the rows a UNIQUE index build has seen, to refuse a duplicate before anything is written */
typedef struct SiftUniqueBuild SiftUniqueBuild;
extern SiftUniqueBuild *sift_unique_build_begin(Relation heap, Relation index, MemoryContext cx);
extern void sift_unique_build_add(SiftUniqueBuild *ub, ItemPointer tid, uint64 key, const Datum *values, const bool *isnull);
extern void sift_unique_build_end(SiftUniqueBuild *ub);

#endif							/* SIFT_BM25_DOC_H */
