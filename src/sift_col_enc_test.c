/*
 * sift_col_enc_test.c - test-only SQL functions sift._test_col_enc_*, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. They build random batches (every storage class, NULLs, overflow, arrays, sparse and dense pages, strings) and check L1
 * (sift_col_enc.c) against a naive evaluation of the same rows.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "common/pg_prng.h"
#include "funcapi.h"

#include "sift_col_enc.h"

PG_FUNCTION_INFO_V1(sift_test_col_enc_roundtrip);

typedef struct TCtx
{
	pg_prng_state rng;
	int64		checks;
	int64		failures;
	uint32		encodings;		/* bit per chunk encoding written: CONST, PACK, FOR 8, 16, 32, 64; bit 9: the chunk codec */
	bool		wide;			/* the next text leaf gets tens of thousands of distinct values (codes of 16 and 17 bits) */
	bool		full;			/* this round is a full superblock: 256 dense pages */
} TCtx;

static inline uint32
dict_len(const SiftCDict *d, uint32 code)
{
	return d->off[code + 1] - d->off[code];
}

static bool
dict_is(const SiftCDict *d, uint32 code, const char *s, uint32 len)
{
	return dict_len(d, code) == len && memcmp(d->bytes + d->off[code], s, len) == 0;
}

/* Counts a check; the first few failures are reported as warnings so that the regression output shows them */
#define CHECK(t, cond, ...) \
	do { \
		(t)->checks++; \
		if (!(cond)) \
		{ \
			if (++(t)->failures <= 5) \
				elog(WARNING, "col_enc check failed at line %d: %s", __LINE__, psprintf(__VA_ARGS__)); \
		} \
	} while (0)

#define RAND(t, n)		((uint32) pg_prng_uint64_range(&(t)->rng, 0, (n) - 1))
#define CHANCE(t, p)	((p) > 0 && pg_prng_double(&(t)->rng) < (p))

/* ---------------------------------------------------------------------------
 * Random schemas, rows and batches
 * ------------------------------------------------------------------------- */

typedef struct LeafProf
{
	int			mode;			/* INT: 0 constant, 1 narrow, 2 wide, 3 clustered; F: 0 specials, 1 integers, 2 any; STR: pool */
	int			bits;
	int64		base;
	double		pnull;
	double		povf;
	uint32		npool;
	char	  **pool;
	uint32	   *plen;
	double		step;
	bool		seqpick;		/* STR: the pool entry is picked by the row number, so every value appears */
} LeafProf;

typedef struct Case
{
	SiftColSchema *schema;
	LeafProf   *prof;
	uint32		sb;
} Case;

static const char *const scalar_paths[] = {"store_id", "status", "total", "discount", "channel", "region", "note", "units"};
static const char *const item_paths[] = {"items[].category", "items[].sku", "items[].kind", "items[].qty", "items[].price"};

static SiftColStore
rand_store(TCtx *t)
{
	static const SiftColStore s[] = {SIFT_COLS_INT, SIFT_COLS_INT, SIFT_COLS_F4, SIFT_COLS_F8, SIFT_COLS_STR, SIFT_COLS_STR};

	return s[RAND(t, 6)];
}

static Oid
store_type(SiftColStore s)
{
	return s == SIFT_COLS_INT ? INT8OID : s == SIFT_COLS_F4 ? FLOAT4OID : s == SIFT_COLS_F8 ? FLOAT8OID : TEXTOID;
}

static void
gen_prof(TCtx *t, LeafProf *p, SiftColStore store)
{
	static const int bitsv[] = {0, 1, 3, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 40, 62, 64};

	memset(p, 0, sizeof(*p));
	p->mode = RAND(t, store == SIFT_COLS_STR ? 1 : store == SIFT_COLS_INT ? 4 : 3);
	p->bits = bitsv[RAND(t, lengthof(bitsv))];
	p->base = (int64) pg_prng_uint64(&t->rng) >> RAND(t, 64);
	p->pnull = RAND(t, 4) == 0 ? 0 : RAND(t, 8) == 0 ? 1.0 : 0.15 * pg_prng_double(&t->rng);
	p->povf = RAND(t, 4) == 0 ? 0.04 : 0;
	p->step = 1 + RAND(t, 1000);
	if (store == SIFT_COLS_STR)
	{
		static const char *const stems[] = {"ship", "shipped", "ship_", "a", "", "zz", "pending", "paid", "paid_late", "\xC3\xA9t\xC3\xA9"};
		uint32		np = t->wide ? 33000 + RAND(t, 41000) : RAND(t, 3) == 0 ? 1 + RAND(t, 300) : 1 + RAND(t, 20);

		p->npool = np;
		p->seqpick = t->wide;
		p->pool = palloc(np * sizeof(char *));
		p->plen = palloc(np * sizeof(uint32));
		for (uint32 i = 0; i < np; i++)
		{
			const char *stem = stems[RAND(t, lengthof(stems))];
			uint32		len = t->wide ? 8 : RAND(t, 40) == 0 ? SIFT_COL_MAX_STR - RAND(t, 2) : strlen(stem) + RAND(t, 6) * (RAND(t, 3) == 0);
			char	   *s = palloc(len + 1);

			for (uint32 j = 0; j < len; j++)
				s[j] = j < strlen(stem) ? stem[j] : "abc\xFF\x01xyz"[RAND(t, 8)];
			if (t->wide)
				snprintf(s, len + 1, "w%07u", i);
			s[len] = '\0';
			p->pool[i] = s;
			p->plen[i] = len;
		}
		t->wide = false;
	}
}

static Case *
gen_case(TCtx *t)
{
	Case	   *c = palloc0(sizeof(Case));
	SiftColDef	defs[SIFT_COL_MAX_COLS];
	int			n = 0;
	int			nscalar = 2 + RAND(t, 5);
	int			ngroup = RAND(t, 3);

	for (int i = 0; i < nscalar; i++, n++)
		defs[n] = (SiftColDef) {n, -1, SIFT_COLK_SCALAR, rand_store(t), 0, n + 1, 0, scalar_paths[i]};
	for (int g = 0; g < ngroup; g++)
	{
		int			gid = n++;
		int			nchild = 1 + RAND(t, 3);

		defs[gid] = (SiftColDef) {gid, -1, SIFT_COLK_GROUP, SIFT_COLS_INT, 0, gid + 1, 0, "items[]"};
		for (int k = 0; k < nchild; k++, n++)
			defs[n] = (SiftColDef) {n, gid, SIFT_COLK_SCALAR, rand_store(t), 0, gid + 1, 0, item_paths[k]};
	}
	for (int i = 0; i < n; i++)
		defs[i].typid = defs[i].kind == SIFT_COLK_GROUP ? INT4OID : store_type(defs[i].store);
	c->schema = sift_cschema_create(CurrentMemoryContext, defs, n);
	c->prof = palloc(n * sizeof(LeafProf));
	for (int i = 0; i < n; i++)
	{
		gen_prof(t, &c->prof[i], c->schema->cols[i].store);
		if (c->schema->cols[i].kind == SIFT_COLK_GROUP)
			c->prof[i].pnull = RAND(t, 2) ? 0.1 : 0;
	}
	c->sb = RAND(t, 100000);
	return c;
}

static int
cmp_u32(const void *a, const void *b)
{
	return (int) *(const uint32 *) a - (int) *(const uint32 *) b;
}

/* Ascending TIDs of one superblock: 1..256 pages, each dense (offsets 1..cnt) or sparse (gaps) */
static SiftKey *
gen_keys(TCtx *t, uint32 sb, uint32 nrows, uint32 *nout)
{
	uint32		npages = t->full ? SIFT_SB_PAGES : 1 + RAND(t, Min(256, nrows)),
				per = t->full ? MaxHeapTuplesPerPage : Max(1, nrows / npages),
				n = 0,
				pages[SIFT_SB_PAGES];
	SiftKey    *keys = palloc((Size) npages * MaxHeapTuplesPerPage * sizeof(SiftKey));

	for (uint32 i = 0; i < SIFT_SB_PAGES; i++)
		pages[i] = i;
	for (uint32 i = 0; i < npages; i++)
	{
		uint32		j = i + RAND(t, SIFT_SB_PAGES - i),
					x = pages[i];

		pages[i] = pages[j];
		pages[j] = x;
	}
	qsort(pages, npages, sizeof(uint32), cmp_u32);
	for (uint32 p = 0; p < npages; p++)
	{
		uint32		cnt = t->full ? MaxHeapTuplesPerPage : Min(MaxHeapTuplesPerPage, 1 + RAND(t, per * 2)),
					off = 0;
		bool		sparse = !t->full && RAND(t, 2);

		for (uint32 i = 0; i < cnt; i++)
		{
			off += sparse ? 1 + (RAND(t, 3) ? 0 : RAND(t, 40)) : 1;
			if (off > MaxHeapTuplesPerPage)
				break;
			keys[n++] = ((uint64) (sb * SIFT_SB_PAGES + pages[p]) << 16) | off;
		}
	}
	*nout = n;
	return keys;
}

static int64
gen_int(TCtx *t, const LeafProf *p, uint32 seq)
{
	switch (p->mode)
	{
		case 0:
			return p->base;
		case 1:
			return p->bits == 0 ? p->base : (int64) ((uint64) p->base + (p->bits >= 64 ? pg_prng_uint64(&t->rng) : pg_prng_uint64(&t->rng) % (UINT64CONST(1) << p->bits)));
		case 2:
			return RAND(t, 50) == 0 ? (RAND(t, 2) ? PG_INT64_MIN : PG_INT64_MAX) : (int64) pg_prng_uint64(&t->rng) >> RAND(t, 64);
		default:
			return p->base / 2 + (int64) (seq / p->step);
	}
}

static double
gen_float(TCtx *t, const LeafProf *p, bool f4)
{
	static const double special[] = {0.0, -0.0, 1.0, -1.0, 0.5, 1e30, -1e30, 3.4028234663852886e38, 1e-45, 1e-300, HUGE_VAL, -HUGE_VAL};
	double		d;

	switch (p->mode)
	{
		case 0:
			d = RAND(t, 12) == 0 ? (double) NAN : special[RAND(t, lengthof(special))];
			break;
		case 1:
			d = (double) ((int) RAND(t, 2001) - 1000);
			break;
		default:
			d = (pg_prng_double(&t->rng) - 0.5) * pow(10.0, (double) RAND(t, 40) - 20);
	}
	return f4 ? (double) (float4) d : d;
}

static void
gen_value(TCtx *t, const LeafProf *p, SiftColStore store, uint32 seq, SiftColValue *v)
{
	memset(v, 0, sizeof(*v));
	if (CHANCE(t, p->pnull))
	{
		v->isnull = true;
		return;
	}
	if (CHANCE(t, p->povf))
	{
		v->isnull = v->overflow = true;
		return;
	}
	switch (store)
	{
		case SIFT_COLS_INT:
			v->v.i = gen_int(t, p, seq);
			break;
		case SIFT_COLS_STR:
			{
				uint32		k = p->seqpick ? seq % p->npool : RAND(t, p->npool);

				v->v.s = p->pool[k];
				v->len = p->plen[k];
			}
			break;
		default:
			v->v.f = gen_float(t, p, store == SIFT_COLS_F4);
	}
}

/* A row for the given key; seq numbers the row (clustered columns grow with it) */
static void
gen_row(TCtx *t, const Case *c, SiftKey key, uint32 seq, SiftColRow *row)
{
	const SiftColSchema *s = c->schema;

	memset(row, 0, sizeof(*row));
	sift_key_to_tid(key, &row->tid);
	row->ncols = s->ncols;
	row->val = palloc0(s->ncols * sizeof(SiftColValue));
	row->elem = palloc0((s->ncols + 1) * sizeof(SiftColValue *));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
			continue;
		if (d->kind == SIFT_COLK_GROUP)
		{
			SiftColValue *v = &row->val[id];

			if (CHANCE(t, c->prof[id].pnull))
				v->isnull = true;
			else if (CHANCE(t, c->prof[id].povf))
				v->isnull = v->overflow = true;
			else
				v->v.i = RAND(t, 4) == 0 ? 0 : RAND(t, 6);
			for (int ch = id + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == id && !v->isnull)
				{
					row->elem[ch] = palloc((v->v.i + 1) * sizeof(SiftColValue));
					for (int64 j = 0; j < v->v.i; j++)
						gen_value(t, &c->prof[ch], s->cols[ch].store, seq * 8 + j, &row->elem[ch][j]);
				}
		}
		else
			gen_value(t, &c->prof[id], d->store, seq, &row->val[id]);
	}
}

static SiftColRow *
gen_rows(TCtx *t, const Case *c, const SiftKey *keys, uint32 n)
{
	SiftColRow *rows = palloc(Max(n, 1) * sizeof(SiftColRow));

	for (uint32 i = 0; i < n; i++)
		gen_row(t, c, keys[i], i, &rows[i]);
	return rows;
}

static SiftColBatch *
batch_of(const Case *c, const SiftColRow *rows, uint32 n)
{
	SiftColBatch *b = sift_cbatch_create(CurrentMemoryContext, c->schema);

	sift_cbatch_reset(b, c->sb);
	for (uint32 i = 0; i < n; i++)
		sift_cbatch_add(b, &rows[i]);
	return b;
}

/* ---------------------------------------------------------------------------
 * Comparing rows
 * ------------------------------------------------------------------------- */

/* Two stored values are equal: the same NULL/overflow state, the same key (floats: -0 and NaN are one value) or bytes */
static bool
value_eq(SiftColStore store, const SiftColValue *a, const SiftColValue *b)
{
	if ((a->isnull || a->overflow) != (b->isnull || b->overflow) || (a->overflow != b->overflow))
		return false;
	if (a->isnull || a->overflow)
		return true;
	switch (store)
	{
		case SIFT_COLS_INT:
			return a->v.i == b->v.i;
		case SIFT_COLS_STR:
			return a->len == b->len && memcmp(a->v.s, b->v.s, a->len) == 0;
		default:
			return sift_col_f8_key(a->v.f) == sift_col_f8_key(b->v.f);
	}
}

static bool
row_eq(const SiftColSchema *s, const SiftColRow *a, const SiftColRow *b)
{
	if (!ItemPointerEquals((ItemPointer) &a->tid, (ItemPointer) &b->tid))
		return false;
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
			continue;
		if (!value_eq(d->store, &a->val[id], &b->val[id]))
			return false;
		if (d->kind == SIFT_COLK_GROUP && !a->val[id].isnull && !a->val[id].overflow)
			for (int ch = id + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == id)
					for (int64 j = 0; j < a->val[id].v.i; j++)
						if (!value_eq(s->cols[ch].store, &a->elem[ch][j], &b->elem[ch][j]))
							return false;
	}
	return true;
}

/* The batch holds exactly the rows (in order) */
static void
check_batch_rows(TCtx *t, const char *what, const Case *c, const SiftColBatch *b, const SiftColRow *rows, uint32 n)
{
	SiftColRow	got = {0};
	uint32		bad = 0;

	CHECK(t, b->nrows == n, "%s: %u rows, expected %u", what, b->nrows, n);
	for (uint32 r = 0; r < Min(n, b->nrows); r++)
	{
		sift_cbatch_row(b, r, &got);
		bad += !row_eq(c->schema, &got, &rows[r]);
	}
	CHECK(t, bad == 0, "%s: %u rows differ", what, bad);
	sift_cbatch_row_free(&got);
}

/* ---------------------------------------------------------------------------
 * Schemas and dictionaries
 * ------------------------------------------------------------------------- */

/* Runs fn and returns the SQLSTATE it raised (0: none) */
#define ERRCODE_OF(stmt) \
	({ \
		int			code_ = 0; \
		MemoryContext old_ = CurrentMemoryContext; \
		PG_TRY(); { stmt; } \
		PG_CATCH(); \
		{ \
			ErrorData *e_; \
			MemoryContextSwitchTo(old_); \
			e_ = CopyErrorData(); \
			code_ = e_->sqlerrcode; \
			FlushErrorState(); \
		} \
		PG_END_TRY(); \
		code_; \
	})

static void
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
			  p->cols[i].store == s->cols[i].store && p->cols[i].attno == s->cols[i].attno && p->cols[i].typid == s->cols[i].typid &&
			  strcmp(p->cols[i].path, s->cols[i].path) == 0, "schema leaf %d", i);
	for (Size cut = 0; cut < buf.len; cut += 1 + RAND(t, 9))
		CHECK(t, sift_cschema_parse(CurrentMemoryContext, buf.data, cut) == NULL, "truncated schema (%zu bytes) parsed", cut);
	buf.data[8 + RAND(t, 4)] ^= 1;	/* the hash */
	CHECK(t, sift_cschema_parse(CurrentMemoryContext, buf.data, buf.len) == NULL, "schema with a wrong hash parsed");

	/* the hash covers everything but the flags */
	defs[0] = (SiftColDef) {0, -1, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 1, INT8OID, "total"};
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

static void
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
		uint32		l = RAND(t, 6) == 0 ? RAND(t, SIFT_COL_MAX_STR + 1) : RAND(t, 6);
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
	CHECK(t, ERRCODE_OF(sift_cdict_intern(d, str[0], SIFT_COL_MAX_STR + 1)) == ERRCODE_PROGRAM_LIMIT_EXCEEDED, "256 byte value accepted");

	initStringInfo(&buf);
	appendStringInfoString(&buf, "pad");
	resetStringInfo(&buf);
	sift_cdict_serialize(d, &buf);
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
	for (Size cut = 0; cut + 1 < (Size) buf.len; cut += 1 + RAND(t, 50))
		CHECK(t, sift_cdict_parse(CurrentMemoryContext, buf.data, cut, &used) == NULL, "truncated dictionary (%zu bytes) parsed", cut);
}

/* ---------------------------------------------------------------------------
 * Reference values of a leaf and the naive evaluation of predicates
 * ------------------------------------------------------------------------- */

typedef struct Ref
{
	SiftColValue *v;
	uint32		n;
} Ref;

/* The values of leaf col over the rows: one per row, or the elements of the rows for a child (a group yields its counts) */
static Ref
ref_leaf(const Case *c, const SiftColRow *rows, uint32 n, int col)
{
	const SiftColDef *d = &c->schema->cols[col];
	Ref			r = {palloc(1 * sizeof(SiftColValue)), 0};
	uint64		total = 0;

	for (uint32 i = 0; i < n; i++)
		total += d->parent < 0 ? 1 : (rows[i].val[d->parent].isnull || rows[i].val[d->parent].overflow) ? 0 : rows[i].val[d->parent].v.i;
	r.v = palloc((total + 1) * sizeof(SiftColValue));
	for (uint32 i = 0; i < n; i++)
	{
		if (d->parent < 0)
			r.v[r.n++] = rows[i].val[col];
		else if (!rows[i].val[d->parent].isnull && !rows[i].val[d->parent].overflow)
			for (int64 j = 0; j < rows[i].val[d->parent].v.i; j++)
				r.v[r.n++] = rows[i].elem[col][j];
	}
	return r;
}

static inline bool
ref_null(const SiftColValue *v)
{
	return v->isnull || v->overflow;
}

static int
fcmp(double a, double b)
{
	if (isnan(a))
		return isnan(b) ? 0 : 1;
	if (isnan(b))
		return -1;
	return (a > b) - (a < b);
}

/* PostgreSQL's order of two non-NULL values of the storage class (floats: NaN above everything, equal to itself; text: bytewise) */
static int
vcmp(SiftColStore store, const SiftColValue *a, const SiftColValue *b)
{
	int			r;

	switch (store)
	{
		case SIFT_COLS_INT:
			return (a->v.i > b->v.i) - (a->v.i < b->v.i);
		case SIFT_COLS_STR:
			r = memcmp(a->v.s, b->v.s, Min(a->len, b->len));
			return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
		default:
			return fcmp(a->v.f, b->v.f);
	}
}

/* Does the non-NULL value satisfy the operator with the constants */
static bool
naive_match(SiftColStore store, SiftColOp op, const SiftColValue *cs, int nc, const SiftColValue *v)
{
	bool		any = false;

	for (int i = 0; i < nc && (op == SIFT_COLOP_IN || op == SIFT_COLOP_NOTIN); i++)
		any |= vcmp(store, v, &cs[i]) == 0;
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return vcmp(store, v, &cs[0]) == 0;
		case SIFT_COLOP_NE:
			return vcmp(store, v, &cs[0]) != 0;
		case SIFT_COLOP_LT:
			return vcmp(store, v, &cs[0]) < 0;
		case SIFT_COLOP_LE:
			return vcmp(store, v, &cs[0]) <= 0;
		case SIFT_COLOP_GT:
			return vcmp(store, v, &cs[0]) > 0;
		case SIFT_COLOP_GE:
			return vcmp(store, v, &cs[0]) >= 0;
		case SIFT_COLOP_IN:
			return any;
		case SIFT_COLOP_NOTIN:
			return !any;
		case SIFT_COLOP_NOTNULL:
			return true;
		case SIFT_COLOP_ISNULL:
			return false;
		default:					/* PREFIX */
			return v->len >= cs[0].len && memcmp(v->v.s, cs[0].v.s, cs[0].len) == 0;
	}
}

static bool
naive_row(SiftColStore store, SiftColOp op, const SiftColValue *cs, int nc, const SiftColValue *v)
{
	return ref_null(v) ? op == SIFT_COLOP_ISNULL : naive_match(store, op, cs, nc, v);
}

/* A constant for a predicate on the leaf: a value it holds (sometimes nudged), or an unrelated one */
static void
gen_const(TCtx *t, const SiftColDef *d, const Ref *ref, SiftColValue *c)
{
	const SiftColValue *from = NULL;

	memset(c, 0, sizeof(*c));
	if (ref->n > 0 && RAND(t, 4) != 0)
	{
		const SiftColValue *x = &ref->v[RAND(t, ref->n)];

		from = ref_null(x) ? NULL : x;
	}
	switch (d->store)
	{
		case SIFT_COLS_INT:
			c->v.i = from ? from->v.i + (RAND(t, 4) == 0 ? (int64) RAND(t, 3) - 1 : 0) : (int64) pg_prng_uint64(&t->rng) >> RAND(t, 64);
			break;
		case SIFT_COLS_STR:
			{
				uint32		len = from ? from->len : RAND(t, 5);
				char	   *s = palloc(len + 2);

				if (from)
					memcpy(s, from->v.s, len);
				else
					for (uint32 i = 0; i < len; i++)
						s[i] = "abz\xFF"[RAND(t, 4)];
				switch (from ? RAND(t, 4) : 3)
				{
					case 0:		/* a prefix */
						len = RAND(t, len + 1);
						break;
					case 1:		/* one byte more */
						s[len++] = "az"[RAND(t, 2)];
						break;
					default:
						break;
				}
				c->v.s = s;
				c->len = len;
			}
			break;
		default:
			if (from)
			{
				c->v.f = from->v.f;
				if (RAND(t, 3) == 0 && isfinite(c->v.f))
					c->v.f = RAND(t, 2) ? nextafter(c->v.f, HUGE_VAL) : nextafter(c->v.f, -HUGE_VAL);
			}
			else
			{
				static const double sp[] = {0.0, -0.0, 1.0, HUGE_VAL, -HUGE_VAL, 1e300, -1e300};

				c->v.f = RAND(t, 8) == 0 ? (double) NAN : RAND(t, 2) ? sp[RAND(t, lengthof(sp))] : (pg_prng_double(&t->rng) - 0.5) * 2000;
			}
	}
}

static void
gen_pred(TCtx *t, const SiftColDef *d, const Ref *ref, SiftColPred *p, SiftColValue *consts, int maxconsts)
{
	static const SiftColOp num[] = {SIFT_COLOP_EQ, SIFT_COLOP_NE, SIFT_COLOP_LT, SIFT_COLOP_LE, SIFT_COLOP_GT, SIFT_COLOP_GE,
		SIFT_COLOP_IN, SIFT_COLOP_NOTIN, SIFT_COLOP_ISNULL, SIFT_COLOP_NOTNULL, SIFT_COLOP_EQ, SIFT_COLOP_IN, SIFT_COLOP_LT, SIFT_COLOP_GE,
	SIFT_COLOP_PREFIX};
	int			nops = d->store == SIFT_COLS_STR ? lengthof(num) : lengthof(num) - 1;

	p->col = d->id;
	p->op = num[RAND(t, nops)];
	p->nvals = p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? RAND(t, maxconsts + 1) : p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL ? 0 : 1;
	for (int i = 0; i < p->nvals; i++)
		gen_const(t, d, ref, &consts[i]);
	p->vals = consts;
}

/* ---------------------------------------------------------------------------
 * Chunks, zones and predicates of a leaf
 * ------------------------------------------------------------------------- */

static int64
key_of(const SiftColDef *d, SiftCDict *segdict, const SiftColValue *v)
{
	return d->store == SIFT_COLS_INT ? v->v.i : d->store == SIFT_COLS_STR ? sift_cdict_find(segdict, v->v.s, v->len) : sift_col_f8_key(v->v.f);
}

/* The zone of the reference values, computed independently of the encoder (128 bit sums) */
static void
check_zone(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCZone *z)
{
	uint32		nn = 0;
	int64		mn = 0,
				mx = 0;
	__int128	sum = 0;
	double		sumf = 0;
	uint64		set[4] = {0};
	uint32		flags = 0;

	for (uint32 i = 0; i < ref->n; i++)
	{
		int64		k;

		if (ref->v[i].overflow)
			flags |= SIFT_COLZ_OVERFLOW;
		if (ref_null(&ref->v[i]))
			continue;
		k = key_of(d, segdict, &ref->v[i]);
		mn = nn == 0 ? k : Min(mn, k);
		mx = nn == 0 ? k : Max(mx, k);
		nn++;
		sum += k;
		if (d->store == SIFT_COLS_STR)
			set[(k & 255) >> 6] |= UINT64CONST(1) << (k & 63);
		if (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8)
		{
			sumf += ref->v[i].v.f;
			flags |= isnan(ref->v[i].v.f) ? SIFT_COLZ_NAN : 0;
		}
	}
	flags |= nn > 0 && mn == mx ? SIFT_COLZ_CONST : 0;
	if (d->store == SIFT_COLS_INT && (sum > PG_INT64_MAX || sum < PG_INT64_MIN))
		flags |= SIFT_COLZ_NOSUM;
	CHECK(t, z->nvals == ref->n && z->nnull == ref->n - nn, "%s zone count %u/%u, expected %u/%u", what, z->nnull, z->nvals, ref->n - nn, ref->n);
	CHECK(t, z->flags == flags, "%s zone flags %x, expected %x", what, z->flags, flags);
	CHECK(t, nn == 0 || (z->min == mn && z->max == mx), "%s zone range", what);
	CHECK(t, nn > 0 || (z->min == 0 && z->max == 0), "%s zone range of NULLs", what);
	CHECK(t, d->store != SIFT_COLS_INT || (flags & SIFT_COLZ_NOSUM) || z->sum_i == (int64) sum, "%s zone sum", what);
	CHECK(t, (d->store != SIFT_COLS_F4 && d->store != SIFT_COLS_F8) || memcmp(&z->sum_f, &sumf, sizeof(double)) == 0 || (isnan(z->sum_f) && isnan(sumf)),
		  "%s zone float sum", what);
	CHECK(t, d->store != SIFT_COLS_STR || memcmp(z->set, set, sizeof(set)) == 0, "%s zone set", what);
}

#define NPREDS	8

static void
check_tests(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch, const SiftCZone *z)
{
	uint32		n = ref->n,
				nw = SIFT_COL_NWORDS(n);
	uint64	   *bits = palloc(nw * sizeof(uint64)),
			   *want = palloc(nw * sizeof(uint64));
	SiftColValue consts[16];

	for (int k = 0; k < NPREDS; k++)
	{
		SiftColPred p;
		SiftCTest	test;
		bool		any = false,
					masked = RAND(t, 2);

		gen_pred(t, d, ref, &p, consts, 13);
		sift_ctest_make(d, segdict, &p, &test);
		memset(bits, 0, nw * sizeof(uint64));
		memset(want, 0, nw * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
		{
			bool		in = !masked || RAND(t, 2),
						m = naive_row(d->store, p.op, consts, p.nvals, &ref->v[i]);

			any |= m;
			if (in)
				bits[i >> 6] |= UINT64CONST(1) << (i & 63);
			if (in && m)
				want[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
		sift_ctest_run(&test, ch, bits);
		CHECK(t, memcmp(bits, want, nw * sizeof(uint64)) == 0, "%s: op %d (%d constants, kind %d) selects the wrong rows of %u",
			  what, p.op, p.nvals, test.kind, n);
		CHECK(t, !any || sift_ctest_zone(&test, z), "%s: op %d (kind %d) prunes a chunk with matches", what, p.op, test.kind);
	}
}

/* Decodes the chunk (all, and a selection) against the reference keys */
static void
check_decode(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch)
{
	int64	   *got = palloc((ref->n + SIFT_COL_PAD / 8) * sizeof(int64)),
			   *sel = palloc((ref->n + SIFT_COL_PAD / 8) * sizeof(int64));
	uint32	   *idx = palloc((ref->n + 1) * sizeof(uint32));
	uint32		m = 0,
				bad = 0,
				bad_sel;

	CHECK(t, ch->n == ref->n, "%s: chunk of %u values, expected %u", what, ch->n, ref->n);
	if (ch->n != ref->n)
		return;
	sift_cchunk_decode(ch, got);
	for (uint32 i = 0; i < ref->n; i++)
	{
		bad += got[i] != (ref_null(&ref->v[i]) ? 0 : key_of(d, segdict, &ref->v[i]));
		if (RAND(t, 4) == 0)
			idx[m++] = i;
	}
	CHECK(t, bad == 0, "%s: %u values decode wrongly", what, bad);
	sift_cchunk_decode_sel(ch, idx, m, sel);
	bad_sel = 0;
	for (uint32 k = 0; k < m; k++)
		bad_sel += sel[k] != got[idx[k]];
	CHECK(t, bad_sel == 0, "%s: %u selected values decode wrongly", what, bad_sel);
}

/* ---------------------------------------------------------------------------
 * Units
 * ------------------------------------------------------------------------- */

/* bits 0..7: CONST, PACK of up to 7 / 9..15 / 17..31 bits, FOR 8 / 16 / 32 / 64; bit 8: a text leaf with codes of 16 bits or more */
static void
note_encoding(TCtx *t, SiftColStore store, const SiftCChunk *c)
{
	int			bit;

	if (c->enc == SIFT_COLENC_CC)
	{
		t->encodings |= 1 << 9;
		return;
	}
	if (c->enc == SIFT_COLENC_CONST)
		bit = 0;
	else if (c->enc == SIFT_COLENC_PACK)
		bit = c->bits < 8 ? 1 : c->bits < 16 ? 2 : 3;
	else
		bit = 4 + (c->bits == 8 ? 0 : c->bits == 16 ? 1 : c->bits == 32 ? 2 : 3);
	t->encodings |= 1 << bit;
	if (store == SIFT_COLS_STR && c->enc != SIFT_COLENC_CONST && c->bits >= 16)
		t->encodings |= 1 << 8;
}

/* The segment dictionaries of the STR leaves: a few unrelated values first, then the batch's (so that the codes move) */
static SiftCDict **
seg_dicts(TCtx *t, const SiftColBatch *b, uint32 ***dictmap)
{
	int			nc = b->schema->ncols;
	SiftCDict **dicts = palloc0(nc * sizeof(SiftCDict *));

	*dictmap = palloc0(nc * sizeof(uint32 *));
	for (int col = 0; col < nc; col++)
	{
		if (b->dict[col] == NULL)
			continue;
		dicts[col] = sift_cdict_create(CurrentMemoryContext);
		for (uint32 i = RAND(t, 4); i > 0; i--)
			sift_cdict_intern(dicts[col], "\x01\x02seg", 3 + (i % 2));
		(*dictmap)[col] = palloc(Max(b->dict[col]->n, 1) * sizeof(uint32));
		for (uint32 code = 0; code < b->dict[col]->n; code++)
			(*dictmap)[col][code] = sift_cdict_intern(dicts[col], b->dict[col]->bytes + b->dict[col]->off[code], dict_len(b->dict[col], code));
	}
	return dicts;
}

static void
check_directory(TCtx *t, const SiftCUnitView *v, const SiftColBatch *b)
{
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	uint32		bad = 0;

	CHECK(t, v->nrows == b->nrows, "unit of %u rows, expected %u", v->nrows, b->nrows);
	sift_cunit_keys(v, b->sb, keys);
	for (uint32 r = 0; r < b->nrows; r++)
	{
		uint32		p = sift_cunit_row_page(v, r);
		uint64		mask[SIFT_OFFMASK_WORDS];
		uint32		off = b->keys[r] & 0xFFFF;

		sift_cunit_page_offmask(v, p, mask);
		bad += keys[r] != b->keys[r] || sift_cunit_page_block(v, b->sb, p) != sift_key_block(b->keys[r]) ||
			!((mask[(off - 1) / 64] >> ((off - 1) % 64)) & 1) || r < v->rowstart[p] || r >= v->rowstart[p + 1];
	}
	CHECK(t, bad == 0, "%u rows of the directory differ", bad);
	for (uint32 p = 0; p < v->npages; p++)
	{
		uint64		mask[SIFT_OFFMASK_WORDS];
		uint32		pop = 0;

		sift_cunit_page_offmask(v, p, mask);
		for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
			pop += sift_pc64(mask[w]);
		CHECK(t, pop == v->rowstart[p + 1] - v->rowstart[p], "offset mask of page %u has %u bits", p, pop);
	}
}

/* Corrupt copies of the record must not crash the parser or read past the length */
static void
check_corrupt(TCtx *t, const uint8 *rec, uint32 len, const SiftColSchema *s)
{
	for (int k = 0; k < 12; k++)
	{
		uint32		cut = len - RAND(t, Min(len - 1, 4096));
		uint8	   *copy = palloc(cut + 8 + SIFT_COL_PAD);
		SiftCUnitView v;

		copy = (uint8 *) TYPEALIGN(8, copy);
		memcpy(copy, rec, cut);
		if (k % 2)
			copy[RAND(t, cut)] ^= (uint8) (1 << RAND(t, 8));
		(void) sift_cunit_parse(copy, cut, s, &v);
		if (!(k % 2) && cut < len)
			CHECK(t, !sift_cunit_parse(copy, cut, s, &v), "truncated unit (%u of %u bytes) parsed", cut, len);
	}
}

/* A policy per leaf: each of the codec's families in turn */
static SiftCcPolicy *
random_policy(TCtx *t, int ncols)
{
	static const char *const specs[] = {"auto", "none", "lz4", "zstd:3", "for+zstd", "dict+lz4", "delta", "rle", "alp"};
	SiftCcPolicy *pol = palloc(ncols * sizeof(SiftCcPolicy));

	for (int i = 0; i < ncols; i++)
		if (sift_cc_policy_parse(specs[RAND(t, lengthof(specs))], &pol[i]) != NULL)
			elog(ERROR, "bad test policy");
	return pol;
}

static void
check_unit(TCtx *t, const Case *c, const SiftColBatch *b, const SiftColRow *rows, uint32 n, const SiftCcPolicy *pol)
{
	const SiftColSchema *s = c->schema;
	uint32	  **dictmap = NULL;
	bool		identity = RAND(t, 4) == 0;		/* the segment's dictionaries are the batch's: no map */
	SiftCDict **dicts = identity ? b->dict : seg_dicts(t, b, &dictmap);
	SiftCZone  *zones = palloc(s->ncols * sizeof(SiftCZone));
	StringInfoData buf;
	Size		start;
	uint8	   *rec;
	SiftCUnitView v;
	SiftColBatch *dst;
	uint64	   *live;
	initStringInfo(&buf);
	appendStringInfoString(&buf, "x");	/* the record starts after a pad: encode aligns it */
	sift_cunit_encode(b, dictmap, pol, &buf, &start, zones);
	{
		static const char zero[SIFT_COL_PAD];

		CHECK(t, buf.maxlen - buf.len >= SIFT_COL_PAD && memcmp(buf.data + buf.len, zero, SIFT_COL_PAD) == 0, "no zero pad after the record");
	}
	rec = palloc(buf.len - start + 8 + SIFT_COL_PAD);
	rec = (uint8 *) TYPEALIGN(8, rec);
	memcpy(rec, buf.data + start, buf.len - start);
	memset(rec + (buf.len - start), 0, SIFT_COL_PAD);
	CHECK(t, start % 8 == 0 && start == 8, "unit starts at %zu", start);
	if (!sift_cunit_parse(rec, buf.len - start, s, &v))
	{
		CHECK(t, false, "unit does not parse");
		return;
	}
	check_directory(t, &v, b);

	for (int col = 0; col < s->ncols; col++)
	{
		Ref			ref = ref_leaf(c, rows, n, col);
		SiftCChunk	ch;
		SiftCZone	z2;
		char		what[64];

		snprintf(what, sizeof(what), "leaf %d (store %d)", col, s->cols[col].store);
		if (!sift_cunit_chunk(&v, col, &ch))
		{
			CHECK(t, false, "%s: no chunk", what);
			continue;
		}
		note_encoding(t, s->cols[col].store, &ch);
		check_decode(t, what, &s->cols[col], dicts[col], &ref, &ch);
		sift_cchunk_zone(&ch, s->cols[col].store, &z2);
		CHECK(t, memcmp(&z2, &zones[col], sizeof(z2)) == 0, "%s: recomputed zone differs from the encoder's", what);
		check_zone(t, what, &s->cols[col], dicts[col], &ref, &zones[col]);
		check_tests(t, what, &s->cols[col], dicts[col], &ref, &ch, &zones[col]);
	}

	dst = sift_cbatch_create(CurrentMemoryContext, s);
	sift_cbatch_reset(dst, b->sb);
	sift_cunit_decode(&v, b->sb, dicts, NULL, dst);
	check_batch_rows(t, "unit decode", c, dst, rows, n);

	live = palloc0(SIFT_COL_NWORDS(n) * sizeof(uint64));
	{
		SiftColRow *kept = palloc(Max(n, 1) * sizeof(SiftColRow));
		uint32		nk = 0;

		for (uint32 r = 0; r < n; r++)
			if (RAND(t, 3) != 0)
			{
				live[r >> 6] |= UINT64CONST(1) << (r & 63);
				kept[nk++] = rows[r];
			}
		sift_cbatch_reset(dst, b->sb);
		sift_cunit_decode(&v, b->sb, dicts, live, dst);
		check_batch_rows(t, "unit decode of live rows", c, dst, kept, nk);
	}
	check_corrupt(t, rec, buf.len - start, s);
}

/* ---------------------------------------------------------------------------
 * Round
 * ------------------------------------------------------------------------- */

static void
check_merge(TCtx *t, const Case *c, const SiftKey *keys, uint32 n)
{
	SiftKey    *ka = palloc(Max(n, 1) * sizeof(SiftKey)),
			   *kb = palloc(Max(n, 1) * sizeof(SiftKey));
	SiftColRow *want = palloc(Max(n, 1) * sizeof(SiftColRow));
	uint32		na = 0,
				nb = 0;
	SiftColBatch *src[2],
			   *dst = sift_cbatch_create(CurrentMemoryContext, c->schema);
	SiftColRow *ra,
			   *rb;

	for (uint32 i = 0; i < n; i++)
	{
		bool		a = RAND(t, 5) < 3,
					b = RAND(t, 5) < 3 || !a;

		if (a)
			ka[na++] = keys[i];
		if (b)
			kb[nb++] = keys[i];
	}
	ra = gen_rows(t, c, ka, na);
	rb = gen_rows(t, c, kb, nb);
	for (uint32 i = 0, ia = 0, ib = 0; i < n; i++)
	{
		if (ia < na && ka[ia] == keys[i])
			want[i] = ra[ia++];
		else
			want[i] = rb[ib];
		if (ib < nb && kb[ib] == keys[i])
			ib++;
	}
	src[0] = batch_of(c, ra, na);
	src[1] = batch_of(c, rb, nb);
	sift_cbatch_reset(dst, c->sb);
	sift_cbatch_merge(dst, src, 2);
	check_batch_rows(t, "merge", c, dst, want, n);
}

/* Rows out of order, outside the superblock, with a bad offset or an oversized string are refused */
static void
check_errors(TCtx *t, const Case *c, const SiftColRow *rows, uint32 n)
{
	SiftColBatch *b = batch_of(c, rows, n);
	SiftColRow	row = rows[n - 1];
	SiftKey		last = sift_key_from_tid((ItemPointer) &rows[n - 1].tid);
	int			str = -1;

	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a repeated TID was accepted");
	row = rows[0];
	CHECK(t, n < 2 || ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a TID out of order was accepted");
	ItemPointerSet(&row.tid, (c->sb + 1) * SIFT_SB_PAGES, 1);
	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a TID of the next superblock was accepted");
	ItemPointerSet(&row.tid, sift_key_block(last), 1);
	row.tid.ip_posid = 0;
	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "offset 0 was accepted");
	CHECK(t, b->nrows == n, "refused rows were counted");

	for (int col = 0; col < c->schema->ncols && str < 0; col++)
		if (c->schema->cols[col].store == SIFT_COLS_STR && c->schema->cols[col].parent < 0)
			str = col;
	if (str >= 0 && (last & 0xFFFF) < MaxHeapTuplesPerPage)
	{
		char		big[SIFT_COL_MAX_STR + 1];

		memset(big, 'x', sizeof(big));
		row = rows[n - 1];
		row.val = palloc(c->schema->ncols * sizeof(SiftColValue));
		memcpy(row.val, rows[n - 1].val, c->schema->ncols * sizeof(SiftColValue));
		row.val[str] = (SiftColValue) {.v.s = big, .len = sizeof(big)};
		ItemPointerSet(&row.tid, sift_key_block(last), (last & 0xFFFF) + 1);
		CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_PROGRAM_LIMIT_EXCEEDED, "a value of 256 bytes was accepted");
		row.val[str].len = SIFT_COL_MAX_STR;
		ItemPointerSet(&row.tid, sift_key_block(last), (last & 0xFFFF) + 1);
		b = batch_of(c, rows, n);
		CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == 0, "a value of 255 bytes was refused");
	}
}

static void
run_round(TCtx *t, uint32 nrows)
{
	Case	   *c = gen_case(t);
	uint32		n;
	SiftKey    *keys = gen_keys(t, c->sb, nrows, &n);
	SiftColRow *rows = gen_rows(t, c, keys, n);
	SiftColBatch *b = batch_of(c, rows, n);
	SiftColBatch *copy = sift_cbatch_create(CurrentMemoryContext, c->schema);

	check_batch_rows(t, "batch", c, b, rows, n);

	sift_cbatch_reset(copy, c->sb);
	for (uint32 r = 0; r < n; r++)
		sift_cbatch_add_from(copy, b, r);
	check_batch_rows(t, "add_from", c, copy, rows, n);

	check_schema(t, c);
	check_dict(t);
	check_unit(t, c, b, rows, n, NULL);
	check_unit(t, c, b, rows, n, random_policy(t, c->schema->ncols));

	sift_cbatch_reset(b, c->sb + 1);
	CHECK(t, b->nrows == 0 && b->col[0].n == 0, "reset leaves rows");
	check_merge(t, c, keys, n);
	check_errors(t, c, rows, n);
}

Datum
sift_test_col_enc_roundtrip(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0),
				nrows = PG_GETARG_INT32(1),
				rounds = PG_GETARG_INT32(2);
	TCtx		t = {0};
	MemoryContext rctx = AllocSetContextCreate(CurrentMemoryContext, "col_enc round", ALLOCSET_DEFAULT_SIZES);
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	pg_prng_seed(&t.rng, (uint64) seed);
	for (int i = 0; i < rounds; i++)
	{
		MemoryContext old = MemoryContextSwitchTo(rctx);

		t.full = i == 0 && nrows >= 40000;
		t.wide = t.wide || t.full;
		run_round(&t, (uint32) Max(nrows, 1));
		MemoryContextSwitchTo(old);
		MemoryContextReset(rctx);
	}
	values[0] = Int64GetDatum(t.checks);
	values[1] = Int64GetDatum(t.failures);
	values[2] = Int64GetDatum(t.encodings);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* ---------------------------------------------------------------------------
 * Size report: the unit record of a full superblock (256 dense pages of 291 rows) for typical shapes
 * ------------------------------------------------------------------------- */

#define SIZE_ROWS	(SIFT_SB_PAGES * MaxHeapTuplesPerPage)

typedef enum SizeShape
{
	SZ_DICT20, SZ_DICT3_NULLS, SZ_DATE_MONTH, SZ_DATE_YEAR, SZ_SMALL_INT, SZ_QTY, SZ_BOOL, SZ_TIMESTAMP, SZ_SEQ_ID, SZ_INT8, SZ_PRICE8,
	SZ_PRICE4, SZ_CONST, SZ_STORE_NULLS, SZ_ITEMS, SZ_NSHAPES
} SizeShape;

static const char *const size_names[] = {
	"status, dictionary of 20 values", "channel, dictionary of 3 values, 10% NULL", "order date, clustered in one month",
	"order date, random in a year", "discount percent, 0..100", "quantity, 1..5", "boolean", "timestamp, random in a day (microseconds)",
	"sequential id", "int8, random", "price, float8 with 2 decimals", "price, float4 with 2 decimals", "constant", "store_id, 0..50, 10% NULL",
	"items[]: 0..5 elements of (sku 200 values, qty 1..5, price float8)"
};

static const char *const size_status[] = {"new", "pending", "paid", "packed", "shipped", "delivered", "returned", "cancelled", "refunded",
	"on_hold", "backordered", "partial", "processing", "queued", "inspection", "fraud_check", "awaiting_pickup", "in_transit", "disputed", "closed"};

static void
size_value(TCtx *t, SizeShape shape, uint32 seq, SiftColValue *v)
{
	static const char *const channel[] = {"web", "store", "phone"};

	memset(v, 0, sizeof(*v));
	switch (shape)
	{
		case SZ_DICT20:
			v->v.s = size_status[RAND(t, 20)];
			break;
		case SZ_DICT3_NULLS:
			v->v.s = channel[RAND(t, 3)];
			break;
		case SZ_DATE_MONTH:
			v->v.i = 19000 + seq / 2400;
			break;
		case SZ_DATE_YEAR:
			v->v.i = 19000 + RAND(t, 365);
			break;
		case SZ_SMALL_INT:
			v->v.i = RAND(t, 101);
			break;
		case SZ_QTY:
			v->v.i = 1 + RAND(t, 5);
			break;
		case SZ_BOOL:
			v->v.i = RAND(t, 2);
			break;
		case SZ_TIMESTAMP:
			v->v.i = INT64CONST(1700000000000000) + (int64) RAND(t, 86400) * 1000000 + RAND(t, 1000000);
			break;
		case SZ_SEQ_ID:
			v->v.i = 1000000 + seq;
			break;
		case SZ_INT8:
			v->v.i = (int64) (pg_prng_uint64(&t->rng) >> 1);
			break;
		case SZ_PRICE8:
			v->v.f = RAND(t, 100000) / 100.0;
			break;
		case SZ_PRICE4:
			v->v.f = (double) (float4) (RAND(t, 100000) / 100.0);
			break;
		case SZ_CONST:
			v->v.i = 42;
			break;
		default:
			v->v.i = RAND(t, 51);
	}
	if (shape == SZ_DICT3_NULLS || shape == SZ_STORE_NULLS)
		v->isnull = RAND(t, 10) == 0;
	if (shape <= SZ_DICT3_NULLS)
		v->len = strlen(v->v.s);
}

static SiftColBatch *
size_batch(TCtx *t, SizeShape shape, SiftColSchema **schema)
{
	SiftColDef	defs[4];
	int			n = 1;
	SiftColStore store = shape <= SZ_DICT3_NULLS ? SIFT_COLS_STR : shape == SZ_PRICE8 ? SIFT_COLS_F8 : shape == SZ_PRICE4 ? SIFT_COLS_F4 : SIFT_COLS_INT;
	SiftColBatch *b;
	SiftColValue val[4],
				elem[3][8];
	SiftColValue *elems[4];
	SiftColRow	row = {.ncols = 1, .val = val, .elem = elems};
	char		skus[200][12];

	defs[0] = (SiftColDef) {0, -1, SIFT_COLK_SCALAR, store, 0, 1, store_type(store), "col"};
	if (shape == SZ_ITEMS)
	{
		defs[0] = (SiftColDef) {0, -1, SIFT_COLK_GROUP, SIFT_COLS_INT, 0, 1, INT4OID, "items[]"};
		defs[1] = (SiftColDef) {1, 0, SIFT_COLK_SCALAR, SIFT_COLS_STR, 0, 1, TEXTOID, "items[].sku"};
		defs[2] = (SiftColDef) {2, 0, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 1, INT4OID, "items[].qty"};
		defs[3] = (SiftColDef) {3, 0, SIFT_COLK_SCALAR, SIFT_COLS_F8, 0, 1, FLOAT8OID, "items[].price"};
		n = 4;
		for (int i = 0; i < 200; i++)
			snprintf(skus[i], sizeof(skus[i]), "SKU-%05d", i * 37);
	}
	*schema = sift_cschema_create(CurrentMemoryContext, defs, n);
	b = sift_cbatch_create(CurrentMemoryContext, *schema);
	sift_cbatch_reset(b, 7);
	row.ncols = n;
	memset(elems, 0, sizeof(elems));
	for (uint32 i = 0; i < SIZE_ROWS; i++)
	{
		sift_key_to_tid(((uint64) (7 * SIFT_SB_PAGES + i / MaxHeapTuplesPerPage) << 16) | (1 + i % MaxHeapTuplesPerPage), &row.tid);
		if (shape == SZ_ITEMS)
		{
			memset(val, 0, sizeof(val));
			val[0].v.i = RAND(t, 6);
			for (int k = 0; k < val[0].v.i; k++)
			{
				const char *s = skus[RAND(t, 200)];

				memset(&elem[0][k], 0, sizeof(SiftColValue));
				memset(&elem[1][k], 0, sizeof(SiftColValue));
				memset(&elem[2][k], 0, sizeof(SiftColValue));
				elem[0][k].v.s = s;
				elem[0][k].len = strlen(s);
				elem[1][k].v.i = 1 + RAND(t, 5);
				elem[2][k].v.f = RAND(t, 100000) / 100.0;
			}
			elems[1] = elem[0];
			elems[2] = elem[1];
			elems[3] = elem[2];
		}
		else
			size_value(t, shape, i, &val[0]);
		sift_cbatch_add(b, &row);
	}
	return b;
}

PG_FUNCTION_INFO_V1(sift_test_col_enc_sizes);

/* (shape text, rows int8, bytes int8, bytes_per_row float8): the encoded unit record of each shape in a full superblock */
Datum
sift_test_col_enc_sizes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TCtx		t = {0};

	InitMaterializedSRF(fcinfo, 0);
	pg_prng_seed(&t.rng, 42);
	for (int shape = 0; shape < SZ_NSHAPES; shape++)
	{
		MemoryContext sctx = AllocSetContextCreate(CurrentMemoryContext, "col_enc size", ALLOCSET_DEFAULT_SIZES);
		MemoryContext old = MemoryContextSwitchTo(sctx);
		SiftColSchema *schema;
		SiftColBatch *b = size_batch(&t, (SizeShape) shape, &schema);
		StringInfoData buf;
		Size		start;
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		initStringInfo(&buf);
		sift_cunit_encode(b, NULL, NULL, &buf, &start, NULL);
		MemoryContextSwitchTo(old);
		values[0] = CStringGetTextDatum(size_names[shape]);
		values[1] = Int64GetDatum(b->nrows);
		values[2] = Int64GetDatum((int64) (buf.len - start));
		values[3] = Float8GetDatum((double) (buf.len - start) / b->nrows);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		MemoryContextDelete(sctx);
	}
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
