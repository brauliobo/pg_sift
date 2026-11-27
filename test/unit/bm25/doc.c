/*
 * test/unit/bm25/doc.c - the document of a row of several key columns (bm25/doc.c): it is allocated once, at its length, and the columns' own texts (detoasted copies,
 * output function results) are freed as they are copied, so that the call leaves only its result; a document past what a text holds fails before any copy; the key term
 * of a UNIQUE index joins a doc that has no terms (the analyzer's array is NULL then) without touching it.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/doc.c"
#include "core/cpu.h"

#define COL_BYTES	(1024 * 1024)

SiftCpu		sift_cpu;

static bool fake_toast = true;

/* A detoasted column is a copy: what a wide column out of line gives */
struct varlena *
pg_detoast_datum_packed(struct varlena *datum)
{
	struct varlena *copy;

	if (!fake_toast)
		return datum;
	copy = palloc(VARSIZE(datum));
	memcpy(copy, datum, VARSIZE(datum));
	return copy;
}

text *
cstring_to_text_with_len(const char *s, int len)
{
	text	   *t = palloc(VARHDRSZ + len);

	SET_VARSIZE(t, VARHDRSZ + len);
	memcpy(VARDATA(t), s, len);
	return t;
}

static text *
column(char c, Size len)
{
	text	   *t = malloc(VARHDRSZ + len);

	SET_VARSIZE(t, VARHDRSZ + len);
	memset(VARDATA(t), c, len);
	return t;
}

static RelationData rel;
static FormData_pg_index idx;
static FormData_pg_class cls;
static SiftDocKeys keys;

static void
two_columns(void)
{
	idx.indnkeyatts = 2;
	strcpy(NameStr(cls.relname), "doc_idx");
	rel.rd_index = &idx;
	rel.rd_rel = &cls;
	keys.index = &rel;
	keys.out[0].fn_oid = keys.out[1].fn_oid = InvalidOid;
}

static void
leaves_only_its_result(void)
{
	Datum		values[2] = {PointerGetDatum(column('a', COL_BYTES)), PointerGetDatum(column('b', COL_BYTES))};
	bool		isnull[2] = {false, false};
	size_t		before = pgshim_live_bytes();
	text	   *doc = sift_bm25_keys_doc(&keys, values, isnull, CurrentMemoryContext);

	SIFT_CHECKF(VARSIZE_ANY_EXHDR(doc) == 2 * COL_BYTES + 1, "the doc holds %zu bytes", (size_t) VARSIZE_ANY_EXHDR(doc));
	SIFT_CHECKF(VARDATA(doc)[COL_BYTES] == SIFT_FIELD_SEP && VARDATA(doc)[COL_BYTES + 1] == 'b', "the sections of the doc");
	SIFT_CHECKF(pgshim_live_bytes() - before == VARSIZE(doc), "the call left %zu bytes for a doc of %zu",
				pgshim_live_bytes() - before, (size_t) VARSIZE(doc));
	pfree(doc);
	free(DatumGetPointer(values[0]));
	free(DatumGetPointer(values[1]));
}

static void
build_past_a_text(void *arg)
{
	static uint32 header[4];
	text	   *huge = (text *) header;
	Datum		values[2] = {PointerGetDatum(huge), PointerGetDatum(huge)};
	bool		isnull[2] = {false, false};

	SET_VARSIZE(huge, MaxAllocSize / 2 + VARHDRSZ);
	fake_toast = false;
	sift_bm25_keys_doc(&keys, values, isnull, CurrentMemoryContext);
}

static void
unique_term_of_no_terms(void)
{
	SiftAnalyzerConfig cfg = {0};
	SiftAnalyzer *an = sift_analyzer_create(&cfg, CurrentMemoryContext);
	SiftTermFreq *out;
	uint32		dl = 0;

	SIFT_CHECKF(sift_bm25_unique_terms(an, NULL, 0, UINT64CONST(42), &dl, &out) == 1, "the key term alone");
	SIFT_CHECKF(out[0].len == UNIQUE_KEY_BYTES && dl == 1, "the key term and its length");
}

int
main(void)
{
	two_columns();
	leaves_only_its_result();
	unique_term_of_no_terms();
	SIFT_CHECKF(!pgshim_try(build_past_a_text, NULL) && pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED &&
				strstr(pgshim_errmsg, "more than a text holds") != NULL, "a doc past 1 GB fails before a copy: %s", pgshim_errmsg);
	return sift_check_summary();
}
