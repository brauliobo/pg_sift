/*
 * agg/star/exec.c - the tables of the small relations of a star join and their probes by the batches of the fact table (agg/star/star.h).
 */
/* check-memory: allow-allocs every array here follows the rows of one small relation, which sift_star_dim_add stops at the budget of its shape and at one allocation */
#include "core/host.h"

#include <math.h>

#include "agg/star/dense.h"
#include "agg/vexpr/str.h"

static uint64
mix64(uint64 x)
{
	x += UINT64CONST(0x9e3779b97f4a7c15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94d049bb133111eb);
	return x ^ (x >> 31);
}

static uint64
hash_bytes64(const char *p, uint32 n)
{
	uint64		h = UINT64CONST(1469598103934665603);

	for (uint32 i = 0; i < n; i++)
		h = (h ^ (uint8) p[i]) * UINT64CONST(1099511628211);
	return mix64(h);
}

static inline bool
bit_at(const uint64 *m, uint32 r)
{
	return m == NULL || ((m[r >> 6] >> (r & 63)) & 1);
}

static inline int
ncols_of(const SiftStarShape *sh)
{
	return sh->nkeys + sh->npay;
}

static inline SiftStarCls
cls_of(const SiftStarShape *sh, int c)
{
	return c < sh->nkeys ? sh->kcls[c] : sh->pcls[c - sh->nkeys];
}

static inline bool
is_range(const SiftStarShape *sh)
{
	return sh->mode == SV6J_RANGE_SMALL;
}

static inline bool
keeps_fact(const SiftStarShape *sh)
{
	return sift_star_keeps_fact(sh->mode) || (is_range(sh) && sh->left);
}

/* an unmatched fact row is one pair with no dimension row: not when a condition of the pairs decides, which also needs the matched rows' pairs */
static inline bool
is_left(const SiftStarShape *sh)
{
	return !sh->on && keeps_fact(sh);
}

void
sift_star_dim_init(SiftStarDim *d, const SiftStarShape *shape)
{
	memset(d, 0, sizeof(*d));
	d->shape = *shape;
	d->dense = sift_star_dense_shape(shape);
	d->hull_min = PG_INT64_MAX;
	d->hull_max = PG_INT64_MIN;
}

#define INDEX_ROW_BYTES	(4 + 36)	/* the chain, and a share of the index at its largest */

/* what a slot of the arrays takes, held or not */
static uint64
slot_bytes(const SiftStarShape *sh)
{
	uint64		b = 0;

	for (int c = 0; c < ncols_of(sh); c++)
		b += 9 + (cls_of(sh, c) == SSC_STR ? 4 : 0);
	return b + (is_range(sh) ? 17 : 0);
}

/* what the table takes when it holds nrows rows in arrays of cap slots and an arena of arena_cap bytes, and the index it will have */
static uint64
table_bytes(const SiftStarShape *sh, uint32 cap, uint32 nrows, uint64 arena_cap)
{
	return (uint64) cap * slot_bytes(sh) + arena_cap + (uint64) nrows * INDEX_ROW_BYTES;
}

double
sift_star_table_est(const SiftStarShape *sh, double nrows, double str_bytes)
{
	const double cap = exp2(ceil(log2(Max(nrows, 64.0))));
	const double arena = str_bytes > 0 ? exp2(ceil(log2(Max(nrows * str_bytes, 1024.0)))) : 0;

	return cap * slot_bytes(sh) + arena + nrows * INDEX_ROW_BYTES;
}

double
sift_star_dense_est(const SiftStarShape *sh, double nrows, double span, double str_bytes)
{
	const double cap = Max(span * 1.5, 64.0);
	const double dense = (double) sift_star_dense_bytes(sh, (uint64) cap, 0) + (str_bytes > 0 ? exp2(ceil(log2(Max(nrows * str_bytes, 1024.0)))) : 0);

	return sift_star_dense_shape(sh) && cap <= PG_UINT32_MAX / 2 && dense <= (double) Min(sh->budget, MaxAllocSize) ? dense : -1;
}

static void
grow(SiftStarDim *d)
{
	const SiftStarShape *sh = &d->shape;
	const uint32 cap = d->cap == 0 ? 64 : d->cap * 2;

	for (int c = 0; c < ncols_of(sh); c++)
	{
		d->col[c] = d->col[c] == NULL ? palloc(cap * sizeof(uint64)) : repalloc(d->col[c], cap * sizeof(uint64));
		d->null[c] = d->null[c] == NULL ? palloc(cap) : repalloc(d->null[c], cap);
		if (cls_of(sh, c) == SSC_STR)
			d->len[c] = d->len[c] == NULL ? palloc(cap * sizeof(uint32)) : repalloc(d->len[c], cap * sizeof(uint32));
	}
	if (is_range(sh))
	{
		d->lo = d->lo == NULL ? palloc(cap * sizeof(int64)) : repalloc(d->lo, cap * sizeof(int64));
		d->hi = d->hi == NULL ? palloc(cap * sizeof(int64)) : repalloc(d->hi, cap * sizeof(int64));
		d->period_ok = d->period_ok == NULL ? palloc(cap) : repalloc(d->period_ok, cap);
	}
	d->cap = cap;
}

static uint64
arena_put(SiftStarDim *d, const char *p, uint32 len)
{
	const uint64 off = TYPEALIGN(MAXIMUM_ALIGNOF, d->arena_used);
	const uint64 cap = sift_star_arena_grown(d, len);

	if (cap != d->arena_cap)
	{
		d->arena = d->arena == NULL ? palloc(cap) : repalloc(d->arena, cap);
		d->arena_cap = cap;
	}
	memcpy(d->arena + off, p, len);
	d->arena_used = off + len;
	return off;
}

static void
put_cell(SiftStarDim *d, int c, uint32 i, int64 v, const char *p, uint32 len, bool isnull)
{
	d->null[c][i] = isnull;
	if (cls_of(&d->shape, c) == SSC_STR)
	{
		d->col[c][i] = isnull ? 0 : arena_put(d, p, len);
		d->len[c][i] = isnull ? 0 : len;
	}
	else
		d->col[c][i] = isnull ? 0 : (uint64) v;
}

static bool
hash_add(SiftStarDim *d, const SiftStarRow *row)
{
	const SiftStarShape *sh = &d->shape;
	uint64		strs = 0;

	if (is_range(sh) && d->nrows >= SIFT_STAR_RANGE_MAX)
		return false;
	for (int k = 0; k < sh->nkeys; k++)
		strs += sh->kcls[k] == SSC_STR && !row->key_null[k] ? row->key_len[k] : 0;
	for (int c = 0; c < sh->npay; c++)
		strs += sh->pcls[c] == SSC_STR && !row->pay_null[c] ? row->pay_len[c] : 0;
	{
		const uint32 cap = d->nrows < d->cap ? d->cap : d->cap == 0 ? 64 : d->cap * 2;
		const uint64 need = TYPEALIGN(MAXIMUM_ALIGNOF, d->arena_used) + strs + (uint64) ncols_of(sh) * MAXIMUM_ALIGNOF;
		const uint64 arena = need <= d->arena_cap ? d->arena_cap : Max(d->arena_cap * 2, Max(need, 1024));

		if (table_bytes(sh, cap, d->nrows + 1, arena) > Min(sh->budget, MaxAllocSize))
			return false;
	}
	if (d->nrows == d->cap)
		grow(d);
	for (int k = 0; k < sh->nkeys; k++)
		put_cell(d, k, d->nrows, row->key_i[k], row->key_p[k], row->key_len[k], row->key_null[k]);
	for (int c = 0; c < sh->npay; c++)
		put_cell(d, sh->nkeys + c, d->nrows, row->pay_i[c], row->pay_p[c], row->pay_len[c], row->pay_null[c]);
	if (is_range(sh))
	{
		const bool	ok = !row->lo_null && !row->hi_null;

		d->lo[d->nrows] = row->lo;
		d->hi[d->nrows] = row->hi;
		d->period_ok[d->nrows] = ok;
		if (ok)
		{
			d->hull_min = Min(d->hull_min, row->lo);
			d->hull_max = Max(d->hull_max, row->hi);
		}
	}
	else if (sh->nkeys > 0 && sh->kcls[0] == SSC_INT && !row->key_null[0])
	{
		d->hull_min = Min(d->hull_min, row->key_i[0]);
		d->hull_max = Max(d->hull_max, row->key_i[0]);
	}
	d->nrows++;
	d->bytes = table_bytes(sh, d->cap, d->nrows, d->arena_cap);
	return true;
}

/* The rows of a dense table into the hash form; false, the table left as it was, when they do not fit it */
static bool
leave_dense(SiftStarDim *d)
{
	SiftStarDim h;

	sift_star_dim_init(&h, &d->shape);
	h.dense = false;
	for (uint32 s = 0; s < d->dcap; s++)
	{
		SiftStarRow row;

		if (!sift_star_dense_has(d, s))
			continue;
		sift_star_dense_take(d, s, &row);
		if (!hash_add(&h, &row))
		{
			sift_star_dim_free(&h);
			return false;
		}
	}
	sift_star_dim_free(d);
	*d = h;
	return true;
}

bool
sift_star_dim_add(SiftStarDim *d, const SiftStarRow *row)
{
	if (d->dense && sift_star_dense_reserve(d, row))
	{
		SiftStarRow r = *row;

		for (int c = 0; c < d->shape.npay; c++)
			if (d->shape.pcls[c] == SSC_STR && !r.pay_null[c])
				r.pay_i[c] = (int64) arena_put(d, r.pay_p[c], r.pay_len[c]);
		sift_star_dense_put(d, &r);
		return true;
	}
	if (d->dense && !leave_dense(d))
		return false;
	return hash_add(d, row);
}

static bool
row_has_null_key(const SiftStarDim *d, uint32 i)
{
	if (d->dense)
		return false;
	for (int k = 0; k < d->shape.nkeys; k++)
		if (d->null[k][i])
			return true;
	return false;
}

static uint64
row_hash(const SiftStarDim *d, uint32 i)
{
	uint64		h = 0;

	if (d->dense)
		return mix64(mix64((uint64) (d->dbase + (int64) i)));
	for (int k = 0; k < d->shape.nkeys; k++)
		h = mix64(h * 31 + (d->shape.kcls[k] == SSC_STR ? hash_bytes64(d->arena + d->col[k][i], d->len[k][i]) : mix64(d->col[k][i])));
	return h;
}

static bool
rows_equal(const SiftStarDim *d, uint32 a, uint32 b)
{
	for (int k = 0; k < d->shape.nkeys; k++)
	{
		if (d->shape.kcls[k] == SSC_STR)
		{
			if (d->len[k][a] != d->len[k][b] || memcmp(d->arena + d->col[k][a], d->arena + d->col[k][b], d->len[k][a]) != 0)
				return false;
		}
		else if (d->col[k][a] != d->col[k][b])
			return false;
	}
	return true;
}

static int
i64_cmp(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

typedef struct Period
{
	int64		lo;
	int64		hi;
	uint32		row;
} Period;

static int
period_cmp(const void *a, const void *b)
{
	const Period *x = a,
			   *y = b;

	if (x->lo != y->lo)
		return x->lo < y->lo ? -1 : 1;
	if (x->hi != y->hi)
		return x->hi < y->hi ? -1 : 1;
	return (x->row > y->row) - (x->row < y->row);
}

/* the periods that are not NULL sorted by their lo, with a segment tree of the largest hi over them: a point is looked up by a binary search and a descent (sift_star_range_hits) */
static void
range_index(SiftStarDim *d)
{
	Period	   *ps = palloc(Max(d->nrows, 1) * sizeof(Period));

	d->nrng = 0;
	for (uint32 i = 0; i < d->nrows; i++)
		if (d->period_ok[i])
			ps[d->nrng++] = (Period) {d->lo[i], d->hi[i], i};
	qsort(ps, d->nrng, sizeof(Period), period_cmp);
	for (d->rng_size = 1; d->rng_size < d->nrng; d->rng_size *= 2)
		;
	d->rng_ord = palloc(Max(d->nrng, 1) * sizeof(uint32));
	d->rng_lo = palloc(Max(d->nrng, 1) * sizeof(int64));
	d->rng_hi = palloc(Max(d->nrng, 1) * sizeof(int64));
	d->rng_max = palloc(2 * d->rng_size * sizeof(int64));
	d->rng_disjoint = true;
	for (uint32 i = 0; i < d->rng_size; i++)
		d->rng_max[d->rng_size + i] = i < d->nrng ? ps[i].hi : PG_INT64_MIN;
	for (uint32 i = 0; i < d->nrng; i++)
	{
		d->rng_ord[i] = ps[i].row;
		d->rng_lo[i] = ps[i].lo;
		d->rng_hi[i] = ps[i].hi;
		d->rng_disjoint &= i + 1 == d->nrng || ps[i].hi < ps[i + 1].lo;
	}
	for (uint32 i = d->rng_size; i-- > 1;)
		d->rng_max[i] = Max(d->rng_max[2 * i], d->rng_max[2 * i + 1]);
	pfree(ps);
}

void
sift_star_dim_finish(SiftStarDim *d)
{
	uint32		inserted = 0;

	if (d->dense)
	{
		sift_star_dense_finish(d);
		return;
	}
	if (is_range(&d->shape))
	{
		range_index(d);
		d->unique_keys = false;
		d->finished = true;
		return;
	}
	d->ht_cap = 16;
	while (d->ht_cap < 2 * d->nrows)
		d->ht_cap *= 2;
	d->head = palloc0(d->ht_cap * sizeof(uint32));
	d->tail = palloc0(d->ht_cap * sizeof(uint32));
	d->next = palloc0(Max(d->nrows, 1) * sizeof(uint32));
	for (uint32 i = 0; i < d->nrows; i++)
	{
		uint32		slot;

		if ((i & 1023) == 0)
			CHECK_FOR_INTERRUPTS();
		if (row_has_null_key(d, i))
			continue;
		inserted++;
		for (slot = (uint32) row_hash(d, i) & (d->ht_cap - 1); d->head[slot] != 0; slot = (slot + 1) & (d->ht_cap - 1))
			if (rows_equal(d, d->head[slot] - 1, i))
				break;
		if (d->head[slot] == 0)
		{
			d->head[slot] = d->tail[slot] = i + 1;
			d->ndistinct++;
		}
		else
		{
			d->next[d->tail[slot] - 1] = i + 1;
			d->tail[slot] = i + 1;
		}
	}
	d->unique_keys = d->ndistinct == inserted;
	if (sift_star_preserves_dim(d->shape.mode))
		d->matched = palloc0(((size_t) d->nrows + 63) / 64 * sizeof(uint64));
	if (d->shape.nkeys > 0 && d->shape.kcls[0] == SSC_INT && d->nrows <= SIFT_STAR_EXACT_MAX)
	{
		d->exact = palloc(Max(d->nrows, 1) * sizeof(int64));
		for (uint32 i = 0; i < d->nrows; i++)
			if (!d->null[0][i])
				d->exact[d->nexact++] = (int64) d->col[0][i];
		qsort(d->exact, d->nexact, sizeof(int64), i64_cmp);
		{
			uint32		m = 0;

			for (uint32 i = 0; i < d->nexact; i++)
				if (m == 0 || d->exact[i] != d->exact[m - 1])
					d->exact[m++] = d->exact[i];
			d->nexact = m;
		}
	}
	d->finished = true;
}

void
sift_star_dim_free(SiftStarDim *d)
{
	for (int c = 0; c < ncols_of(&d->shape); c++)
	{
		if (d->col[c] != NULL)
			pfree(d->col[c]);
		if (d->null[c] != NULL)
			pfree(d->null[c]);
		if (d->len[c] != NULL)
			pfree(d->len[c]);
	}
	if (d->arena != NULL)
		pfree(d->arena);
	if (d->lo != NULL)
		pfree(d->lo);
	if (d->hi != NULL)
		pfree(d->hi);
	if (d->period_ok != NULL)
		pfree(d->period_ok);
	if (d->rng_ord != NULL)
	{
		pfree(d->rng_ord);
		pfree(d->rng_lo);
		pfree(d->rng_hi);
		pfree(d->rng_max);
	}
	if (d->head != NULL)
		pfree(d->head);
	if (d->tail != NULL)
		pfree(d->tail);
	if (d->next != NULL)
		pfree(d->next);
	if (d->exact != NULL)
		pfree(d->exact);
	if (d->matched != NULL)
		pfree(d->matched);
	sift_star_dense_free(d);
	memset(d, 0, sizeof(*d));
}

/* ---- the probe ---- */

typedef struct Cell
{
	int64		i;
	const char *p;
	uint32		len;
	bool		isnull;
} Cell;

static Cell
probe_cell(const SiftVec *v, SiftStarCls cls, uint32 r)
{
	Cell		c = {0};

	c.isnull = !(v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1));
	if (c.isnull)
		return c;
	switch (v->type)
	{
		case SVEC_STRVIEW:
			c.p = sift_vstr_bytes(&((const SiftStrView *) v->data)[r]);
			c.len = ((const SiftStrView *) v->data)[r].len;
			break;
		case SVEC_I32:
			c.i = ((const int32 *) v->data)[r];
			break;
		case SVEC_BOOL:
			c.i = ((const uint8 *) v->data)[r];
			break;
		default:
			c.i = ((const int64 *) v->data)[r];
			break;
	}
	(void) cls;
	return c;
}

static bool
key_matches(const SiftStarDim *d, uint32 row0, const Cell *k)
{
	for (int c = 0; c < d->shape.nkeys; c++)
	{
		if (d->shape.kcls[c] == SSC_STR)
		{
			if (d->len[c][row0] != k[c].len || memcmp(d->arena + d->col[c][row0], k[c].p, k[c].len) != 0)
				return false;
		}
		else if (d->col[c][row0] != (uint64) k[c].i)
			return false;
	}
	return true;
}

/* the first row of the key of fact row r (1-based), 0 when there is none or the key has a NULL */
static uint32
hash_first(const SiftStarDim *d, const SiftStarProbe *p, uint32 r)
{
	Cell		k[SIFT_STAR_MAX_KEYS];
	uint64		h = 0;

	if (d->dense)
	{
		k[0] = probe_cell(p->key[0], SSC_INT, r);
		return k[0].isnull ? 0 : sift_star_dense_row(d, k[0].i);
	}
	for (int c = 0; c < d->shape.nkeys; c++)
	{
		k[c] = probe_cell(p->key[c], d->shape.kcls[c], r);
		if (k[c].isnull)
			return 0;
		h = mix64(h * 31 + (d->shape.kcls[c] == SSC_STR ? hash_bytes64(k[c].p, k[c].len) : mix64((uint64) k[c].i)));
	}
	for (uint32 slot = (uint32) h & (d->ht_cap - 1); d->head[slot] != 0; slot = (slot + 1) & (d->ht_cap - 1))
		if (key_matches(d, d->head[slot] - 1, k))
			return d->head[slot];
	return 0;
}

static uint32
part_of_hash(uint64 h, uint32 nparts)
{
	return (uint32) (h >> 32) % nparts;
}

uint32
sift_star_row_part(const SiftStarShape *sh, const SiftStarRow *row, uint32 nparts)
{
	uint64		h = 0;

	if (is_range(sh))
		return 0;
	for (int k = 0; k < sh->nkeys; k++)
	{
		if (row->key_null[k])
			return 0;
		h = mix64(h * 31 + (sh->kcls[k] == SSC_STR ? hash_bytes64(row->key_p[k], row->key_len[k]) : mix64((uint64) row->key_i[k])));
	}
	return part_of_hash(h, nparts);
}

/* the part of row i of the table: that sift_star_row_part gave it */
static uint32
row_part(const SiftStarDim *d, uint32 i, uint32 nparts)
{
	return row_has_null_key(d, i) ? 0 : part_of_hash(row_hash(d, i), nparts);
}

bool
sift_star_dim_splits(const SiftStarDim *d, uint32 nparts)
{
	const uint32 n = d->dense ? d->dcap : d->nrows;
	int64		first = -1;

	for (uint32 i = 0; i < n; i++)
	{
		if (d->dense && !sift_star_dense_has(d, i))
			continue;
		if (first < 0)
			first = row_part(d, i, nparts);
		else if (row_part(d, i, nparts) != (uint32) first)
			return true;
	}
	return false;
}

uint32
sift_star_probe_part(const SiftStarShape *sh, const SiftStarProbe *p, uint32 r, uint32 nparts)
{
	uint64		h = 0;

	if (is_range(sh))
		return 0;
	for (int k = 0; k < sh->nkeys; k++)
	{
		const Cell	c = probe_cell(p->key[k], sh->kcls[k], r);

		if (c.isnull)
			return 0;
		h = mix64(h * 31 + (sh->kcls[k] == SSC_STR ? hash_bytes64(c.p, c.len) : mix64((uint64) c.i)));
	}
	return part_of_hash(h, nparts);
}

uint32
sift_star_probe(const SiftStarDim *d, const SiftStarProbe *p, uint32 *match)
{
	uint32		hits = 0;

	for (uint32 r = 0; r < p->n; r++)
	{
		match[r] = bit_at(p->active, r) ? hash_first(d, p, r) : 0;
		hits += match[r] != 0;
	}
	return hits;
}

uint16
sift_star_apply(const SiftStarDim *d, const uint32 *match, const uint64 *gate, uint16 n, uint64 *mask)
{
	const SiftV6JoinMode mode = d->shape.mode;
	uint16		left = 0;

	for (uint16 r = 0; r < n; r++)
	{
		if (!((mask[r >> 6] >> (r & 63)) & 1))
			continue;
		if (!sift_star_keeps_fact(mode))
		{
			const bool	matched = match[r] != 0 && bit_at(gate, r);

			if (matched == (mode == SV6J_ANTI_EQUI))
				mask[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
		}
		left += (mask[r >> 6] >> (r & 63)) & 1;
	}
	return left;
}

void
sift_star_gather_bits(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, uint64 *out, uint64 *valid_out)
{
	memset(valid_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 i = 0; i < n; i++)
	{
		const uint32 row = dim_row[i];
		const bool	ok = row != 0 && !sift_star_pay_null(d, pay, row - 1);

		out[i] = ok ? sift_star_pay_bits(d, pay, row - 1) : 0;
		valid_out[i >> 6] |= (uint64) ok << (i & 63);
	}
}

void
sift_star_gather_str(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, SiftStrView *out, uint64 *valid_out)
{
	memset(valid_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 i = 0; i < n; i++)
	{
		const uint32 row = dim_row[i];
		const bool	ok = row != 0 && !sift_star_pay_null(d, pay, row - 1);

		out[i] = ok ? sift_vstr_make(d->arena + sift_star_pay_bits(d, pay, row - 1), sift_star_pay_len(d, pay, row - 1)) : (SiftStrView) {0};
		valid_out[i >> 6] |= (uint64) ok << (i & 63);
	}
}

void
sift_star_gather_datum(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, uint64 *out, uint64 *valid_out)
{
	memset(valid_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 i = 0; i < n; i++)
	{
		const uint32 row = dim_row[i];
		const bool	ok = row != 0 && !sift_star_pay_null(d, pay, row - 1);

		out[i] = ok ? (uint64) (uintptr_t) (d->arena + sift_star_pay_bits(d, pay, row - 1)) : 0;
		valid_out[i >> 6] |= (uint64) ok << (i & 63);
	}
}

/* ---- the pairs ---- */

static inline bool
lo_holds(const SiftStarDim *d, int64 lo, int64 x)
{
	return x > lo || (d->shape.lo_inc && x == lo);
}

static inline bool
hi_holds(const SiftStarDim *d, int64 hi, int64 x)
{
	return x < hi || (d->shape.hi_inc && x == hi);
}

/* the periods below position p (inclusive) of the tree's span [nl, nr] of the sorted order whose hi holds x, in that order: the descent skips a span whose largest hi does not */
static uint32
stab(const SiftStarDim *d, uint32 node, uint32 nl, uint32 nr, uint32 p, int64 x, uint32 *out, uint32 cap, uint32 n)
{
	uint32		mid;

	if (nl > p || !hi_holds(d, d->rng_max[node], x))
		return n;
	if (nl == nr)
	{
		if (n < cap)
			out[n] = d->rng_ord[nl];
		return n + 1;
	}
	mid = nl + (nr - nl) / 2;
	n = stab(d, 2 * node, nl, mid, p, x, out, cap, n);
	return stab(d, 2 * node + 1, mid + 1, nr, p, x, out, cap, n);
}

static int
row_cmp(const void *a, const void *b)
{
	const uint32 x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

uint32
sift_star_range_hits(const SiftStarDim *d, int64 x, uint32 *out, uint32 cap)
{
	uint32		lo = 0,
				hi = d->nrng,
				n;

	/* the periods whose lo holds x are a prefix of the sorted order */
	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (lo_holds(d, d->rng_lo[mid], x))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return 0;
	if (d->rng_disjoint)
	{
		if (!hi_holds(d, d->rng_hi[lo - 1], x))
			return 0;
		if (cap > 0)
			out[0] = d->rng_ord[lo - 1];
		return 1;
	}
	n = stab(d, 1, 0, d->rng_size - 1, lo - 1, x, out, cap, 0);
	qsort(out, Min(n, cap), sizeof(uint32), row_cmp);
	return n;
}

/* the first period that holds the point of the row (the rows that hold it are kept in the cursor), 1-based; 0 when none */
static uint32
range_first(const SiftStarDim *d, const SiftStarProbe *p, uint32 r, SiftStarCursor *cur)
{
	const Cell	c = probe_cell(p->key[0], SSC_INT, r);

	cur->hit_at = 0;
	cur->nhit = c.isnull ? 0 : sift_star_range_hits(d, c.i, cur->hit, SIFT_STAR_RANGE_MAX);
	return cur->nhit > 0 ? cur->hit[0] + 1 : 0;
}

static uint32
first_match(const SiftStarDim *d, const SiftStarProbe *p, const uint64 *gate, uint32 r, SiftStarCursor *cur)
{
	if (!bit_at(gate, r))
		return 0;
	return is_range(&d->shape) ? range_first(d, p, r, cur) : hash_first(d, p, r);
}

static uint32
next_match(const SiftStarDim *d, uint32 chain, SiftStarCursor *cur)
{
	if (is_range(&d->shape))
		return ++cur->hit_at < cur->nhit ? cur->hit[cur->hit_at] + 1 : 0;
	return d->dense ? 0 : d->next[chain - 1];
}

uint32
sift_star_expand(const SiftStarDim *d, const SiftStarProbe *p, const uint64 *gate, SiftStarCursor *cur, uint16 *fact_idx, uint32 *dim_row, uint32 cap)
{
	uint32		out = 0;

	while (cur->row < p->n && out < cap)
	{
		const uint32 r = cur->row;

		if (!bit_at(p->active, r))
		{
			cur->row++;
			continue;
		}
		if (!cur->in_row)
		{
			cur->chain = first_match(d, p, gate, r, cur);
			cur->in_row = true;
			if (cur->chain == 0)
			{
				if (is_left(&d->shape))
				{
					fact_idx[out] = (uint16) r;
					dim_row[out++] = 0;
				}
				cur->row++;
				cur->in_row = false;
				continue;
			}
		}
		fact_idx[out] = (uint16) r;
		dim_row[out++] = cur->chain;
		cur->chain = next_match(d, cur->chain, cur);
		if (cur->chain == 0)
		{
			cur->row++;
			cur->in_row = false;
		}
	}
	return out;
}

bool
sift_star_cursor_done(const SiftStarCursor *cur, uint16 n)
{
	return cur->row >= n;
}

/* ---- what it proves ---- */

SiftStarPush
sift_star_push(const SiftStarDim *d)
{
	const SiftStarShape *sh = &d->shape;
	SiftStarPush push = {.kind = SSP_NONE};
	bool		any;

	if (keeps_fact(sh) || sh->mode == SV6J_ANTI_EQUI || sh->nkeys == 0)
		return push;
	if (is_range(sh))
	{
		any = d->hull_min <= d->hull_max;
		push.empty = !any;
		if (any)
		{
			push.kind = SSP_RANGE;
			push.lo = d->hull_min;
			push.hi = d->hull_max;
		}
		return push;
	}
	any = d->hull_min <= d->hull_max;
	push.empty = d->ndistinct == 0;
	if (sh->kcls[0] != SSC_INT || !any)
		return push;
	if (d->exact != NULL && d->nexact <= SIFT_STAR_IN_MAX)
	{
		push.kind = SSP_EXACT;
		push.keys = d->exact;
		push.nkeys = d->nexact;
	}
	else
		push.kind = SSP_RANGE;
	push.lo = d->hull_min;
	push.hi = d->hull_max;
	return push;
}
