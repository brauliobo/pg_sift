/*
 * col/schema_test.c - L1 tests: schemas and dictionaries.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/compress.h"
#include "col/enc.h"
#include "col/enc_test_int.h"

static bool
dict_is(const SiftCDict *d, uint32 code, const char *s, uint32 len)
{
	return dict_len(d, code) == len && memcmp(d->bytes + d->off[code], s, len) == 0;
}

void
check_schema(TCtx *t, const Case *c)
{
	const SiftColSchema *s = c->schema;
	StringInfoData buf;
	SiftColSchema *p;
	SiftColDef	defs[2];

	initStringInfo(&buf);
	sift_cschema_serialize(s, &buf);
	p = sift_cschema_parse(CurrentMemoryContext, buf.data, buf.len);
	CHECK(t, p != NULL && p->ncols == s->ncols && p->hash == s->hash && p->ngroups == s->ngroups, "schema parse");
	for (int i = 0; p != NULL && i < s->ncols; i++)
		CHECK(t, p->cols[i].id == s->cols[i].id && p->cols[i].parent == s->cols[i].parent && p->cols[i].kind == s->cols[i].kind &&
			  p->cols[i].store == s->cols[i].store && p->cols[i].attno == s->cols[i].attno && p->cols[i].type == s->cols[i].type &&
			  strcmp(p->cols[i].path, s->cols[i].path) == 0, "schema leaf %d", i);
	for (Size cut = 0; cut < buf.len; cut += 1 + RAND(t, 9))
		CHECK(t, sift_cschema_parse(CurrentMemoryContext, buf.data, cut) == NULL, "truncated schema (%zu bytes) parsed", cut);
	buf.data[8 + RAND(t, 4)] ^= 1;	/* the hash */
	CHECK(t, sift_cschema_parse(CurrentMemoryContext, buf.data, buf.len) == NULL, "schema with a wrong hash parsed");

	/* the hash covers everything but the flags */
	defs[0] = (SiftColDef) {0, -1, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 1, SIFT_COLT_INT8, "total"};
	defs[1] = defs[0];
	defs[1].id = 1;
	defs[1].path = "total2";
	{
		SiftColSchema *a = sift_cschema_create(CurrentMemoryContext, defs, 2),
				   *b;

		defs[1].flags = SIFT_COLD_LOSSY;
		b = sift_cschema_create(CurrentMemoryContext, defs, 2);
		CHECK(t, a->hash == b->hash, "flags changed the schema hash");
		defs[1].path = "total3";
		b = sift_cschema_create(CurrentMemoryContext, defs, 2);
		CHECK(t, a->hash != b->hash, "path left the schema hash alone");
	}

	/* invalid leaf lists */
	defs[0].parent = 0;
	CHECK(t, ERRCODE_OF(sift_cschema_create(CurrentMemoryContext, defs, 1)) == ERRCODE_INVALID_PARAMETER_VALUE, "self parent accepted");
	defs[0].parent = -1;
	defs[1].id = 5;
	CHECK(t, ERRCODE_OF(sift_cschema_create(CurrentMemoryContext, defs, 2)) == ERRCODE_INVALID_PARAMETER_VALUE, "bad id accepted");
	defs[1].id = 1;
	defs[1].parent = 0;
	CHECK(t, ERRCODE_OF(sift_cschema_create(CurrentMemoryContext, defs, 2)) == ERRCODE_INVALID_PARAMETER_VALUE, "child of a scalar accepted");
	defs[1].parent = -1;
	defs[1].kind = SIFT_COLK_GROUP;
	defs[1].store = SIFT_COLS_STR;
	CHECK(t, ERRCODE_OF(sift_cschema_create(CurrentMemoryContext, defs, 2)) == ERRCODE_INVALID_PARAMETER_VALUE, "string group accepted");
	CHECK(t, ERRCODE_OF(sift_cschema_create(CurrentMemoryContext, defs, 0)) == ERRCODE_INVALID_PARAMETER_VALUE, "empty schema accepted");
}

/*
 * The string list form of a dictionary (policies auto, none, lz4, zstd): the same codes come back, and damaged bytes are refused or read
 * as a valid dictionary, never read past their end. Draws from a generator of its own: the rounds that follow keep theirs.
 */
static void
check_dict_strs(TCtx *t, SiftCDict *d, char **str, const uint32 *len, uint32 n, const StringInfoData *plain)
{
	static const char *const specs[] = {"auto", "none", "lz4", "zstd"};
	pg_prng_state rng;

	pg_prng_seed(&rng, n);
	/* the plain form: every truncation is refused */
	for (Size cut = 0; cut + 1 < (Size) plain->len; cut += 1 + pg_prng_uint64_range(&rng, 0, Max(49, plain->len / 32)))
		CHECK(t, sift_cdict_parse(CurrentMemoryContext, plain->data, cut, &(Size) {0}) == NULL, "truncated dictionary (%zu bytes) parsed", cut);
	for (int k = 0; k < 4; k++)
	{
		SiftCcPolicy pol;
		StringInfoData buf;
		SiftCDict  *p;
		Size		used = 0;

		CHECK(t, sift_cc_policy_parse(specs[k], &pol) == NULL, "policy %s", specs[k]);
		if (pol.mode == SIFT_CCM_LAYER && !sift_colz_available(pol.alg))
			continue;
		initStringInfo(&buf);
		sift_cdict_serialize(d, &pol, &buf);
		CHECK(t, buf.len <= plain->len, "dictionary (%s) of %d bytes is larger than the plain form of %d", specs[k], buf.len, plain->len);
		p = sift_cdict_parse(CurrentMemoryContext, buf.data, buf.len, &used);
		CHECK(t, p != NULL && used == (Size) buf.len && p->n == d->n && p->nbytes == d->nbytes, "dictionary (%s) parse", specs[k]);
		for (uint32 i = 0; p != NULL && i < n; i += 1 + (uint32) pg_prng_uint64_range(&rng, 0, 4))
			CHECK(t, sift_cdict_find(p, str[i], len[i]) == sift_cdict_find(d, str[i], len[i]), "dictionary (%s) code", specs[k]);
		for (int rounds = 0; rounds < 10; rounds++)
		{
			SiftCDict  *q = NULL;
			char	   *bad = memcpy(palloc(buf.len), buf.data, buf.len);
			Size		cut = buf.len;

			if (pg_prng_bool(&rng))
				bad[pg_prng_uint64_range(&rng, 0, buf.len - 1)] ^= (char) (1 << pg_prng_uint64_range(&rng, 0, 7));
			else
				cut = pg_prng_uint64_range(&rng, 0, buf.len - 1);
			(void) ERRCODE_OF(q = sift_cdict_parse(CurrentMemoryContext, bad, cut, &used));
			CHECK(t, q == NULL || used <= cut, "damaged dictionary consumed %zu of %zu bytes", used, cut);
			pfree(bad);
		}
	}
}

void
check_dict(TCtx *t)
{
	SiftCDict  *d = sift_cdict_create(CurrentMemoryContext),
			   *p;
	uint32		n = 1 + RAND(t, 3000);
	char	  **str = palloc(n * sizeof(char *));
	uint32	   *len = palloc(n * sizeof(uint32));
	uint32		distinct = 0;
	StringInfoData buf;
	SiftColDict view;
	Size		used = 0;

	for (uint32 i = 0; i < n; i++)
	{
		uint32		l = RAND(t, 6) == 0 ? RAND(t, SIFT_COL_DICT_MAX_ENTRY + 1) : RAND(t, 6);
		uint32		code;

		str[i] = palloc(l + 1);
		for (uint32 j = 0; j < l; j++)
			str[i][j] = (char) RAND(t, RAND(t, 2) ? 3 : 256);
		len[i] = l;
		code = sift_cdict_intern(d, str[i], l);
		CHECK(t, code <= distinct, "code %u past %u", code, distinct);
		distinct = Max(distinct, code + 1);
		CHECK(t, sift_cdict_find(d, str[i], l) == code, "find after intern");
	}
	CHECK(t, d->n == distinct, "dictionary of %u codes, %u distinct", d->n, distinct);
	for (uint32 i = 0; i < n; i++)
	{
		int64		code = sift_cdict_find(d, str[i], len[i]);

		CHECK(t, code >= 0 && dict_is(d, (uint32) code, str[i], len[i]), "stored string differs");
	}
	{
		char		absent[200];

		memset(absent, 'Z', sizeof(absent));
		CHECK(t, sift_cdict_find(d, absent, sizeof(absent)) == -1, "find of an absent value");
	}
	CHECK(t, ERRCODE_OF(sift_cdict_intern(d, str[0], SIFT_COL_MAX_VALUE + 1)) == ERRCODE_PROGRAM_LIMIT_EXCEEDED, "a value over 1 GB accepted");

	initStringInfo(&buf);
	appendStringInfoString(&buf, "pad");
	resetStringInfo(&buf);
	sift_cdict_serialize(d, NULL, &buf);
	p = sift_cdict_parse(CurrentMemoryContext, buf.data, buf.len, &used);
	CHECK(t, p != NULL && used == (Size) buf.len && p->n == d->n, "dictionary parse");
	for (uint32 i = 0; p != NULL && i < n; i += 1 + RAND(t, 5))
		CHECK(t, sift_cdict_find(p, str[i], len[i]) == sift_cdict_find(d, str[i], len[i]), "parsed dictionary code");
	if (p)
	{
		sift_cdict_view(p, 77, &view);
		CHECK(t, view.n == p->n && view.id == 77 && (p->n == 0 || SIFT_COL_DICT_LEN(&view, p->n - 1) == dict_len(p, p->n - 1)), "dictionary view");
		/* a parsed dictionary keeps interning */
		CHECK(t, sift_cdict_intern(p, str[0], len[0]) == sift_cdict_find(d, str[0], len[0]), "intern into a parsed dictionary");
	}
	check_dict_strs(t, d, str, len, n, &buf);
}

#endif							/* SIFT_TESTING */
