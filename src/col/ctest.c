/*
 * col/ctest.c - L1 of the column engine: chunk tests, a predicate as a test in the key domain, run on a chunk or its zone.
 */
#include "core/host.h"

#include "core/cmp.h"
#include "col/enc.h"
#include "col/enc_int.h"

/* ---------------------------------------------------------------------------
 * 7. Chunk tests
 *
 * A predicate becomes a test in the key domain. Floats need no rounding of the constant: F4 keys are the F8 keys of the promoted value, so
 * comparing a float4 column against a float8 constant in the key domain is exactly PostgreSQL's float48 comparison.
 * ------------------------------------------------------------------------- */

static bool
set_has(const SiftCTest *t, int64 k)
{
	return bsearch(&k, t->set, t->nset, sizeof(int64), sift_cmp_i64) != NULL;
}

static void
ctest_range(SiftCTest *t, int64 lo, int64 hi)
{
	t->kind = lo > hi ? SIFT_CT_NONE : SIFT_CT_RANGE;
	t->lo = lo;
	t->hi = hi;
}

/* Sorted distinct keys -> NONE/NONNULL (none), a range (one) or a set */
static void
ctest_keys(SiftCTest *t, int64 *keys, uint32 n, bool negate)
{
	uint32		m = 0;

	qsort(keys, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (m == 0 || keys[m - 1] != keys[i])
			keys[m++] = keys[i];
	if (m == 0)
		t->kind = negate ? SIFT_CT_NONNULL : SIFT_CT_NONE;
	else if (m == 1)
	{
		ctest_range(t, keys[0], keys[0]);
		t->kind = negate ? SIFT_CT_NRANGE : SIFT_CT_RANGE;
	}
	else
	{
		t->kind = negate ? SIFT_CT_NSET : SIFT_CT_SET;
		t->set = keys;
		t->nset = m;
		t->lo = keys[0];
		t->hi = keys[m - 1];
	}
}

/* The keys a constant stands for: PostgreSQL compares -0 and +0 equal, so a zero spans the two keys of the float domain */
static void
const_span(const SiftColDef *def, const SiftColValue *v, int64 *lo, int64 *hi)
{
	*lo = *hi = def->store == SIFT_COLS_INT ? v->v.i : sift_col_f8_key(v->v.f);
	if (def->store != SIFT_COLS_INT && v->v.f == 0)
	{
		*lo = sift_col_f8_key(-0.0);
		*hi = sift_col_f8_key(0.0);
	}
}

static void
ctest_make_num(const SiftColDef *def, const SiftColPred *p, SiftCTest *t)
{
	int64		lo = 0,
				hi = 0;

	if (p->nvals > 0)
		const_span(def, &p->vals[0], &lo, &hi);
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
			ctest_range(t, lo, hi);
			t->kind = p->op == SIFT_COLOP_EQ ? SIFT_CT_RANGE : SIFT_CT_NRANGE;
			break;
		case SIFT_COLOP_LT:
			if (lo == PG_INT64_MIN)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, PG_INT64_MIN, lo - 1);
			break;
		case SIFT_COLOP_LE:
			ctest_range(t, PG_INT64_MIN, hi);
			break;
		case SIFT_COLOP_GT:
			if (hi == PG_INT64_MAX)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, hi + 1, PG_INT64_MAX);
			break;
		case SIFT_COLOP_GE:
			ctest_range(t, lo, PG_INT64_MAX);
			break;
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			{
				int64	   *keys = palloc(Max(p->nvals, 1) * 2 * sizeof(int64));
				int			m = 0;

				for (int i = 0; i < p->nvals; i++)
				{
					const_span(def, &p->vals[i], &lo, &hi);
					keys[m++] = lo;
					if (hi != lo)
						keys[m++] = hi;
				}
				ctest_keys(t, keys, m, p->op == SIFT_COLOP_NOTIN);
			}
			break;
		default:
			SIFT_INVARIANT(false, "sift_col: operator %d is not defined on a numeric column", p->op);
	}
}

/* Turns the bit set of matching codes into a LUT test: none -> NONE, all -> NONNULL, else the LUT with its bounds and fold */
static void
ctest_lut_finish(SiftCTest *t, uint64 *lut, uint32 ncodes)
{
	uint32		count = 0;

	t->kind = SIFT_CT_LUT;
	t->lo = PG_INT64_MAX;
	t->hi = PG_INT64_MIN;
	for (uint32 w = 0; w < SIFT_COL_NWORDS(ncodes); w++)
		if (lut[w])
		{
			count += pg_popcount64(lut[w]);
			t->lo = Min(t->lo, (int64) w * 64 + pg_rightmost_one_pos64(lut[w]));
			t->hi = Max(t->hi, (int64) w * 64 + pg_leftmost_one_pos64(lut[w]));
			t->fold[w & 3] |= lut[w];		/* code & 255 is the bit of the word w & 3 */
		}
	t->fold_valid = true;
	t->lut = lut;
	t->lutbits = SIFT_COL_NWORDS(ncodes) * 64;
	if (count == 0)
		t->kind = SIFT_CT_NONE;
	else if (count == ncodes)
		t->kind = SIFT_CT_NONNULL;
}

static void
ctest_make_str(SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	uint64	   *lut;
	int64	   *codes = NULL;
	uint32		nfound = 0;

	SIFT_INVARIANT(dict != NULL, "sift_col: a test on a text column needs its dictionary");
	t->pred = p;
	lut = palloc0(SIFT_COL_NWORDS(dict->n) * sizeof(uint64));
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			codes = palloc(Max(p->nvals, 1) * sizeof(int64));
			for (int i = 0; i < (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_NE ? 1 : p->nvals); i++)
			{
				int64		code = sift_cdict_find(dict, p->vals[i].v.s, p->vals[i].len);

				if (code >= 0)
					codes[nfound++] = code;
			}
			ctest_keys(t, codes, nfound, p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN);
			if (t->kind == SIFT_CT_RANGE)
			{
				t->fold[(t->lo & 255) >> 6] = UINT64CONST(1) << (t->lo & 63);
				t->fold_valid = true;
			}
			if (t->kind == SIFT_CT_SET || t->kind == SIFT_CT_NSET)
			{
				/* several codes: the same answer as a LUT over the dictionary */
				SiftCTest	set = *t;

				memset(t, 0, sizeof(*t));
				for (uint32 c = 0; c < dict->n; c++)
					if (set_has(&set, c) != (set.kind == SIFT_CT_NSET))
						lut[c >> 6] |= UINT64CONST(1) << (c & 63);
				ctest_lut_finish(t, lut, dict->n);
			}
			return;
		default:
			sift_cdict_match(dict, p->op, &p->vals[0], lut);
			ctest_lut_finish(t, lut, dict->n);
	}
}

void
sift_ctest_make_strs(const SiftColPred *p, SiftCTest *t)
{
	memset(t, 0, sizeof(*t));
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		t->kind = p->op == SIFT_COLOP_ISNULL ? SIFT_CT_ISNULL : SIFT_CT_NOTNULL;
	else if (p->nvals < 1 && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN)
		SIFT_INVARIANT(false, "sift_col: predicate without a constant");
	else if (p->nvals < 1)
		t->kind = p->op == SIFT_COLOP_IN ? SIFT_CT_NONE : SIFT_CT_NONNULL;
	else
		t->kind = SIFT_CT_STRS;
	t->pred = p;
}

void
sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	memset(t, 0, sizeof(*t));
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		t->kind = p->op == SIFT_COLOP_ISNULL ? SIFT_CT_ISNULL : SIFT_CT_NOTNULL;
	else if (def->store == SIFT_COLS_VAR)
		sift_ctest_make_strs(p, t);
	else if (p->nvals < 1 && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN)
		SIFT_INVARIANT(false, "sift_col: predicate without a constant");
	else if (def->store == SIFT_COLS_STR)
		ctest_make_str(dict, p, t);
	else
		ctest_make_num(def, p, t);
}

/* Can a unit with inline strings, whose smallest and largest prefix keys are z->min and z->max, hold a match of p (STR predicates; conservative) */
static bool
zone_inline_matches(const SiftColPred *p, const SiftCZone *z)
{
	const SiftColValue *c = &p->vals[0];
	int64		k = p->nvals > 0 ? sift_col_str_prefix(c->v.s, c->len) : 0;

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return k >= z->min && k <= z->max;
		case SIFT_COLOP_IN:
			for (int i = 0; i < p->nvals; i++)
			{
				k = sift_col_str_prefix(p->vals[i].v.s, p->vals[i].len);
				if (k >= z->min && k <= z->max)
					return true;
			}
			return false;
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			return z->min <= k;
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
			return z->max >= k;
		case SIFT_COLOP_PREFIX:
			{
				char		pad[8];

				memset(pad, 0xFF, sizeof(pad));
				memcpy(pad, c->v.s, Min(c->len, 8));
				return z->max >= k && z->min <= (c->len >= 8 ? k : sift_col_str_prefix(pad, 8));
			}
		default:					/* NE, NOTIN */
			return true;
	}
}

bool
sift_ctest_zone(const SiftCTest *t, const SiftCZone *z)
{
	if (z->flags & SIFT_COLZ_OVERFLOW)	/* the zone does not describe the heap: the recheck of the unit's overflow rows may find the constant */
		return true;
	if (t->kind == SIFT_CT_NONE && !(z->flags & SIFT_COLZ_INLINE))	/* NONE: no constant is in the dictionary, which an inline unit does not use */
		return false;
	if (t->kind == SIFT_CT_ISNULL)
		return z->nnull > 0;
	if (z->nnull == z->nvals)
		return false;
	if (z->flags & SIFT_COLZ_INLINE)
		return t->pred == NULL || zone_inline_matches(t->pred, z);
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
		case SIFT_CT_LUT:
		case SIFT_CT_SET:
			if (z->max < t->lo || z->min > t->hi)
				return false;
			if (t->kind == SIFT_CT_SET)
			{
				const int64 *first = t->set;
				uint32		n = t->nset;

				while (n > 0)		/* the first member >= z->min */
				{
					uint32		h = n / 2;

					if (first[h] < z->min)
					{
						first += h + 1;
						n -= h + 1;
					}
					else
						n = h;
				}
				return first < t->set + t->nset && *first <= z->max;
			}
			if (t->fold_valid)
				return ((t->fold[0] & z->set[0]) | (t->fold[1] & z->set[1]) | (t->fold[2] & z->set[2]) | (t->fold[3] & z->set[3])) != 0;
			return true;
		case SIFT_CT_NRANGE:
			return z->min < t->lo || z->max > t->hi;
		case SIFT_CT_NSET:
			return !(z->min == z->max && set_has(t, z->min));
		default:					/* NONNULL, NOTNULL */
			return true;
	}
}

/* The test applied to one key: the specification of the vector paths below (and their fallback) */
static bool
key_matches(const SiftCTest *t, int64 k)
{
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
			return k >= t->lo && k <= t->hi;
		case SIFT_CT_NRANGE:
			return k < t->lo || k > t->hi;
		case SIFT_CT_LUT:
			return k >= 0 && k < t->lutbits && sift_col_bit_test(t->lut, (uint32) k);
		case SIFT_CT_SET:
			return set_has(t, k);
		default:					/* NSET */
			return !set_has(t, k);
	}
}

static void
bits_fill(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
		bits[w] = w == n / 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0);
}

/* The widest payload value an encoding can hold */
static uint64
codec_maxval(const SiftCChunk *c)
{
	return c->enc == SIFT_COLENC_CONST ? 0 : c->bits >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << c->bits) - 1;
}

/* tmp (padded words of n bits) = rows whose key is in [lo, hi] (null slots hold base and are removed by the caller) */
static void
chunk_range(const SiftCChunk *c, int64 lo, int64 hi, uint64 *tmp)
{
	uint64		ulo,
				uhi,
				maxv = codec_maxval(c);
	uint32		nw = SIFT_COL_NWORDS(c->n);

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_filter(cc, lo, hi, tmp);
		sift_cc_close(cc);
		return;
	}
	if (lo > hi || hi < c->base)
		return;
	ulo = lo <= c->base ? 0 : (uint64) lo - (uint64) c->base;
	uhi = Min((uint64) hi - (uint64) c->base, maxv);
	if (ulo > uhi)
		return;
	if (c->enc == SIFT_COLENC_CONST)
		bits_fill(tmp, c->n);
	else if (c->enc == SIFT_COLENC_FOR)
		sift_colk->range[codec_slot(c->bits)](c->payload, c->n, ulo, uhi - ulo, tmp);
	else
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			sift_colk->range[SIFT_COLW_32](blk, m, ulo, uhi - ulo, tmp + s / 64);
		}
	}
}

/* tmp = rows matching, evaluated key by key (sets of many members, and what has no vector path) */
static void
chunk_scalar(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	sift_cchunk_decode(c, key);
	for (uint32 i = 0; i < c->n; i++)
		if (key_matches(t, key[i]))
			tmp[i >> 6] |= UINT64CONST(1) << (i & 63);
	pfree(key);
}

/* dst (nbits) = src (srcbits) shifted down by `by` bits, zero past the end of src */
static void
lut_shift(const uint64 *src, uint64 srcbits, uint64 by, uint64 *dst, uint32 nbits)
{
	uint64		srcwords = (srcbits + 63) / 64;

	for (uint32 w = 0; w < words_of(nbits); w++)
	{
		uint64		i = (by >> 6) + w,
					lo = i < srcwords ? src[i] : 0,
					hi = i + 1 < srcwords ? src[i + 1] : 0;

		dst[w] = (by & 63) ? (lo >> (by & 63)) | (hi << (64 - (by & 63))) : lo;
	}
}

void
sift_ccodes_below(const uint32 *codes, uint32 n, uint32 limit)
{
	if (sift_cck->max32(codes, n) >= limit)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: a code is past the table it indexes (%u entries)", limit)));
}

/* chunk_lut of a CC chunk: the codec hands the codes as offsets from its minimum */
static void
cc_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	SiftCc	   *cc = cc_open(c, false);
	int64		min = sift_cc_min(cc);
	uint32	   *off;
	uint64	   *shifted;
	uint32		sbits;

	if (min >= 0 && (uint64) min < t->lutbits)
	{
		off = palloc(SIFT_CC_CAP32(c->n) * sizeof(uint32));
		sbits = t->lutbits - (uint32) min;
		shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
		lut_shift(t->lut, t->lutbits, min, shifted, sbits);
		if (sift_cc_decode_off32(cc, off))
		{
			if (sift_cc_off32_bound(cc) >= sbits)
				sift_ccodes_below(off, c->n, sbits);
			sift_colk->lut[2](off, c->n, shifted, tmp);
		}
		else
			chunk_scalar(t, c, tmp);
		pfree(shifted);
		pfree(off);
	}
	sift_cc_close(cc);
}

/* tmp = rows whose code is in the LUT; the chunk holds code - base, so the LUT is shifted by base */
static void
chunk_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				sbits;
	uint64	   *shifted;

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		cc_lut(t, c, tmp);
		return;
	}
	if (c->base < 0 || (uint64) c->base >= t->lutbits)
		return;
	if (c->enc == SIFT_COLENC_CONST)
	{
		if (sift_col_bit_test(t->lut, (uint32) c->base))
			bits_fill(tmp, c->n);
		return;
	}
	if (c->enc == SIFT_COLENC_FOR && c->bits == 64)		/* codes are 32 bits: only a foreign writer makes this */
	{
		chunk_scalar(t, c, tmp);
		return;
	}
	/* a code of at most 16 bits has a bit of its own in the table, whatever the chunk holds; wider codes are checked against the table */
	sbits = c->bits <= 16 ? 1U << c->bits : t->lutbits - (uint32) c->base;
	shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
	lut_shift(t->lut, t->lutbits, c->base, shifted, sbits);
	if (c->enc == SIFT_COLENC_FOR && c->bits <= 16)
		sift_colk->lut[codec_slot(c->bits)](c->payload, c->n, shifted, tmp);
	else if (c->enc == SIFT_COLENC_FOR)
	{
		sift_ccodes_below((const uint32 *) c->payload, c->n, sbits);
		sift_colk->lut[2](c->payload, c->n, shifted, tmp);
	}
	else if (c->enc == SIFT_COLENC_PACK)
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			if (c->bits > 16)
				sift_ccodes_below(blk, m, sbits);
			sift_colk->lut[2](blk, m, shifted, tmp + s / 64);
		}
	}
	pfree(shifted);
}

/* Whole-string comparison of a STRS or ISTR chunk's row with the predicate's constants (EQ and IN: equal to one of them) */
static bool
strs_match(SiftColOp op, const SiftColPred *p, const char *s, uint32 len)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return len == p->vals[0].len && memcmp(s, p->vals[0].v.s, len) == 0;
		case SIFT_COLOP_IN:
			for (int i = 0; i < p->nvals; i++)
				if (len == p->vals[i].len && memcmp(s, p->vals[i].v.s, len) == 0)
					return true;
			return false;
		default:
			return sift_cstr_matches(op, s, len, &p->vals[0]);
	}
}

/* IN lists longer than this are verified on every row: the prefix filter would run once per constant */
#define STRS_PREFIX_MAXIN	16

/*
 * The rows of an inline chunk that can satisfy op (EQ, IN, a range or PREFIX) according to the prefix blob alone, on the encoded form (the
 * codec's filter: bit-packed keys compare in registers, a constant blob never expands). The string list is not touched. NULL: the operator
 * gives the prefix no use, every row is a candidate.
 */
static uint64 *
istr_candidates(const SiftCChunk *c, SiftColOp op, const SiftColPred *p)
{
	const uint8 *pre,
			   *strs;
	uint32		prelen,
				strslen,
				nw = SIFT_COL_NWORDS(c->n);
	SiftCc	   *cc;
	uint64	   *cand,
			   *one;
	int64		k;

	if ((op == SIFT_COLOP_IN && p->nvals > STRS_PREFIX_MAXIN) || !istr_split(c, &pre, &prelen, &strs, &strslen))
		return NULL;
	cc = sift_cc_open_rows(CurrentMemoryContext, pre, prelen, false, c->n);
	cand = palloc0(nw * sizeof(uint64));
	k = p->nvals > 0 ? sift_col_str_prefix(p->vals[0].v.s, p->vals[0].len) : 0;
	switch (op)
	{
		case SIFT_COLOP_EQ:
			sift_cc_filter(cc, k, k, cand);
			break;
		case SIFT_COLOP_IN:
			one = palloc(nw * sizeof(uint64));
			for (int i = 0; i < p->nvals; i++)
			{
				k = sift_col_str_prefix(p->vals[i].v.s, p->vals[i].len);
				sift_cc_filter(cc, k, k, one);
				sift_colk->bits_or(cand, one, nw);
			}
			pfree(one);
			break;
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			sift_cc_filter(cc, PG_INT64_MIN, k, cand);
			break;
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
			sift_cc_filter(cc, k, PG_INT64_MAX, cand);
			break;
		default:					/* PREFIX: the strings with it have the prefix keys from the constant padded with 0 up to padded with 255 */
			{
				char		pad[8];

				memset(pad, 0xFF, sizeof(pad));
				memcpy(pad, p->vals[0].v.s, Min(p->vals[0].len, 8));
				sift_cc_filter(cc, k, p->vals[0].len >= 8 ? k : sift_col_str_prefix(pad, 8), cand);
			}
	}
	sift_cc_close(cc);
	return cand;
}

/* tmp = rows of a STRS or ISTR chunk whose string satisfies the predicate (NE and NOTIN: its opposite, EQ and IN), NULL slots may be set */
static void
chunk_strs(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	const SiftColPred *p = t->pred;
	SiftColOp	op = p->op == SIFT_COLOP_NE ? SIFT_COLOP_EQ : p->op == SIFT_COLOP_NOTIN ? SIFT_COLOP_IN : p->op;
	uint64	   *cand = c->enc == SIFT_COLENC_ISTR ? istr_candidates(c, op, p) : NULL;
	uint32		nw = SIFT_COL_NWORDS(c->n),
				cw = words_of(c->n),
			   *off;
	char	   *bytes;
	bool		any = cand == NULL;

	memset(tmp, 0, nw * sizeof(uint64));
	for (uint32 w = 0; cand != NULL && w < cw; w++)
		any |= cand[w] != 0;
	if (!any)
		return;
	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	for (uint32 w = 0; w < cw; w++)
	{
		uint64		m = cand ? cand[w] : ~UINT64CONST(0);

		if (w == c->n / 64)
			m &= (UINT64CONST(1) << (c->n % 64)) - 1;
		for (; m; m &= m - 1)
		{
			uint32		i = w * 64 + pg_rightmost_one_pos64(m);

			if (!sift_col_bit_test(c->nulls, i) && strs_match(op, p, bytes + off[i], off[i + 1] - off[i]))
				tmp[w] |= UINT64CONST(1) << (i % 64);
		}
	}
	pfree(off);
	pfree(bytes);
	if (cand)
		pfree(cand);
}

void
sift_cchunk_strs_length(const SiftCChunk *c, uint32 lo, uint32 hi, uint64 *bits)
{
	int64	   *len = palloc((Size) (c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
	uint32	   *u32 = palloc((Size) c->n * sizeof(uint32) + SIFT_COL_PAD);

	memset(bits, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	if (lo <= hi)
	{
		sift_cchunk_decode(c, len);
		for (uint32 i = 0; i < c->n; i++)
			u32[i] = (uint32) len[i];
		sift_colk->range[SIFT_COLW_32](u32, c->n, lo, hi - lo, bits);
		if (c->nulls)
			sift_colk->bits_andnot(bits, c->nulls, words_of(c->n));
	}
	pfree(len);
	pfree(u32);
}

/* Sets of up to this many members are OR-ed range tests, larger ones are searched */
#define SET_RANGES_MAX	8

static void
chunk_set(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint64	   *one = palloc(SIFT_COL_NWORDS(c->n) * sizeof(uint64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	for (uint32 i = 0; i < t->nset; i++)
	{
		chunk_range(c, t->set[i], t->set[i], one);
		sift_colk->bits_or(tmp, one, SIFT_COL_NWORDS(c->n));
	}
	pfree(one);
}

void
sift_ctest_run(const SiftCTest *t, const SiftCChunk *c, uint64 *bits)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				cw = words_of(c->n);
	uint64	   *tmp;
	bool		negate = t->kind == SIFT_CT_NRANGE || t->kind == SIFT_CT_NSET ||
	(t->kind == SIFT_CT_STRS && (t->pred->op == SIFT_COLOP_NE || t->pred->op == SIFT_COLOP_NOTIN));

	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(bits, 0, nw * sizeof(uint64));
			return;
		case SIFT_CT_ISNULL:
			if (c->nulls)
				sift_colk->bits_and(bits, c->nulls, cw);
			memset(bits + (c->nulls ? cw : 0), 0, (nw - (c->nulls ? cw : 0)) * sizeof(uint64));
			return;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
			if (c->nulls)
				sift_colk->bits_andnot(bits, c->nulls, cw);
			return;
		default:
			break;
	}
	tmp = palloc(nw * sizeof(uint64));
	if (t->kind == SIFT_CT_RANGE || t->kind == SIFT_CT_NRANGE)
		chunk_range(c, t->lo, t->hi, tmp);
	else if (t->kind == SIFT_CT_LUT)
		chunk_lut(t, c, tmp);
	else if (t->kind == SIFT_CT_STRS)
		chunk_strs(t, c, tmp);
	else if (t->nset > SET_RANGES_MAX)
	{
		chunk_scalar(t, c, tmp);	/* key_matches includes the negation of NSET */
		negate = false;
	}
	else
		chunk_set(t, c, tmp);
	if (negate)
		sift_colk->bits_andnot(bits, tmp, nw);
	else
		sift_colk->bits_and(bits, tmp, nw);
	if (c->nulls)
		sift_colk->bits_andnot(bits, c->nulls, cw);
	pfree(tmp);
}
