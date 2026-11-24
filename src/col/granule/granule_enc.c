/*
 * col/granule_enc.c - the encoders of the chunks of a granule (col/granule.h). A column is written in the encoding that holds its rows in the fewest bytes, unless its owner says which
 * it holds (SiftChunkHint). Every integer-class body is a blob of the column codec (col/codec.h: the vertical 32 x 32 bit-packing, the same bytes at every SIMD level); what a column
 * gets depends on the tier of the granule (col/granule.h): the lightweight tiers keep to constants, runs, deltas and frames of reference, the cold tier adds dictionaries of the whole
 * part, wrapped string blocks, byte-split doubles and the cost rule of the codec for LZ4 and zstd.
 */
#include "core/host.h"

#include <math.h>

#include "common/hashfn.h"
#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/granule_int.h"
#include "core/par/sbuf.h"

#define SIFT_WRAP_MIN		512
#define SIFT_WRAP_HOT_MIN	2048

void
sift_cc_policy_tier(SiftCcPolicy *pol, SiftCompressTier tier, int alg)
{
	sift_cc_policy_default(pol);
	if (!SIFT_TIER_IS_COLD(tier))
	{
		pol->explicit_encs = 1;
		pol->encs = (1U << SIFT_CCN_FORBIT) | (1U << SIFT_CCN_DELTA) | (1U << SIFT_CCN_RLE) | (1U << SIFT_CCN_ALP);
		pol->alg = SIFT_CC_ALG_NONE;
	}
	else if (alg < 0)
		pol->alg = SIFT_CC_ALG_NONE;
	else if (alg > 0 || tier == SIFT_TIER_COLD_L2)
	{
		pol->mode = SIFT_CCM_LAYER;
		pol->alg = alg > 0 ? (uint8) alg : SIFT_CC_ALG_ZSTD;
	}
	else
		pol->fast_decode = tier == SIFT_TIER_WARM_L1;
}

uint32
sift_cc_blob_put(StringInfo s, const StringInfoData *blob)
{
	uint32		hdr[2] = {blob->len, 0};
	uint32		start = s->len;

	sift_sb_append(s, (char *) hdr, 8);
	sift_sb_append(s, blob->data, blob->len);
	pad8(s);
	return s->len - start;
}

/* The values of integer-class cascades, appended behind their length; the label of the cascade the codec chose */
int
sift_put_cc_ints(StringInfo s, const int64 *vals, int n, const SiftChunkOpts *opt)
{
	SiftCcPolicy pol;
	SiftCcInfo	info;
	StringInfoData blob;
	int			tag;

	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	sift_sb_init(&blob);
	sift_cc_encode(&blob, SIFT_CC_INT, vals, n, NULL, &pol, &info);
	sift_cc_blob_put(s, &blob);
	sift_free(blob.data);
	tag = info.tag == SIFT_CCN_WRAP ? info.inner : info.tag;
	return tag == SIFT_CCN_CONST ? SIFT_CENC_INT_CONST : tag == SIFT_CCN_DELTA ? SIFT_CENC_INT_DELTA : tag == SIFT_CCN_RLE ? SIFT_CENC_RLE : SIFT_CENC_INT_FOR;
}

/* A list of the rows (the nulls or the values) that are few: their count and their indexes, padded */
static void
put_row_list(StringInfo s, const bool *isnull, int n, bool want_null, uint16 cnt)
{
	sift_sb_append(s, (char *) &cnt, 2);
	for (int i = 0; i < n; i++)
		if (isnull[i] == want_null)
		{
			uint16		idx = i;

			sift_sb_append(s, (char *) &idx, 2);
		}
	pad8(s);
}

/* The validity of a column, apart from its body: nothing when every row has a value or none, else a list of the rows without one or of the rows with one, whichever are few, else a bit per row; the count of rows with a value */
/* STOR[STOR-CODEC-09]: null bitmap 0 B empty/full, sparse <=1/16, dense raw (1024 B/granule) */
static int
put_validity(StringInfo s, const bool *isnull, int n, SiftChunkRef *ref)
{
	int			nn = 0;

	for (int i = 0; i < n; i++)
		nn += isnull == NULL || !isnull[i];
	ref->nnonnull = nn;
	if (nn == n)
		ref->flags |= SIFT_CHUNK_ALL_VALID;
	else if (nn == 0)
		ref->flags |= SIFT_CHUNK_ALL_NULL;
	else if (n - nn <= n / SIFT_NULL_SPARSE_MAX_DIV)
	{
		put_row_list(s, isnull, n, true, n - nn);
		ref->enc |= SIFT_ENC_NULL_SPARSE;
	}
	else if (nn <= n / SIFT_NULL_SPARSE_MAX_DIV)
	{
		put_row_list(s, isnull, n, false, nn);
		ref->enc |= SIFT_ENC_NONNULL_SPARSE;
	}
	else
	{
		uint8	   *bm = sift_alloc0((n + 7) / 8);

		for (int i = 0; i < n; i++)
			if (!isnull[i])
				bm[i >> 3] |= 1 << (i & 7);
		sift_sb_append(s, (char *) bm, (n + 7) / 8);
		pad8(s);
		sift_free(bm);
	}
	return nn;
}

/* The kind of each row as one cascade of the codec: a run while the rows of a column share a kind, a few bits for each row else (not the 8 bytes of an entry) */
static void
put_packed_kinds(StringInfo s, const uint8 *tag, int n, const SiftChunkOpts *opt)
{
	int64	   *kinds = sift_alloc(n * sizeof(int64));

	for (int i = 0; i < n; i++)
		kinds[i] = tag[i];
	sift_put_cc_ints(s, kinds, n, opt);
	sift_free(kinds);
}

/* The rows with a tag: one entry each in row order (behind the mask of the rows when there are many), then the payloads of the entries that keep the value itself (the entry gives where its payload starts); rows that keep only their kind are held as the kinds of all rows when that is smaller */
static void
put_exceptions(StringInfo s, const SiftColIn *col, int n, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	SiftExcHeader h = {0};
	SiftVariantExcEntry e = {0};
	StringInfoData pay;
	const uint8 *tag = col->tag;

	sift_sb_init(&pay);
	for (int i = 0; i < n; i++)
		if (tag[i] != 0)
		{
			h.n_entries++;
			ref->flags |= tag[i] == SIFT_COLTAG_JNULL || col->isnull == NULL || !col->isnull[i] ? SIFT_CHUNK_HAS_KINDS : SIFT_CHUNK_HAS_OVERFLOW;
		}
	for (int i = 0; i < n; i++)
		if (tag[i] != 0)
		{
			uint32		len = col->xplen != NULL ? col->xplen[i] : 0;

			if (len > 0)
				sift_sb_append(&pay, col->xpay[i], len);
		}
	h.payload_bytes = pay.len;
	/* STOR[STOR-CODEC-07]: the 1024 B exception mask is written only for granules of 2048 rows or more (the reader rebuilds it from the entries) */
	if (h.n_entries > SIFT_EXC_SPARSE_MAX && n >= SIFT_EXC_MASK_MIN_ROWS)
		h.flags |= SIFT_EXC_MASKED;
	if (pay.len == 0)
	{
		StringInfoData kinds;

		sift_sb_init(&kinds);
		put_packed_kinds(&kinds, tag, n, opt);
		if (kinds.len < SIFT_EXC_BLOCK_LEN(&h))
		{
			h.flags = SIFT_EXC_KINDS_PACKED;
			h.payload_bytes = kinds.len;
			sift_sb_append(s, (char *) &h, sizeof(h));
			sift_sb_append(s, kinds.data, kinds.len);
			pad8(s);
			sift_free(kinds.data);
			sift_free(pay.data);
			return;
		}
		sift_free(kinds.data);
	}
	sift_sb_append(s, (char *) &h, sizeof(h));
	if (h.flags & SIFT_EXC_MASKED)
	{
		uint64		mask[SIFT_EXC_MASK_WORDS] = {0};

		for (int i = 0; i < n; i++)
			if (tag[i] != 0)
				mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		sift_sb_append(s, (char *) mask, sizeof(mask));
	}
	for (int i = 0, off = 0; i < n; i++)
		if (tag[i] != 0)
		{
			uint32		len = col->xplen != NULL ? col->xplen[i] : 0;

			e = (SiftVariantExcEntry) {.row_idx = i,.kind = sift_exc_kind(tag[i]),.reserved = tag[i],.payload_off = off};
			sift_sb_append(s, (char *) &e, sizeof(e));
			off += len;
		}
	e = (SiftVariantExcEntry) {.payload_off = pay.len};
	sift_sb_append(s, (char *) &e, sizeof(e));
	sift_sb_append(s, pay.data, pay.len);
	pad8(s);
	sift_free(pay.data);
}

static bool
has_tags(const SiftColIn *col, int n)
{
	if (col->tag == NULL)
		return false;
	for (int i = 0; i < n; i++)
		if (col->tag[i] != 0)
			return true;
	return false;
}

static void
put_bits(StringInfo s, const int64 *vals, int nn)
{
	uint8	   *bits = sift_alloc0((nn + 7) / 8 + 1);

	for (int i = 0; i < nn; i++)
		if (vals[i] != 0)
			bits[i >> 3] |= 1 << (i & 7);
	sift_sb_append(s, (char *) bits, (nn + 7) / 8);
	pad8(s);
	sift_free(bits);
}

void
sift_chunk_put_int(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	int64	   *vals = sift_alloc(nn * sizeof(int64));
	int			k = 0;
	int64		min,
				max;

	for (int i = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			SIFT_INVARIANT(k < nn, "a column holds as many values as the rows it says have one");
			vals[k++] = col->i[i];
		}
	min = max = vals[0];
	for (int i = 1; i < nn; i++)
	{
		min = Min(min, vals[i]);
		max = Max(max, vals[i]);
	}
	if (col->hint == SIFT_CHINT_NUM64)
	{
		int64		scale = col->scale;

		sift_sb_append(s, (char *) &scale, 8);
		sift_put_cc_ints(s, vals, nn, opt);
		ref->enc |= SIFT_CENC_NUM64;
	}
	else if (col->hint == SIFT_CHINT_BOOL && min != max)
	{
		put_bits(s, vals, nn);
		ref->enc |= SIFT_CENC_BOOL_BITS;
	}
	else if (col->hint == SIFT_CHINT_LIST)
	{
		uint32		hdr[2] = {n, 0};
		uint32	   *boff = sift_alloc0(((n + 63) / 64 + 1) * sizeof(uint32));
		uint32		total = 0;
		int			vi = 0;

		for (int i = 0; i < n; i++)
		{
			if (i % 64 == 0)
				boff[i / 64] = total;
			if (col->isnull == NULL || !col->isnull[i])
				total += (uint32) vals[vi++];
		}
		boff[(n + 63) / 64] = total;
		hdr[1] = total;
		sift_sb_append(s, (char *) hdr, 8);
		sift_sb_append(s, (char *) boff, ((n + 63) / 64 + 1) * sizeof(uint32));
		pad8(s);
		sift_put_cc_ints(s, vals, nn, opt);
		ref->enc |= SIFT_CENC_LIST_LEN;
		sift_free(boff);
	}
	else if (min == max)
	{
		/* STOR[STOR-CODEC-02]: a constant column, bool included, is 8 B of INT_CONST, not a 1024 B bitmap (-1016 B/granule) */
		sift_sb_append(s, (char *) &min, 8);
		ref->enc |= SIFT_CENC_INT_CONST;
	}
	else
		ref->enc |= sift_put_cc_ints(s, vals, nn, opt);
	sift_free(vals);
}

static const double f8_p10[16] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
static const int64 f8_p10i[16] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, INT64CONST(10000000000), INT64CONST(100000000000),
								  INT64CONST(1000000000000), INT64CONST(10000000000000), INT64CONST(100000000000000), INT64CONST(1000000000000000)};

#define SIFT_ALP_SWEET			6755399441055744.0
#define SIFT_ALP_FAST_MAX		2.0e15
#define SIFT_F8_SAMPLE			64
#define SIFT_F8_BSS_SAMPLE		256
#define SIFT_F8_BSS_FRAME		18

static int	wrap_alg(const SiftChunkOpts *opt);

/* The nearest integer of x (ties to even) for |x| < SIFT_ALP_FAST_MAX: the sum with 2^51 + 2^52 leaves the integer in the low bits of the double */
static inline int64
alp_fast_round(double x)
{
	volatile double d = x + SIFT_ALP_SWEET;
	uint64		u;

	memcpy(&u, (const void *) &d, 8);
	return (int64) (u - UINT64CONST(0x4338000000000000));
}

typedef enum F8Pick
{
	F8_PICK_RAW = 0,
	F8_PICK_ALP,
	F8_PICK_ALPRD,
	F8_PICK_BSS
} F8Pick;

typedef struct F8Plan
{
	F8Pick		primary;
	F8Pick		fallback;
	uint8		tz;
	uint64		raw_cap;
} F8Plan;

/* The bytes of the decimal cascade for a sample of doubles: how many of them a power of ten holds exactly, and the width of the integers they become */
static uint64
f8_est_alp(const double *samp, uint32 sn, uint32 nn)
{
	uint32		best = 0;
	int			best_e = 0;
	int64		lo = INT64_MAX,
				hi = INT64_MIN;
	int			f = 0,
				bw;
	uint64		span;

	for (int e = 0; e < 16; e++)
	{
		uint32		cnt = 0;

		for (uint32 i = 0; i < sn; i++)
		{
			double		t = samp[i] * f8_p10[e],
						rt;
			uint64		a,
						b;

			if (!isfinite(t) || fabs(t) >= SIFT_ALP_FAST_MAX)
				continue;
			rt = (double) alp_fast_round(t) / f8_p10[e];
			memcpy(&a, &rt, 8);
			memcpy(&b, &samp[i], 8);
			cnt += a == b;
		}
		if (cnt > best)
		{
			best = cnt;
			best_e = e;
		}
	}
	/* STOR[STOR-CODEC-03]: the ALP estimate takes the 90% exact floor of alp_build, not 85% (no false ALP pick that falls back to raw) */
	if ((uint64) best * 100 < (uint64) sn * 90)
		return UINT64_MAX;
	for (uint32 i = 0; i < sn; i++)
	{
		double		t = samp[i] * f8_p10[best_e];

		if (isfinite(t) && fabs(t) < SIFT_ALP_FAST_MAX)
		{
			int64		r = alp_fast_round(t);

			lo = Min(lo, r);
			hi = Max(hi, r);
		}
	}
	while (f < best_e)
	{
		bool		all = true;

		for (uint32 i = 0; i < sn && all; i++)
		{
			double		t = samp[i] * f8_p10[best_e];

			all = !isfinite(t) || fabs(t) >= SIFT_ALP_FAST_MAX || alp_fast_round(t) % f8_p10i[f + 1] == 0;
		}
		if (!all)
			break;
		f++;
	}
	span = (uint64) (hi - lo) / (uint64) f8_p10i[f];
	bw = span == 0 ? 0 : 64 - __builtin_clzll(span);
	return 24 + ((uint64) nn * bw + 7) / 8 + ((uint64) nn * (sn - best) / sn) * 12;
}

/* The bytes of the split-bits encoding for a sample: for each left width the 8 most frequent left parts must hold all but a sixteenth of the sample; the narrowest total is the estimate */
static uint64
f8_est_alprd(const uint64 *ubits, uint32 sn, uint32 nn, uint8 tz)
{
	uint64		best = UINT64_MAX;

	for (int lb = SIFT_ALP_RD_MAX_LEFT; lb >= SIFT_ALP_RD_MIN_LEFT; lb--)
	{
		uint16		val[SIFT_F8_SAMPLE];
		uint32		cnt[SIFT_F8_SAMPLE];
		uint32		nv = 0,
					hits = 0,
					s_exc;
		int			rb = 64 - lb - tz;
		uint64		cand;

		if (rb < 1)
			continue;
		for (uint32 i = 0; i < sn; i++)
		{
			uint16		top = (uint16) (ubits[i] >> (64 - lb));
			uint32		d = 0;

			while (d < nv && val[d] != top)
				d++;
			if (d == nv)
			{
				val[nv] = top;
				cnt[nv++] = 0;
			}
			cnt[d]++;
		}
		for (uint32 k = 1; k < nv; k++)
			for (uint32 j = k; j > 0 && cnt[j - 1] < cnt[j]; j--)
			{
				uint32		t = cnt[j];

				cnt[j] = cnt[j - 1];
				cnt[j - 1] = t;
			}
		for (uint32 k = 0; k < Min(nv, SIFT_ALP_RD_DICT); k++)
			hits += cnt[k];
		s_exc = sn - hits;
		if (s_exc > sn / 16)
			continue;
		cand = 8 + 16 + ((uint64) nn * 3 + 7) / 8 + ((uint64) nn * rb + 7) / 8 + ((uint64) nn * s_exc / sn) * 4;
		best = Min(best, cand);
	}
	return best;
}

/* The bytes of the byte-split doubles of a sample under the algorithm that wraps the chunk */
static uint64
f8_est_bss(const double *vals, uint32 nn, int alg)
{
	uint8		bss[SIFT_F8_BSS_SAMPLE * 8],
				zout[SIFT_F8_BSS_SAMPLE * 8 + 256];
	uint32		sn = Min(nn, (uint32) SIFT_F8_BSS_SAMPLE);
	Size		zlen;

	if (alg <= 0 || !sift_colz_available(alg))
		return UINT64_MAX;
	for (uint32 i = 0; i < sn; i++)
	{
		uint64		u;

		memcpy(&u, &vals[(uint64) i * nn / sn], 8);
		for (int b = 0; b < 8; b++)
			bss[b * sn + i] = (uint8) (u >> (8 * b));
	}
	zlen = sift_colz_compress(alg, 0, bss, (Size) sn * 8, zout, sizeof(zout));
	/* STOR[STOR-CODEC-03]: the BSS estimate wraps a 256-value sample (2 KB) and leaves the fixed zstd frame header out of the extrapolation (bytes) */
	return zlen > 0 ? (uint64) (zlen - Min(zlen, (Size) SIFT_F8_BSS_FRAME)) * nn / sn + SIFT_F8_BSS_FRAME : UINT64_MAX;
}

/*
 * Which encoding holds the doubles of a chunk in the fewest bytes, from 64 of them spread over the chunk: the cascade of the codec for decimals, the split of the bits for doubles
 * that share their top bits, the byte planes wrapped in LZ4 or zstd for the rest. The first two decode several times faster than a wrapped chunk, so they win within 5% of it.
 */
static void
f8_select_plan(F8Plan *plan, const double *vals, uint32 nn, const SiftChunkOpts *opt)
{
	uint32		sn = Min(nn, (uint32) SIFT_F8_SAMPLE);
	double		samp[SIFT_F8_SAMPLE];
	uint64		ubits[SIFT_F8_SAMPLE],
				all_or = 0;
	uint64		est_alp,
				est_rd,
				est_bss,
				fam;
	int			walg = SIFT_TIER_IS_COLD(opt->tier) ? wrap_alg(opt) : 0;
	uint8		tz;

	for (uint32 i = 0; i < sn; i++)
	{
		samp[i] = vals[(uint64) i * nn / sn];
		memcpy(&ubits[i], &samp[i], 8);
	}
	for (uint32 i = 0; i < nn; i++)
	{
		uint64		u;

		memcpy(&u, &vals[i], 8);
		all_or |= u;
	}
	tz = all_or == 0 ? 0 : (uint8) Min(SIFT_ALPRD_MAX_TZ, __builtin_ctzll(all_or));
	plan->tz = tz;
	plan->raw_cap = (uint64) nn * 8 * 95 / 100;
	est_alp = f8_est_alp(samp, sn, nn);
	est_rd = f8_est_alprd(ubits, sn, nn, tz);
	est_bss = f8_est_bss(vals, nn, walg);
	fam = Min(est_alp, est_rd);
	if (fam == UINT64_MAX && est_bss == UINT64_MAX)
		plan->primary = F8_PICK_RAW;
	else if (fam != UINT64_MAX && (est_bss == UINT64_MAX || fam <= est_bss + est_bss / 20))
		plan->primary = est_alp <= est_rd ? F8_PICK_ALP : F8_PICK_ALPRD;
	else
		plan->primary = F8_PICK_BSS;
	if (Min(fam, est_bss) >= plan->raw_cap)
		plan->primary = F8_PICK_RAW;
	plan->fallback = walg != 0 && plan->primary != F8_PICK_BSS && plan->primary != F8_PICK_RAW ? F8_PICK_BSS : F8_PICK_RAW;
}

static void
put_rd_bits(uint8 *dst, uint64 at, uint64 v, int w)
{
	uint64		x = v << (at & 7);
	uint8	   *p = dst + (at >> 3);

	for (int j = 0; j * 8 < w + (int) (at & 7); j++)
		p[j] |= (uint8) (x >> (8 * j));
}

/* The 8 most frequent of the left parts of 16 bits (value, count) of a chunk once each is cut to its top lb bits, merged from the distinct ones in order of value */
typedef struct RdTop
{
	int			n;
	uint16		left[SIFT_ALP_RD_DICT];
	uint32		cnt[SIFT_ALP_RD_DICT];
} RdTop;

static void
rd_top_add(RdTop *t, uint16 left, uint32 cnt)
{
	int			j;

	if (t->n == SIFT_ALP_RD_DICT)
	{
		if (cnt <= t->cnt[t->n - 1])
			return;
		t->n--;
	}
	for (j = t->n++; j > 0 && t->cnt[j - 1] < cnt; j--)
	{
		t->left[j] = t->left[j - 1];
		t->cnt[j] = t->cnt[j - 1];
	}
	t->left[j] = left;
	t->cnt[j] = cnt;
}

static void
rd_top_of(RdTop *t, const uint16 *pv, const uint32 *pn, uint32 nd, int shift)
{
	uint32		i = 0;

	t->n = 0;
	while (i < nd)
	{
		uint16		key = pv[i] >> shift;
		uint32		cnt = 0;

		for (; i < nd && (uint16) (pv[i] >> shift) == key; i++)
			cnt += pn[i];
		rd_top_add(t, key, cnt);
	}
}

static uint64
rd_body_len(int nn, int rb, uint32 n_exc)
{
	return 8 + 2 * SIFT_ALP_RD_DICT + MAXALIGN(((uint64) nn * 3 + 7) / 8) + MAXALIGN(((uint64) nn * rb + 7) / 8) + MAXALIGN((uint64) n_exc * 4);
}

/*
 * A double of no short decimal as the bits that vary and the bits that do not: the top lb bits (8 to 16) are each a 3 bit code of a table of the 8 most frequent ones, the bottom tz bits
 * (zero in every value) are dropped, and the 64 - lb - tz bits between are packed as they are. The rows whose top bits are not in the table (at most a sixteenth) are listed behind,
 * as the row and the top bits. The width lb is the one with the narrowest body over all the values. Every bit pattern (NaN payloads, -0, subnormals) comes back as it went in. The body is
 * the header (lb, the table size, tz, a flag for the list, the length of the list), the table of 8 left parts, the codes, the right parts, each stream padded to 8 bytes, then the rows of
 * the list followed by their top bits (padded to 8); false when the body is not 5% smaller than the raw doubles.
 */
static bool
put_alp_rd(StringInfo s, const double *vals, int nn, int tz, uint64 raw_cap)
{
	uint32	   *hist = sift_alloc0(((Size) 1 << 16) * sizeof(uint32));
	uint16	   *pv = sift_alloc(Min((Size) nn, (Size) 1 << 16) * sizeof(uint16));
	uint32	   *pn = sift_alloc(Min((Size) nn, (Size) 1 << 16) * sizeof(uint32));
	uint32		ndist = 0,
				n_exc = 0;
	uint64		best_len = UINT64_MAX;
	RdTop		best = {0},
				top;
	int			lb,
				best_lb = 0,
				rb;
	uint8		hdr[8] = {0};
	uint16		dict[SIFT_ALP_RD_DICT] = {0};
	bool		ok;

	for (int i = 0; i < nn; i++)
	{
		uint64		u;

		memcpy(&u, &vals[i], 8);
		hist[u >> 48]++;
	}
	for (uint32 v = 0; v < ((uint32) 1 << 16); v++)
		if (hist[v] != 0)
		{
			pv[ndist] = (uint16) v;
			pn[ndist++] = hist[v];
		}
	for (lb = SIFT_ALP_RD_MAX_LEFT; lb >= SIFT_ALP_RD_MIN_LEFT; lb--)
	{
		uint32		hits = 0;
		uint64		len;

		if (64 - lb - tz < 1)
			continue;
		rd_top_of(&top, pv, pn, ndist, 16 - lb);
		for (int k = 0; k < top.n; k++)
			hits += top.cnt[k];
		len = rd_body_len(nn, 64 - lb - tz, nn - hits);
		if (nn - hits <= (uint32) nn / 16 && len < best_len)
		{
			best_len = len;
			best_lb = lb;
			best = top;
		}
	}
	ok = best_lb != 0 && best_len < raw_cap;
	if (ok)
	{
		uint16	   *exc_pos = sift_alloc((nn / 16 + 1) * sizeof(uint16)),
				   *exc_left = sift_alloc((nn / 16 + 1) * sizeof(uint16));
		Size		clen = MAXALIGN(((uint64) nn * 3 + 7) / 8),
					rlen;
		uint8	   *codes = sift_alloc0(clen),
				   *rights;

		lb = best_lb;
		rb = 64 - lb - tz;
		rlen = MAXALIGN(((uint64) nn * rb + 7) / 8);
		rights = sift_alloc0(rlen);
		memcpy(dict, best.left, best.n * sizeof(uint16));
		for (int i = 0; i < nn; i++)
		{
			uint64		u;
			uint16		left;
			int			d = 0;

			memcpy(&u, &vals[i], 8);
			left = (uint16) (u >> (64 - lb));
			while (d < best.n && dict[d] != left)
				d++;
			if (d == best.n)
			{
				exc_pos[n_exc] = (uint16) i;
				exc_left[n_exc++] = left;
				d = 0;
			}
			put_rd_bits(codes, (uint64) i * 3, (uint64) d, 3);
			put_rd_bits(rights, (uint64) i * rb, (u >> tz) & ((UINT64CONST(1) << rb) - 1), rb);
		}
		hdr[0] = (uint8) lb;
		hdr[1] = (uint8) best.n;
		hdr[2] = (uint8) tz;
		hdr[3] = n_exc > 0 ? SIFT_ALPRD_HAS_EXC : 0;
		memcpy(hdr + 4, &n_exc, 4);
		sift_sb_append(s, (char *) hdr, 8);
		sift_sb_append(s, (char *) dict, sizeof(dict));
		sift_sb_append(s, (char *) codes, clen);
		sift_sb_append(s, (char *) rights, rlen);
		if (n_exc > 0)
		{
			sift_sb_append(s, (char *) exc_pos, n_exc * 2);
			sift_sb_append(s, (char *) exc_left, n_exc * 2);
			pad8(s);
		}
		sift_free(exc_pos);
		sift_free(exc_left);
		sift_free(codes);
		sift_free(rights);
	}
	sift_free(hist);
	sift_free(pv);
	sift_free(pn);
	return ok;
}

/* The decimals as the cascade of the codec on their order-preserving keys; false when the blob is not 5% smaller than the raw doubles */
static bool
put_alp(StringInfo s, const double *vals, int nn, const SiftChunkOpts *opt, uint64 raw_cap)
{
	int64	   *keys = sift_alloc(nn * sizeof(int64));
	SiftCcPolicy pol;
	StringInfoData blob;
	bool		ok;

	for (int i = 0; i < nn; i++)
		keys[i] = sift_col_f8_key(vals[i]);
	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	sift_sb_init(&blob);
	sift_cc_encode(&blob, SIFT_CC_F8, keys, nn, NULL, &pol, NULL);
	ok = 8 + (uint64) blob.len < raw_cap;
	if (ok)
		sift_cc_blob_put(s, &blob);
	sift_free(keys);
	sift_free(blob.data);
	return ok;
}

static void
put_f8_bss(StringInfo s, const double *vals, int nn)
{
	uint8	   *planes = sift_alloc((Size) nn * 8);

	for (int i = 0; i < nn; i++)
		for (int b = 0; b < 8; b++)
			planes[b * nn + i] = ((const uint8 *) &vals[i])[b];
	sift_sb_append(s, (char *) planes, nn * 8);
	sift_free(planes);
}

typedef struct BssFrames
{
	const double *vals;
	int			alg;
} BssFrames;

static void
put_bss_frame(StringInfo frame, int first, int cnt, void *arg)
{
	const BssFrames *bf = arg;

	sift_frame_f8_put(frame, bf->vals + first, cnt, bf->alg);
}

/* One encoding of the plan; false when the chunk did not shrink under it (nothing is appended then) */
static bool
put_f8_pick(StringInfo s, F8Pick pick, const double *vals, int nn, const F8Plan *plan, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	switch (pick)
	{
		case F8_PICK_ALP:
			if (!put_alp(s, vals, nn, opt, plan->raw_cap))
				return false;
			ref->enc |= SIFT_CENC_ALP;
			return true;
		case F8_PICK_ALPRD:
			if (!put_alp_rd(s, vals, nn, plan->tz, plan->raw_cap))
				return false;
			ref->enc |= SIFT_CENC_ALP_RD;
			return true;
		case F8_PICK_BSS:
			if (opt->framed)
			{
				sift_frames_put(s, nn, put_bss_frame, &(BssFrames) {vals, wrap_alg(opt)});
				ref->enc |= SIFT_CENC_FLOAT_BSS | SIFT_ENC_FRAMED;
				return true;
			}
			put_f8_bss(s, vals, nn);
			ref->enc |= SIFT_CENC_FLOAT_BSS;
			return true;
		case F8_PICK_RAW:
			break;
	}
	sift_sb_append(s, (char *) vals, (uint64) nn * 8);
	ref->enc |= SIFT_CENC_FLOAT_RAW;
	return true;
}

static void
put_f8(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	double	   *vals = sift_alloc(nn * sizeof(double));
	int			k = 0;
	F8Plan		plan = {.primary = F8_PICK_RAW,.fallback = F8_PICK_RAW};

	for (int i = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			SIFT_INVARIANT(k < nn, "a column holds as many values as the rows it says have one");
			vals[k++] = col->f[i];
		}
	if (nn >= 8)
		f8_select_plan(&plan, vals, nn, opt);
	if (!put_f8_pick(s, plan.primary, vals, nn, &plan, ref, opt))
		put_f8_pick(s, plan.fallback, vals, nn, &plan, ref, opt);
	sift_free(vals);
}

/* The chunk wrapped: algorithm and raw length in the first 8 bytes, then the compressed bytes; only when it is at least an eighth smaller (deep, the cold level: two twenty-fifths) */
static void
wrap_chunk(StringInfo body, int alg, SiftChunkRef *ref, bool probe, bool deep)
{
	Size		cap,
				clen;
	char	   *buf;

	if (alg <= 0 || body->len < (probe ? SIFT_WRAP_HOT_MIN : SIFT_WRAP_MIN) || !sift_colz_available(alg))
		return;
	cap = sift_colz_bound(alg, body->len);
	buf = sift_alloc(cap);
	if (probe)
	{
		clen = sift_colz_compress(alg, 0, body->data, 512, buf, cap);
		if (clen == 0 || clen > 512 - 512 / 4)
		{
			sift_free(buf);
			return;
		}
	}
	clen = sift_colz_compress(alg, 0, body->data, body->len, buf, cap);
	if (clen != 0 && 8 + clen <= body->len - (deep ? body->len * 2 / 25 : body->len / 8))
	{
		uint32		hdr[2] = {alg, body->len};

		sift_sb_reset(body);
		sift_sb_append(body, (char *) hdr, 8);
		sift_sb_append(body, buf, clen);
		ref->flags |= SIFT_CHUNK_COMPRESSED;
	}
	sift_free(buf);
}

bool
sift_key_row_present(bool isnull, uint8 tag)
{
	return !isnull || (tag != SIFT_COLTAG_UNKNOWN && tag != SIFT_COLTAG_NONE);
}

static bool
key_row_present(const SiftColIn *col, int i)
{
	return sift_key_row_present(col->isnull != NULL && col->isnull[i], col->tag != NULL ? col->tag[i] : 0);
}

/* Which rows of the leaf of a key have it (SIFT_ENC_KEY_*): the enc says it when no row differs from the rows with a value, else the smallest of a list of the rows that have it, a list of the rows that do not, and a bit per row */
static void
put_key_present(StringInfo s, const SiftColIn *col, int n, SiftChunkRef *ref)
{
	int			npresent = 0;
	bool		eq_valid = true;
	uint32		list_bytes,
				neg_bytes,
				bitmap_bytes = (n + 7) / 8;
	SiftKeyPresentHdr h;

	for (int i = 0; i < n; i++)
	{
		bool		present = key_row_present(col, i);

		npresent += present;
		eq_valid &= present == (col->isnull == NULL || !col->isnull[i]);
	}
	if (npresent == 0)
		ref->enc |= SIFT_ENC_KEY_ALL_ABSENT;
	else if (npresent == n)
		ref->enc |= SIFT_ENC_KEY_ALL_PRESENT;
	else if (eq_valid)
		ref->enc |= SIFT_ENC_KEY_EQ_VALID;
	else
	{
		list_bytes = 2 * (uint32) npresent;
		neg_bytes = 2 * (uint32) (n - npresent);
		ref->enc |= SIFT_ENC_KEY_HAS_MASK;
		if (list_bytes <= neg_bytes && list_bytes <= bitmap_bytes)
			h = (SiftKeyPresentHdr) {.mode = SIFT_KP_ARRAY16,.count = npresent,.npresent = npresent};
		else if (neg_bytes <= bitmap_bytes)
			h = (SiftKeyPresentHdr) {.mode = SIFT_KP_NEG_ARRAY16,.count = n - npresent,.npresent = npresent};
		else
			h = (SiftKeyPresentHdr) {.mode = SIFT_KP_BITMAP,.count = n,.npresent = npresent};
		sift_sb_append(s, (char *) &h, sizeof(h));
		if (h.mode == SIFT_KP_BITMAP)
		{
			uint8	   *bm = sift_alloc0(bitmap_bytes);

			for (int i = 0; i < n; i++)
				if (key_row_present(col, i))
					bm[i >> 3] |= 1 << (i & 7);
			sift_sb_append(s, (char *) bm, bitmap_bytes);
			sift_free(bm);
		}
		else
			for (int i = 0; i < n; i++)
				if (key_row_present(col, i) == (h.mode == SIFT_KP_ARRAY16))
				{
					uint16		idx = i;

					sift_sb_append(s, (char *) &idx, 2);
				}
		pad8(s);
	}
}

/* The encodings whose bodies are not a cascade of the codec, which has its own wrapper: the chunk as a whole may be wrapped */
static bool
wrappable(uint16 enc)
{
	if (enc & SIFT_ENC_FRAMED)
		return false;
	enc &= SIFT_ENC_MASK;
	return enc == SIFT_CENC_STR_INLINE || enc == SIFT_CENC_STR_VIEW || enc == SIFT_CENC_FLOAT_BSS || enc == SIFT_CENC_FLOAT_RAW || enc == SIFT_CENC_UUID16 || enc == SIFT_CENC_NUM128 ||
		enc == SIFT_CENC_INTERVAL16 || enc == SIFT_CENC_BOOL_BITS;
}

static int
wrap_alg(const SiftChunkOpts *opt)
{
	if (opt->alg < 0)
		return 0;
	if (!SIFT_TIER_IS_COLD(opt->tier))
		return sift_colz_available(SIFT_CC_ALG_LZ4) ? SIFT_CC_ALG_LZ4 : 0;
	if (opt->alg > 0)
		return opt->alg;
	return sift_colz_available(SIFT_CC_ALG_ZSTD) ? SIFT_CC_ALG_ZSTD : sift_colz_available(SIFT_CC_ALG_LZ4) ? SIFT_CC_ALG_LZ4 : 0;
}

void
sift_chunk_put(StringInfo body, const SiftColIn *col, int n, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	StringInfoData val,
				keys,
				data,
				out;
	bool		tags = has_tags(col, n);
	int			nn;

	sift_sb_init(&val);
	sift_sb_init(&keys);
	sift_sb_init(&data);
	nn = put_validity(&val, col->isnull, n, ref);
	if (col->key_leaf || col->hint == SIFT_CHINT_KEY_PRESENT)
		put_key_present(&keys, col, n, ref);
	if (nn == 0)
		ref->enc |= SIFT_CENC_ALL_NULL;
	else if (col->hint == SIFT_CHINT_KEY_PRESENT)
		ref->enc |= SIFT_CENC_KEY_PRESENT;
	else if (col->kind == SIFT_CELL_INT)
		sift_chunk_put_int(&data, col, n, nn, ref, opt);
	else if (col->kind == SIFT_CELL_F8)
		put_f8(&data, col, n, nn, ref, opt);
	else if (col->promo == NULL || !sift_chunk_put_promo(&data, col, n, nn, ref, opt))
		sift_chunk_put_str(&data, col, n, nn, ref, opt);
	sift_sb_init(&out);
	sift_sb_append(&out, val.data, val.len);
	if (tags)
		put_exceptions(&out, col, n, ref, opt);
	sift_sb_append(&out, keys.data, keys.len);
	sift_sb_append(&out, data.data, data.len);
	/* STOR[STOR-CODEC-05]: a cold uuid chunk that is still raw is random ids, which no wrapper shrinks (no zstd pass over 128 KB) */
	if (wrappable(ref->enc) && !(col->hint == SIFT_CHINT_UUID && SIFT_TIER_IS_COLD(opt->tier)))
		wrap_chunk(&out, wrap_alg(opt), ref, !SIFT_TIER_IS_COLD(opt->tier), opt->tier == SIFT_TIER_COLD_L2);
	sift_sb_append(body, out.data, out.len);
	sift_free(val.data);
	sift_free(keys.data);
	sift_free(data.data);
	sift_free(out.data);
}
