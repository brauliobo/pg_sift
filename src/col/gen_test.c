/*
 * col/gen_test.c - L1 tests: random schemas, rows and batches, the reference values of a leaf and the naive evaluation of predicates.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "core/cmp.h"
#include "col/enc.h"
#include "col/enc_test_int.h"

static const char *const scalar_paths[] = {"tenant_id", "status", "total", "discount", "channel", "region", "note", "units"};
static const char *const item_paths[] = {"items[].category", "items[].sku", "items[].kind", "items[].qty", "items[].price"};

/* Variable-length leaves are top level scalars */
static SiftColStore
rand_store(TCtx *t, bool top)
{
	static const SiftColStore s[] = {SIFT_COLS_INT, SIFT_COLS_INT, SIFT_COLS_F4, SIFT_COLS_F8, SIFT_COLS_STR, SIFT_COLS_STR, SIFT_COLS_VAR};

	return s[RAND(t, top ? 7 : 6)];
}

uint8
store_type(SiftColStore s)
{
	return s == SIFT_COLS_INT ? SIFT_COLT_INT8 : s == SIFT_COLS_F4 ? SIFT_COLT_FLOAT4 : s == SIFT_COLS_F8 ? SIFT_COLT_FLOAT8 : s == SIFT_COLS_VAR ? SIFT_COLT_BYTEA : SIFT_COLT_TEXT;
}

static void
gen_prof(TCtx *t, LeafProf *p, SiftColStore store)
{
	static const int bitsv[] = {0, 1, 3, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 40, 62, 64};

	memset(p, 0, sizeof(*p));
	p->mode = RAND(t, store == SIFT_COLS_STR || store == SIFT_COLS_VAR ? 1 : store == SIFT_COLS_INT ? 4 : 3);
	p->bits = bitsv[RAND(t, lengthof(bitsv))];
	p->base = (int64) pg_prng_uint64(&t->rng) >> RAND(t, 64);
	p->pnull = RAND(t, 4) == 0 ? 0 : RAND(t, 8) == 0 ? 1.0 : 0.15 * pg_prng_double(&t->rng);
	p->povf = RAND(t, 4) == 0 ? 0.04 : 0;
	p->step = 1 + RAND(t, 1000);
	if (store == SIFT_COLS_STR || store == SIFT_COLS_VAR)
	{
		bool		wide = t->wide && store == SIFT_COLS_STR;
		static const char *const stems[] = {"ship", "shipped", "ship_", "a", "", "zz", "pending", "paid", "paid_late", "\xC3\xA9t\xC3\xA9"};
		uint32		np = wide ? 33000 + RAND(t, 41000) : RAND(t, 3) == 0 ? 1 + RAND(t, 300) : 1 + RAND(t, 20);

		p->npool = np;
		p->seqpick = wide;
		p->pool = palloc(np * sizeof(char *));
		p->plen = palloc(np * sizeof(uint32));
		for (uint32 i = 0; i < np; i++)
		{
			const char *stem = stems[RAND(t, lengthof(stems))];
			uint32		len = wide ? 8 : RAND(t, 40) == 0 ? (store == SIFT_COLS_VAR || RAND(t, 2) ? 300 + RAND(t, 9000) : SIFT_COL_DICT_MAX_ENTRY - RAND(t, 2)) :
				strlen(stem) + RAND(t, 6) * (RAND(t, 3) == 0);
			char	   *s = palloc(len + 1);

			for (uint32 j = 0; j < len; j++)
				s[j] = j < strlen(stem) ? stem[j] : "abc\xFF\x01xyz"[RAND(t, 8)];
			if (wide)
				snprintf(s, len + 1, "w%07u", i);
			s[len] = '\0';
			p->pool[i] = s;
			p->plen[i] = len;
		}
		t->wide &= store != SIFT_COLS_STR;
	}
}

Case *
gen_case(TCtx *t)
{
	Case	   *c = palloc0(sizeof(Case));
	SiftColDef	defs[SIFT_COL_MAX_COLS];
	int			n = 0;
	int			nscalar = 2 + RAND(t, 5);
	int			ngroup = RAND(t, 3);

	for (int i = 0; i < nscalar; i++, n++)
		defs[n] = (SiftColDef) {n, -1, SIFT_COLK_SCALAR, rand_store(t, true), 0, n + 1, 0, scalar_paths[i]};
	for (int g = 0; g < ngroup; g++)
	{
		int			gid = n++;
		int			nchild = 1 + RAND(t, 3);

		defs[gid] = (SiftColDef) {gid, -1, SIFT_COLK_GROUP, SIFT_COLS_INT, 0, gid + 1, 0, "items[]"};
		for (int k = 0; k < nchild; k++, n++)
			defs[n] = (SiftColDef) {n, gid, SIFT_COLK_SCALAR, rand_store(t, false), 0, gid + 1, 0, item_paths[k]};
	}
	for (int i = 0; i < n; i++)
		defs[i].type = defs[i].kind == SIFT_COLK_GROUP ? SIFT_COLT_INT4 : store_type(defs[i].store);
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

/* Ascending TIDs of one superblock: 1..256 pages, each dense (offsets 1..cnt) or sparse (gaps) */
SiftKey *
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
	qsort(pages, npages, sizeof(uint32), sift_cmp_u32);
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

/* The type of an overflow value, from the row number: the random stream of the values stays what it was */
static uint8
tag_of(uint32 seq)
{
	return (uint8) ((seq * 2654435761U >> 9) % (SIFT_COLTAG_OVF_MAX + 1));
}

/* The kind of a slot that does not overflow: a JSON null in a NULL slot, the number or the boolean type of a value (an astext leaf's), mostly none */
static uint8
kind_of(uint32 seq, bool isnull)
{
	uint32		h = seq * 2246822519U >> 11;

	return h % 3 ? SIFT_COLTAG_UNKNOWN : isnull ? SIFT_COLTAG_JNULL : (uint8) (SIFT_COLTAG_NUMBER + h / 3 % 2);
}

static void
gen_value(TCtx *t, const LeafProf *p, SiftColStore store, uint32 seq, SiftColValue *v)
{
	memset(v, 0, sizeof(*v));
	if (CHANCE(t, p->pnull))
	{
		v->isnull = true;
		v->tag = kind_of(seq, true);
		return;
	}
	if (CHANCE(t, p->povf))
	{
		v->isnull = v->overflow = true;
		v->tag = tag_of(seq);
		return;
	}
	v->tag = kind_of(seq, false);
	switch (store)
	{
		case SIFT_COLS_INT:
			v->v.i = gen_int(t, p, seq);
			break;
		case SIFT_COLS_STR:
		case SIFT_COLS_VAR:
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
			{
				v->isnull = v->overflow = true;
				v->tag = tag_of(seq);
			}
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

SiftColRow *
gen_rows(TCtx *t, const Case *c, const SiftKey *keys, uint32 n)
{
	SiftColRow *rows = palloc(Max(n, 1) * sizeof(SiftColRow));

	for (uint32 i = 0; i < n; i++)
		gen_row(t, c, keys[i], i, &rows[i]);
	return rows;
}

SiftColBatch *
batch_of(const Case *c, const SiftColRow *rows, uint32 n)
{
	SiftColBatch *b = sift_cbatch_create(CurrentMemoryContext, c->schema);

	sift_cbatch_reset(b, c->sb);
	for (uint32 i = 0; i < n; i++)
		sift_cbatch_add(b, &rows[i]);
	return b;
}

/* The values of leaf col over the rows: one per row, or the elements of the rows for a child (a group yields its counts) */
Ref
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
		case SIFT_COLS_VAR:
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

bool
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
		case SIFT_COLS_VAR:
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

void
gen_pred(TCtx *t, const SiftColDef *d, const Ref *ref, SiftColPred *p, SiftColValue *consts, int maxconsts)
{
	static const SiftColOp num[] = {SIFT_COLOP_EQ, SIFT_COLOP_NE, SIFT_COLOP_LT, SIFT_COLOP_LE, SIFT_COLOP_GT, SIFT_COLOP_GE,
		SIFT_COLOP_IN, SIFT_COLOP_NOTIN, SIFT_COLOP_ISNULL, SIFT_COLOP_NOTNULL, SIFT_COLOP_EQ, SIFT_COLOP_IN, SIFT_COLOP_LT, SIFT_COLOP_GE,
	SIFT_COLOP_PREFIX};
	int			nops = d->store == SIFT_COLS_STR || d->store == SIFT_COLS_VAR ? lengthof(num) : lengthof(num) - 1;

	p->col = d->id;
	p->op = num[RAND(t, nops)];
	p->nvals = p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? RAND(t, maxconsts + 1) : p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL ? 0 : 1;
	for (int i = 0; i < p->nvals; i++)
		gen_const(t, d, ref, &consts[i]);
	p->vals = consts;
}

#endif							/* SIFT_TESTING */
