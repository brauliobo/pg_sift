/*
 * col/setsum.c - the keys of a leaf in a unit. Portable (core/host.h only).
 *
 * Blob of a unit and leaf (little endian): u8 kind, u8 aux, u16 flags (0), u32 n (distinct keys, at least 2), u64 v, then the body.
 *     EXACT (kind 1): aux = bytes of a key offset (1, 2, 4 or 8), v = the smallest key, then n offsets from it, strictly ascending, the first 0.
 *     BLOOM (kind 2): aux = probes k, v = bits (a multiple of 8, 64 or more, below 2^32), then v / 8 bytes. Probe j of key x is bit
 *                     ((uint32) (h1 + j * h2) * v) >> 32 with h = mix(x), h1 = the low and h2 = the high half of h with its low bit set.
 *     PRES (kind 3): aux = 0, n >= 1 distinct keys, v = the key of bit 0 (a multiple of 64, at most the smallest key), then 8-byte words up to the one that
 *                    holds the largest key; bit i of word w is key v + 64 w + i. The first and last word are not zero and the bits number n.
 *     PLIST (kind 4): the layout of EXACT, for any number of keys that fit the bytes of the unit's share.
 * A leaf of key_columns keeps every key as PRES or PLIST where that takes at most SIFT_CSET_PRES_PCT of the unit's bytes (with the other leaves);
 * else, for a leaf the user named, up to SIFT_CSET_LIMIT keys exactly and the rest as a filter of 10 bits a key. A unit whose keys fill the range between
 * its smallest and largest, or that has fewer than two, has no EXACT or BLOOM (its zone says as much).
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

bool
sift_cset_pres_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cpagg_dim_ok(s, d) && d->parent < 0 && d->store == SIFT_COLS_INT && !sift_col_def_has_kinds(d) &&
		(d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_DATE);
}

/* the list of key_columns without its 'auto' word and colon; *autolist: the build chose it (or is to) */
static const char *
keys_list(const char *opt, bool *autolist)
{
	const char *p = opt ? opt : "";

	while (*p == ' ')
		p++;
	*autolist = strncmp(p, "auto", 4) == 0 && (p[4] == '\0' || p[4] == ':' || p[4] == ' ');
	if (*autolist)
	{
		for (p += 4; *p == ' '; p++)
			;
		if (*p == ':')
			p++;
	}
	return p;
}

const char *
sift_cset_keys_syntax(const char *opt)
{
	bool		autolist;

	return sift_cpagg_leaf_list(keys_list(opt, &autolist), NULL, NULL, NULL, NULL, NULL);
}

const char *
sift_cset_keys_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec)
{
	const char *err = sift_cpagg_leaf_list(keys_list(opt, &spec->keyauto), s, names, spec->set, sift_cset_leaf_ok, "not a plain integer, text or uuid column");

	for (int c = 0; err == NULL && c < s->ncols; c++)
		spec->pres[c] = spec->set[c] && sift_cset_pres_leaf_ok(s, &s->cols[c]);
	return err;
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

static uint8
offset_width(uint64 span)
{
	return span <= UINT8_MAX ? 1 : span <= UINT16_MAX ? 2 : span <= UINT32_MAX ? 4 : 8;
}

/* the offsets from base of the keys, ascending, in w bytes each: the list form of the keys of a unit, laid out like an exact summary */
static void
put_list(StringInfo out, uint8 w, int64 base, const uint64 *off, uint32 nd)
{
	put_head(out, SIFT_CSET_PLIST, w, nd, (uint64) base);
	for (uint32 i = 0; i < nd; i++)
		appendBinaryStringInfo(out, (const char *) &off[i], w);
}

/* the distinct offsets of the keys from lo, if there are at most cap of them: a hash set that gives up at cap + 1, then sorted */
static uint64 *
distinct_offsets(const SiftCPaggCol *c, int64 lo, uint64 cap, uint32 *nd)
{
	uint32		size = 16,
				n = 0;
	uint64	   *slot,
			   *out;

	while (size < cap * 2 + 2)
		size *= 2;
	slot = palloc0(size * sizeof(uint64));

	for (uint32 i = 0; i < c->n; i++)
	{
		uint64		d;
		uint32		h;

		if ((c->nulls && (c->nulls[i / 64] >> (i % 64) & 1)) || (c->ovf && (c->ovf[i / 64] >> (i % 64) & 1)))
			continue;
		d = (uint64) c->key[i] - (uint64) lo + 1;
		for (h = (uint32) ((d * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & (size - 1); slot[h] != 0 && slot[h] != d; h = (h + 1) & (size - 1))
			;
		if (slot[h] == 0)
		{
			slot[h] = d;
			if (++n > cap)
			{
				pfree(slot);
				return NULL;
			}
		}
	}
	out = palloc(Max(n, 1) * sizeof(uint64));
	n = 0;
	for (uint32 h = 0; h < size; h++)
		if (slot[h] != 0)
			out[n++] = slot[h] - 1;
	pfree(slot);
	qsort(out, n, sizeof(uint64), sift_cmp_u64);
	*nd = n;
	return out;
}

/*
 * RUNS: the keys as runs of consecutive integers, for ids that arrive in batches (aux: RUNS_AUX with the log2 of the bytes of a start and of a length minus 1);
 * the body is nr starts (offsets from the smallest key, ascending, the first 0) and then nr lengths minus 1, so that a test is a binary search like that of a list.
 */
#define RUNS_OFF_CAP	((uint64) 1 << 62)
#define RUNS_AUX		0x40	/* aux: this marker, then the log2 of the bytes of a start (bits 0-1) and of a length minus 1 (bits 2-3): a byte of any other kind's aux is not one */

static inline uint64
runs_get(const uint8 *p, uint8 w, uint32 i)
{
	switch (w)
	{
		case 1:
			return p[i];
		case 2:
			{
				uint16		d;

				memcpy(&d, p + (Size) i * 2, sizeof(d));
				return d;
			}
		case 4:
			{
				uint32		d;

				memcpy(&d, p + (Size) i * 4, sizeof(d));
				return d;
			}
		default:
			{
				uint64		d;

				memcpy(&d, p + (Size) i * 8, sizeof(d));
				return d;
			}
	}
}

static inline uint8
runs_ws(const SiftCSet *s)
{
	return 1 << (s->aux & 3);
}

static inline uint8
runs_wl(const SiftCSet *s)
{
	return 1 << (s->aux >> 2 & 3);
}

static inline uint32
runs_nr(const SiftCSet *s)
{
	return s->dlen / (runs_ws(s) + runs_wl(s));
}

static inline uint64
runs_start(const SiftCSet *s, uint32 i)
{
	return runs_get(s->data, runs_ws(s), i);
}

/* the offset of the last key of run i */
static inline uint64
runs_last(const SiftCSet *s, uint32 i)
{
	return runs_get(s->data, runs_ws(s), i) + runs_get(s->data + (Size) runs_nr(s) * runs_ws(s), runs_wl(s), i);
}

static uint8
width_log2(uint64 v)
{
	return v <= UINT8_MAX ? 0 : v <= UINT16_MAX ? 1 : v <= UINT32_MAX ? 2 : 3;
}

/* The keys, ascending offsets from lo, as a RUNS blob if it takes at most max_bytes */
static bool
put_runs(StringInfo out, int64 lo, const uint64 *off, uint32 nd, Size max_bytes)
{
	uint64	   *st = palloc(nd * sizeof(uint64)),
			   *ln = palloc(nd * sizeof(uint64)),
				maxl = 0;
	uint32		nr = 0;
	uint8		lws,
				lwl;
	bool		ok;

	for (uint32 i = 0, j; i < nd; i = j + 1)
	{
		for (j = i; j + 1 < nd && off[j + 1] == off[j] + 1; j++)
			;
		st[nr] = off[i];
		ln[nr] = j - i;
		maxl = Max(maxl, ln[nr]);
		nr++;
	}
	lws = width_log2(st[nr - 1]);
	lwl = width_log2(maxl);
	/* one run is the zone: the keys fill their range */
	ok = nr > 1 && SIFT_CSET_HDR + (Size) nr * ((1 << lws) + (1 << lwl)) <= max_bytes;
	if (ok)
	{
		put_head(out, SIFT_CSET_RUNS, RUNS_AUX | lws | lwl << 2, nd, (uint64) lo);
		for (uint32 i = 0; i < nr; i++)
			appendBinaryStringInfo(out, (const char *) &st[i], 1 << lws);
		for (uint32 i = 0; i < nr; i++)
			appendBinaryStringInfo(out, (const char *) &ln[i], 1 << lwl);
	}
	pfree(st);
	pfree(ln);
	return ok;
}

bool
sift_cset_build_pres(const SiftCPaggCol *c, Size max_bytes, StringInfo out)
{
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN,
				base;
	uint64		nwords,
				nd = 0,
				cap;
	uint8		w;
	uint64	   *bits,
			   *off;
	bool		bitmap_fits;

	for (uint32 i = 0; i < c->n; i++)
	{
		int64		k = c->key[i];

		if ((c->nulls && (c->nulls[i / 64] >> (i % 64) & 1)) || (c->ovf && (c->ovf[i / 64] >> (i % 64) & 1)))
			continue;
		lo = Min(lo, k);
		hi = Max(hi, k);
		nd++;
	}
	if (nd == 0)
		return false;
	base = lo & ~(int64) 63;
	nwords = ((uint64) hi - (uint64) base) / 64 + 1;
	w = offset_width((uint64) hi - (uint64) lo);
	bitmap_fits = nwords * sizeof(uint64) + SIFT_CSET_HDR <= max_bytes;
	if (!bitmap_fits)
	{
		uint32		n;

		cap = max_bytes > SIFT_CSET_HDR ? (max_bytes - SIFT_CSET_HDR) / w : 0;
		if (cap == 0 || (off = distinct_offsets(c, lo, cap, &n)) == NULL)
		{
			/* no bitmap or list of offsets fits: the keys that come in runs of consecutive ones (ids that arrive in batches) may */
			bool		ok = max_bytes > SIFT_CSET_HDR && (off = distinct_offsets(c, lo, c->n, &n)) != NULL && put_runs(out, lo, off, n, max_bytes);

			if (off)
				pfree(off);
			return ok;
		}
		put_list(out, w, lo, off, n);
		pfree(off);
		return true;
	}
	bits = palloc0(nwords * sizeof(uint64));
	for (uint32 i = 0; i < c->n; i++)
	{
		uint64		d;

		if ((c->nulls && (c->nulls[i / 64] >> (i % 64) & 1)) || (c->ovf && (c->ovf[i / 64] >> (i % 64) & 1)))
			continue;
		d = (uint64) c->key[i] - (uint64) base;
		bits[d / 64] |= UINT64CONST(1) << (d % 64);
	}
	nd = 0;
	for (uint64 x = 0; x < nwords; x++)
		nd += sift_pc64(bits[x]);
	/* the smaller of the two forms: the bitmap is a bit for every integer of the range, the list a width for every key */
	if (nd * w < nwords * sizeof(uint64))
	{
		uint32		m = 0;

		off = palloc(nd * sizeof(uint64));
		for (uint64 x = 0; x < nwords; x++)
			for (int b = 0; b < 64; b++)
				if (bits[x] >> b & 1)
					off[m++] = (uint64) base - (uint64) lo + x * 64 + b;
		put_list(out, w, lo, off, m);
		pfree(off);
	}
	else
	{
		put_head(out, SIFT_CSET_PRES, 0, (uint32) nd, (uint64) base);
		appendBinaryStringInfo(out, (const char *) bits, nwords * sizeof(uint64));
	}
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
	/* check-memory: allow b is below nbits, a count of bits of a summary, so b / 64 is far from the maximum */
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

/* does the list start at 0 and ascend strictly? one loop for each width, so that a load is one instruction (the check is made for every set a statement prunes with) */
#define EXACT_ASCENDING(T) \
	do { \
		T			prev, \
					cur; \
		memcpy(&prev, s->data, sizeof(T)); \
		if (prev != 0) \
			return false; \
		for (uint32 i = 1; i < s->n; i++) \
		{ \
			memcpy(&cur, s->data + (Size) i * sizeof(T), sizeof(T)); \
			if (cur <= prev) \
				return false; \
			prev = cur; \
		} \
		return true; \
	} while (0)

static bool
exact_ascending(const SiftCSet *s)
{
	switch (s->aux)
	{
		case 1:
			EXACT_ASCENDING(uint8);
		case 2:
			EXACT_ASCENDING(uint16);
		case 4:
			EXACT_ASCENDING(uint32);
		default:
			EXACT_ASCENDING(uint64);
	}
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
	s->dlen = 0;
	if (flags != 0 || s->n < (s->kind == SIFT_CSET_PRES || s->kind == SIFT_CSET_PLIST || s->kind == SIFT_CSET_RUNS ? 1 : 2))
		return false;
	if (s->kind == SIFT_CSET_RUNS)
	{
		uint8		ws = runs_ws(s),
					wl = runs_wl(s);
		uint32		nr;
		uint64		keys = 0,
					prev = 0;

		if ((s->aux & 0xf0) != RUNS_AUX || (int64) v > (int64) RUNS_OFF_CAP || (int64) v < -(int64) RUNS_OFF_CAP || len - SIFT_CSET_HDR == 0 || (len - SIFT_CSET_HDR) % (ws + wl) != 0)
			return false;
		s->base = (int64) v;
		s->dlen = len - SIFT_CSET_HDR;
		nr = runs_nr(s);
		for (uint32 i = 0; i < nr; i++)
		{
			uint64		st = runs_start(s, i),
						last = runs_last(s, i);

			if (st > RUNS_OFF_CAP || last >= RUNS_OFF_CAP || (i == 0 ? st != 0 : st < prev + 2))
				return false;
			keys += last - st + 1;
			prev = last;
			if (keys > s->n)
				return false;
		}
		return keys == s->n;
	}
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
	if (s->kind == SIFT_CSET_EXACT || s->kind == SIFT_CSET_PLIST)
	{
		if ((s->aux != 1 && s->aux != 2 && s->aux != 4 && s->aux != 8) || (s->kind == SIFT_CSET_EXACT && s->n > SIFT_CSET_LIMIT_MAX) ||
			(uint64) s->n * s->aux != len - SIFT_CSET_HDR)
			return false;
		s->base = (int64) v;
		return exact_ascending(s);
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
	if (s->kind == SIFT_CSET_RUNS)
	{
		uint64		dlo,
					dhi;
		uint32		a = 0,
					b = runs_nr(s);

		if (hi < s->base)
			return false;
		dlo = lo <= s->base ? 0 : (uint64) lo - (uint64) s->base;
		dhi = (uint64) hi - (uint64) s->base;
		SIFT_BISECT(uint32, m, a, b, runs_last(s, m) < dlo);
		return a < runs_nr(s) && runs_start(s, a) <= dhi;
	}
	if (s->kind == SIFT_CSET_EXACT || s->kind == SIFT_CSET_PLIST)
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
	Assert(s->kind == SIFT_CSET_EXACT || s->kind == SIFT_CSET_PLIST || s->kind == SIFT_CSET_RUNS);
	if (s->kind == SIFT_CSET_RUNS)
	{
		uint32		m = 0;

		for (uint32 i = 0, nr = runs_nr(s); i < nr; i++)
			for (uint64 k = 0, st = runs_start(s, i), len = runs_last(s, i) - st + 1; k < len && m < s->n; k++)
				out[m++] = (int64) ((uint64) s->base + st + k);
		return;
	}
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
	if (s->kind == SIFT_CSET_RUNS)
	{
		uint32		m = 0;

		for (uint32 i = 0, nr = runs_nr(s); i < nr; i++)
			for (uint64 k = 0, st = runs_start(s, i), len = runs_last(s, i) - st + 1; k < len; k++, m++)
				if (m >= nd || keys[m] != (int64) ((uint64) s->base + st + k))
					return false;
		return m == nd;
	}
	if (s->kind == SIFT_CSET_EXACT || s->kind == SIFT_CSET_PLIST)
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
