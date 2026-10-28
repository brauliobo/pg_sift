/*
 * col/hll.c - the HyperLogLog sketch (col/hll.h)
 */
#include <math.h>

#include "core/host.h"
#include "col/hll.h"
#include "col/runs.h"
#include "core/cmp.h"

#define ENT(reg, rank)	((uint32) (reg) << 6 | (rank))
#define LINEAR_CUTOVER	3.5			/* the estimate by linear counting is used while it is at most this many times the registers: measured, 2.5 is biased +1% at 3 times */

void
sift_hll_init(SiftHll *h, MemoryContext cxt)
{
	memset(h, 0, sizeof(*h));
	h->cxt = cxt;
}

void
sift_hll_free(SiftHll *h)
{
	if (h->e)
		pfree(h->e);
	if (h->r)
		pfree(h->r);
	h->e = NULL;
	h->r = NULL;
	h->n = h->cap = 0;
}

/* the entries sorted, one per register (the highest rank) */
static void
compact(SiftHll *h)
{
	uint32		d = 0;

	sift_sort_u32(h->e, h->n);
	for (uint32 i = 0; i < h->n; i++)
	{
		if (i + 1 < h->n && (h->e[i + 1] >> 6) == (h->e[i] >> 6))
			continue;
		h->e[d++] = h->e[i];
	}
	h->n = d;
}

static void
densify(SiftHll *h)
{
	if (h->r)
		return;
	h->r = MemoryContextAllocZero(h->cxt, SIFT_HLL_M);
	for (uint32 i = 0; i < h->n; i++)
		h->r[h->e[i] >> 6] = Max(h->r[h->e[i] >> 6], h->e[i] & 63);
	if (h->e)
		pfree(h->e);
	h->e = NULL;
	h->n = h->cap = 0;
}

static inline void
put(SiftHll *h, uint32 reg, uint32 rank)
{
	if (h->r)
	{
		h->r[reg] = Max(h->r[reg], rank);
		return;
	}
	if (h->n == h->cap)
	{
		if (h->cap > 0)
			compact(h);
		if (h->n > SIFT_HLL_SPARSE)
		{
			densify(h);
			h->r[reg] = Max(h->r[reg], rank);
			return;
		}
		if (h->n >= h->cap / 2)
		{
			h->cap = h->cap ? Min(h->cap * 2, 2 * SIFT_HLL_SPARSE) : 32;
			h->e = h->e ? repalloc(h->e, h->cap * sizeof(uint32)) : MemoryContextAlloc(h->cxt, h->cap * sizeof(uint32));
		}
	}
	h->e[h->n++] = ENT(reg, rank);
}

void
sift_hll_add(SiftHll *h, uint64 hash)
{
	uint64		w = hash << SIFT_HLL_P;

	put(h, (uint32) (hash >> (64 - SIFT_HLL_P)), w ? __builtin_clzll(w) + 1 : SIFT_HLL_TOP_RANK);
}

void
sift_hll_merge(SiftHll *h, const SiftHll *o)
{
	if (o->r)
	{
		densify(h);
		for (uint32 i = 0; i < SIFT_HLL_M; i++)
			h->r[i] = Max(h->r[i], o->r[i]);
	}
	else
		for (uint32 i = 0; i < o->n; i++)
			put(h, o->e[i] >> 6, o->e[i] & 63);
}

static inline uint32
list_ent(const uint8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16;
}

/* register i of the packed registers p (6 bits each; the last one has no byte after it) */
static inline uint32
reg6(const uint8 *p, uint32 i)
{
	uint32		bit = i * 6,
				w = p[bit / 8];

	if (bit / 8 + 1 < SIFT_HLL_M * 6 / 8)
		w |= p[bit / 8 + 1] << 8;
	return w >> (bit % 8) & 63;
}

void
sift_hll_merge_blob(SiftHll *h, const uint8 *blob)
{
	uint32		n;

	memcpy(&n, blob + 4, sizeof(n));
	if (blob[0] == SIFT_HLL_LIST)
	{
		for (uint32 i = 0; i < n; i++)
		{
			uint32		e = list_ent(blob + SIFT_HLL_HDR + 3 * i);

			put(h, e >> 6, e & 63);
		}
		return;
	}
	densify(h);
	for (uint32 i = 0; i < SIFT_HLL_M; i++)
	{
		h->r[i] = Max(h->r[i], reg6(blob + SIFT_HLL_HDR, i));
	}
}

int64
sift_hll_estimate(SiftHll *h)
{
	uint32		cnt[SIFT_HLL_TOP_RANK + 1] = {0};
	double		sum = 0,
				est;

	if (h->r)
		for (uint32 i = 0; i < SIFT_HLL_M; i++)
			cnt[h->r[i]]++;
	else
	{
		if (h->n > 1)
			compact(h);
		cnt[0] = SIFT_HLL_M - h->n;
		for (uint32 i = 0; i < h->n; i++)
			cnt[h->e[i] & 63]++;
	}
	if (cnt[0] == SIFT_HLL_M)
		return 0;
	for (int k = 0; k <= SIFT_HLL_TOP_RANK; k++)
		sum += cnt[k] * ldexp(1.0, -k);
	est = (0.7213 / (1 + 1.079 / SIFT_HLL_M)) * SIFT_HLL_M * (double) SIFT_HLL_M / sum;
	if (cnt[0] > 0)
	{
		double		lc = SIFT_HLL_M * log((double) SIFT_HLL_M / cnt[0]);

		if (lc <= LINEAR_CUTOVER * SIFT_HLL_M)
			est = lc;
	}
	return (int64) llround(est);
}

bool
sift_hll_encode(SiftHll *h, StringInfo out)
{
	uint32		n = 0;
	uint8		hdr[SIFT_HLL_HDR] = {0, SIFT_HLL_P};

	if (!h->r)
	{
		if (h->n > 1)
			compact(h);
		if (h->n > SIFT_HLL_SPARSE)
			densify(h);
	}
	if (h->r)
		for (uint32 i = 0; i < SIFT_HLL_M; i++)
			n += h->r[i] != 0;
	else
		n = h->n;
	if (n == 0)
		return false;
	hdr[0] = n <= SIFT_HLL_SPARSE ? SIFT_HLL_LIST : SIFT_HLL_DENSE;
	memcpy(hdr + 4, &n, sizeof(n));
	appendBinaryStringInfo(out, (const char *) hdr, sizeof(hdr));
	if (hdr[0] == SIFT_HLL_LIST)
	{
		for (uint32 i = 0; i < SIFT_HLL_M && h->r; i++)
			if (h->r[i])
			{
				uint32		e = ENT(i, h->r[i]);
				uint8		b[3] = {e, e >> 8, e >> 16};

				appendBinaryStringInfo(out, (const char *) b, 3);
			}
		for (uint32 i = 0; !h->r && i < n; i++)
		{
			uint8		b[3] = {h->e[i], h->e[i] >> 8, h->e[i] >> 16};

			appendBinaryStringInfo(out, (const char *) b, 3);
		}
		return true;
	}
	{
		uint8	   *p;

		enlargeStringInfo(out, SIFT_HLL_M * 6 / 8);
		p = (uint8 *) out->data + out->len;
		memset(p, 0, SIFT_HLL_M * 6 / 8);
		for (uint32 i = 0; i < SIFT_HLL_M; i++)
		{
			uint32		bit = i * 6,
						v = (uint32) h->r[i] << (bit % 8);

			p[bit / 8] |= (uint8) v;
			p[bit / 8 + 1] |= (uint8) (v >> 8);
		}
		out->len += SIFT_HLL_M * 6 / 8;
		out->data[out->len] = '\0';
	}
	return true;
}

bool
sift_hll_check(const uint8 *blob, uint32 len)
{
	uint32		n;

	if (len < SIFT_HLL_HDR || blob[1] != SIFT_HLL_P || blob[2] != 0 || blob[3] != 0)
		return false;
	memcpy(&n, blob + 4, sizeof(n));
	if (blob[0] == SIFT_HLL_LIST)
	{
		int64		prev = -1;

		if (n < 1 || n > SIFT_HLL_SPARSE || len != SIFT_HLL_HDR + 3 * n)
			return false;
		for (uint32 i = 0; i < n; i++)
		{
			uint32		e = list_ent(blob + SIFT_HLL_HDR + 3 * i),
						rank = e & 63;

			if (e >> 20 != 0 || (int64) (e >> 6) <= prev || rank < 1 || rank > SIFT_HLL_TOP_RANK)
				return false;
			prev = e >> 6;
		}
		return true;
	}
	if (blob[0] != SIFT_HLL_DENSE || n <= SIFT_HLL_SPARSE || n > SIFT_HLL_M || len != SIFT_HLL_HDR + SIFT_HLL_M * 6 / 8)
		return false;
	{
		uint32		set = 0;

		for (uint32 i = 0; i < SIFT_HLL_M; i++)
		{
			uint32		v = reg6(blob + SIFT_HLL_HDR, i);

			if (v > SIFT_HLL_TOP_RANK)
				return false;
			set += v != 0;
		}
		return set == n;
	}
}

bool
sift_hll_build(const SiftColDef *def, const SiftCPaggCol *c, const SiftCDict *dict, StringInfo out)
{
	bool		wide = def->type == SIFT_COLT_INT8 || def->type == SIFT_COLT_TIMESTAMP || def->type == SIFT_COLT_TIMESTAMPTZ,
				built;
	SiftHll		h;

	if (!c->ready)
		return false;
	for (uint32 i = 0; c->ovf && i < SIFT_COL_NWORDS(c->n); i++)
		if (c->ovf[i])
			return false;
	sift_hll_init(&h, CurrentMemoryContext);
	for (uint32 i = 0; i < c->n; i++)
	{
		int64		k = c->key[i];

		if (c->nulls && (c->nulls[i / 64] >> (i % 64) & 1))
			continue;
		if (def->store == SIFT_COLS_STR)
			sift_hll_add(&h, sift_hll_hash_bytes(dict->bytes + dict->off[k], dict->off[k + 1] - dict->off[k]));
		else
			sift_hll_add(&h, wide ? sift_hll_hash_i64(k) : sift_hll_hash_i32((int32) k));
	}
	built = sift_hll_encode(&h, out);
	sift_hll_free(&h);
	return built;
}

uint64
sift_hll_hash_i32(int32 v)
{
	return hash_bytes_uint32_extended((uint32) v, 0);
}

uint64
sift_hll_hash_i64(int64 v)
{
	uint32		lo = (uint32) v,
				hi = (uint32) (v >> 32);

	return hash_bytes_uint32_extended(lo ^ (v >= 0 ? hi : ~hi), 0);
}

uint64
sift_hll_hash_bytes(const void *p, int len)
{
	return hash_bytes_extended((const unsigned char *) p, len, 0);
}

bool
sift_hll_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	(void) s;
	if (d->kind != SIFT_COLK_SCALAR || d->parent >= 0)
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return d->store == SIFT_COLS_INT && !sift_col_def_has_kinds(d);
		case SIFT_COLT_TEXT:
		case SIFT_COLT_UUID:
			return d->store == SIFT_COLS_STR;
		default:
			return false;
	}
}

const char *
sift_hll_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, sift_hll_leaf_ok, "not an int2, int4, int8, date, timestamp, timestamptz, text, varchar or uuid column");
}

const char *
sift_hll_syntax(const char *opt)
{
	return sift_cpagg_leaf_list(opt, NULL, NULL, NULL, NULL, NULL);
}
