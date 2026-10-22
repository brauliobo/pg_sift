/*
 * col/setsum.c - the small-set summary of a leaf in a unit. Portable (core/host.h only).
 *
 * Blob of a unit and leaf (little endian): u8 kind, u8 aux, u16 flags (0), u32 n (distinct keys, at least 2), u64 v, then the body.
 *     EXACT (kind 1): aux = bytes of a key offset (1, 2, 4 or 8), v = the smallest key, then n offsets from it, strictly ascending, the first 0.
 *     BLOOM (kind 2): aux = probes k, v = bits (a multiple of 8, 64 or more, below 2^32), then v / 8 bytes. Probe j of key x is bit
 *                     ((uint32) (h1 + j * h2) * v) >> 32 with h = mix(x), h1 = the low and h2 = the high half of h with its low bit set.
 *     PRES (kind 3): aux = 0, n >= 1 distinct keys, v = the key of bit 0 (a multiple of 64, at most the smallest key), then 8-byte words up to the one that
 *                    holds the largest key; bit i of word w is key v + 64 w + i. The first and last word are not zero and the bits number n.
 * The writer keeps up to a limit of keys exactly and the rest as a filter of 10 bits a key; a unit whose keys fill the range between its smallest
 * and largest, or that has fewer than two, has none (its zone says as much). A leaf of key_presence keeps every key as PRES where the bitmap takes at
 * most SIFT_CSET_PRES_PCT of the unit's bytes, else what set_columns would give it.
 */

#include "core/host.h"

#include "col/setsum.h"
#include "core/cmp.h"
#include "core/popcount.h"

static inline uint64
mix64(uint64 x)
{
	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64CONST(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

static inline uint32
bloom_bit(uint64 h, uint32 j, uint64 nbits)
{
	uint32		h1 = (uint32) h,
				h2 = (uint32) (h >> 32) | 1;

	return (uint32) (((uint64) (uint32) (h1 + j * h2) * nbits) >> 32);
}

static void
bloom_add(uint8 *bits, uint64 nbits, uint32 k, int64 key)
{
	uint64		h = mix64((uint64) key);

	for (uint32 j = 0; j < k; j++)
	{
		uint32		b = bloom_bit(h, j, nbits);

		bits[b >> 3] |= (uint8) (1 << (b & 7));
	}
}

static bool
bloom_has(const SiftCSet *s, int64 key)
{
	uint64		h = mix64((uint64) key);

	for (uint32 j = 0; j < s->aux; j++)
	{
		uint32		b = bloom_bit(h, j, s->nbits);

		if (!(s->data[b >> 3] >> (b & 7) & 1))
			return false;
	}
	return true;
}

/* The leaves of a set: those of a pre-aggregate dimension, and a top-level uuid, whose dictionary holds the 16 bytes of each value (its keys are codes, as a text's) */
bool
sift_cset_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cpagg_dim_ok(s, d) ||
		(d->kind == SIFT_COLK_SCALAR && d->parent < 0 && d->store == SIFT_COLS_STR && d->type == SIFT_COLT_UUID);
}

const char *
sift_cset_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, sift_cset_leaf_ok, "not a plain integer, text or uuid column");
}

const char *
sift_cset_syntax(const char *opt)
{
	return sift_cpagg_leaf_list(opt, NULL, NULL, NULL, NULL, NULL);
}

bool
sift_cset_pres_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cpagg_dim_ok(s, d) && d->parent < 0 && d->store == SIFT_COLS_INT && !sift_col_def_has_kinds(d) &&
		(d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_DATE);
}

const char *
sift_cset_pres_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, sift_cset_pres_leaf_ok, "not a plain int2, int4 or date column");
}

uint32
sift_cset_keys(const SiftCPaggCol *c, int64 *out)
{
	uint32		m = 0;

	for (uint32 i = 0; i < c->n; i++)
	{
		if (c->nulls && (c->nulls[i / 64] >> (i % 64) & 1))
			continue;
		if (c->ovf && (c->ovf[i / 64] >> (i % 64) & 1))
			continue;
		out[m++] = c->key[i];
	}
	if (m > 1)
	{
		uint32		d = 1;

		qsort(out, m, sizeof(int64), sift_cmp_i64);
		for (uint32 i = 1; i < m; i++)
			if (out[i] != out[d - 1])
				out[d++] = out[i];
		m = d;
	}
	return m;
}

static void
put_head(StringInfo out, uint8 kind, uint8 aux, uint32 n, uint64 v)
{
	uint8		h[SIFT_CSET_HDR] = {kind, aux};

	memcpy(h + 4, &n, sizeof(n));
	memcpy(h + 8, &v, sizeof(v));
	appendBinaryStringInfo(out, (const char *) h, sizeof(h));
}

bool
sift_cset_build(const int64 *keys, uint32 nd, uint32 limit, StringInfo out)
{
	uint64		span;

	if (nd < 2)
		return false;
	span = (uint64) keys[nd - 1] - (uint64) keys[0];
	if (span == nd - 1)
		return false;
	if (nd <= limit)
	{
		uint8		w = span <= UINT8_MAX ? 1 : span <= UINT16_MAX ? 2 : span <= UINT32_MAX ? 4 : 8;

		put_head(out, SIFT_CSET_EXACT, w, nd, (uint64) keys[0]);
		for (uint32 i = 0; i < nd; i++)
		{
			uint64		d = (uint64) keys[i] - (uint64) keys[0];

			appendBinaryStringInfo(out, (const char *) &d, w);
		}
	}
	else
	{
		uint64		nbits = ((uint64) nd * SIFT_CSET_BLOOM_BITS + 7) / 8 * 8;
		uint8	   *bits;

		if (nbits < 64)
			nbits = 64;
		if (nbits >= UINT64CONST(1) << 32)
			return false;
		bits = palloc0(nbits / 8);
		for (uint32 i = 0; i < nd; i++)
			bloom_add(bits, nbits, SIFT_CSET_BLOOM_K, keys[i]);
		put_head(out, SIFT_CSET_BLOOM, SIFT_CSET_BLOOM_K, nd, nbits);
		appendBinaryStringInfo(out, (const char *) bits, nbits / 8);
		pfree(bits);
	}
	return true;
}

static inline uint64
pres_word(const SiftCSet *s, uint64 w)
{
	uint64		x;

	memcpy(&x, s->data + w * sizeof(x), sizeof(x));
	return x;
}

bool
sift_cset_build_pres(const int64 *keys, uint32 nd, Size max_bytes, StringInfo out)
{
	int64		base = keys[0] & ~(int64) 63;
	uint64		nwords = ((uint64) keys[nd - 1] - (uint64) base) / 64 + 1;
	uint64	   *bits;

	if (nwords * sizeof(uint64) + SIFT_CSET_HDR > max_bytes)
		return false;
	bits = palloc0(nwords * sizeof(uint64));
	for (uint32 i = 0; i < nd; i++)
	{
		uint64		d = (uint64) keys[i] - (uint64) base;

		bits[d / 64] |= UINT64CONST(1) << (d % 64);
	}
	put_head(out, SIFT_CSET_PRES, 0, nd, (uint64) base);
	appendBinaryStringInfo(out, (const char *) bits, nwords * sizeof(uint64));
	pfree(bits);
	return true;
}

/* is any key of [lo, hi] a bit of the bitmap */
static bool
pres_any(const SiftCSet *s, int64 lo, int64 hi)
{
	uint64		a,
				b;

	if (hi < s->base || lo > hi)
		return false;
	a = lo <= s->base ? 0 : (uint64) lo - (uint64) s->base;
	b = (uint64) hi - (uint64) s->base;
	if (a >= s->nbits)
		return false;
	b = Min(b, s->nbits - 1);
	for (uint64 w = a / 64; w <= b / 64; w++)
	{
		uint64		m = pres_word(s, w);

		if (w == a / 64)
			m &= ~UINT64CONST(0) << (a % 64);
		if (w == b / 64)
			m &= ~UINT64CONST(0) >> (63 - b % 64);
		if (m)
			return true;
	}
	return false;
}

static inline uint64
exact_at(const SiftCSet *s, uint32 i)
{
	uint64		d = 0;

	memcpy(&d, s->data + (Size) i * s->aux, s->aux);
	return d;
}

bool
sift_cset_open(const uint8 *blob, uint32 len, SiftCSet *s)
{
	uint16		flags;
	uint64		v;

	if (len < SIFT_CSET_HDR)
		return false;
	memcpy(&flags, blob + 2, sizeof(flags));
	memcpy(&s->n, blob + 4, sizeof(s->n));
	memcpy(&v, blob + 8, sizeof(v));
	s->kind = blob[0];
	s->aux = blob[1];
	s->data = blob + SIFT_CSET_HDR;
	s->base = 0;
	s->nbits = 0;
	if (flags != 0 || s->n < (s->kind == SIFT_CSET_PRES ? 1 : 2))
		return false;
	if (s->kind == SIFT_CSET_PRES)
	{
		uint64		nw = (len - SIFT_CSET_HDR) / sizeof(uint64),
					pop = 0;

		if (s->aux != 0 || nw == 0 || (len - SIFT_CSET_HDR) % sizeof(uint64) != 0 || (v & 63) != 0 || nw >= UINT64CONST(1) << 32 ||
			(int64) v > PG_INT64_MAX - (int64) (nw * 64))
			return false;
		s->base = (int64) v;
		s->nbits = nw * 64;
		for (uint64 w = 0; w < nw; w++)
			pop += sift_pc64(pres_word(s, w));
		return pop == s->n && pres_word(s, 0) != 0 && pres_word(s, nw - 1) != 0;
	}
	if (s->kind == SIFT_CSET_EXACT)
	{
		if ((s->aux != 1 && s->aux != 2 && s->aux != 4 && s->aux != 8) || s->n > SIFT_CSET_LIMIT_MAX ||
			(uint64) s->n * s->aux != len - SIFT_CSET_HDR)
			return false;
		s->base = (int64) v;
		if (exact_at(s, 0) != 0)
			return false;
		for (uint32 i = 1; i < s->n; i++)
			if (exact_at(s, i) <= exact_at(s, i - 1))
				return false;
		return true;
	}
	if (s->kind == SIFT_CSET_BLOOM)
	{
		bool		any = false;

		if (s->aux < 1 || s->aux > 16 || v < 64 || v % 8 != 0 || v >= UINT64CONST(1) << 32 || v / 8 != len - SIFT_CSET_HDR || s->n > v)
			return false;
		s->nbits = v;
		for (uint64 i = 0; i < v / 8 && !any; i++)
			any = s->data[i] != 0;
		return any;
	}
	return false;
}

bool
sift_cset_may_hold(const SiftCSet *s, int64 lo, int64 hi)
{
	if (s->kind == SIFT_CSET_PRES)
		return pres_any(s, lo, hi);
	if (s->kind == SIFT_CSET_EXACT)
	{
		uint64		dlo,
					dhi;
		uint32		a = 0,
					b = s->n;

		if (hi < s->base)
			return false;
		dlo = lo <= s->base ? 0 : (uint64) lo - (uint64) s->base;
		dhi = (uint64) hi - (uint64) s->base;
		SIFT_BISECT(uint32, m, a, b, exact_at(s, m) < dlo);
		return a < s->n && exact_at(s, a) <= dhi;
	}
	if ((uint64) hi - (uint64) lo >= SIFT_CSET_PROBES)
		return true;
	for (int64 k = lo;; k++)
	{
		if (bloom_has(s, k))
			return true;
		if (k == hi)
			return false;
	}
}

void
sift_cset_members(const SiftCSet *s, int64 *out)
{
	Assert(s->kind == SIFT_CSET_EXACT);
	for (uint32 i = 0; i < s->n; i++)
		out[i] = (int64) ((uint64) s->base + exact_at(s, i));
}

bool
sift_cset_matches(const SiftCSet *s, const int64 *keys, uint32 nd)
{
	if (s->n != nd)
		return false;
	if (s->kind == SIFT_CSET_PRES)
	{
		/* the population is n, so n distinct keys that are all bits are the bits */
		for (uint32 i = 0; i < nd; i++)
			if (!pres_any(s, keys[i], keys[i]))
				return false;
		return true;
	}
	if (s->kind == SIFT_CSET_EXACT)
	{
		for (uint32 i = 0; i < nd; i++)
			if ((int64) ((uint64) s->base + exact_at(s, i)) != keys[i])
				return false;
		return true;
	}
	{
		uint8	   *bits = palloc0(s->nbits / 8);
		bool		same;

		for (uint32 i = 0; i < nd; i++)
			bloom_add(bits, s->nbits, s->aux, keys[i]);
		same = memcmp(bits, s->data, s->nbits / 8) == 0;
		pfree(bits);
		return same;
	}
}
