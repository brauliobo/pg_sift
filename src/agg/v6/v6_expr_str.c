/*
 * agg/v6/v6_expr_str.c - the computed keys and arguments of the scan (agg/v6/v6_expr_str.h).
 */
#include "postgres.h"

#include "core/cpu.h"

#include "agg/group/ht.h"
#include "agg/v6/v6_expr_str.h"

#if defined(__x86_64__)
#include <immintrin.h>

SIFT_TARGET_AVX2 static uint32
utf8_len_avx2(const uint8 *p, uint32 len)
{
	const __m256i bound = _mm256_set1_epi8(-0x40);
	uint32		cont = 0,
				i = 0;

	for (; i + 32 <= len; i += 32)
		cont += (uint32) __builtin_popcount((uint32) _mm256_movemask_epi8(_mm256_cmpgt_epi8(bound, _mm256_loadu_si256((const __m256i *) (p + i)))));
	for (; i < len; i++)
		cont += (p[i] & 0xC0) == 0x80;
	return len - cont;
}
#endif

uint32
sift_vx_utf8_len(const uint8 *p, uint32 len)
{
	uint32		cont = 0;

#if defined(__x86_64__)
	if (len >= 32 && sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_AVX2))
		return utf8_len_avx2(p, len);
#endif
	for (uint32 i = 0; i < len; i++)
		cont += (p[i] & 0xC0) == 0x80;
	return len - cont;
}

bool
sift_vx_url_host(const uint8 *p, uint32 len, const uint8 **host, uint32 *hlen)
{
	uint32		off;
	const uint8 *rest,
			   *slash;
	uint32		rn,
				q;

	if (len >= 7 && memcmp(p, "http://", 7) == 0)
		off = 7;
	else if (len >= 8 && memcmp(p, "https://", 8) == 0)
		off = 8;
	else
		return false;
	rest = p + off;
	rn = len - off;
	slash = memchr(rest, '/', rn);
	if (slash == NULL || slash == rest)
		return false;
	q = (uint32) (slash - rest);
	if (q > 4 && memcmp(rest, "www.", 4) == 0)
	{
		*host = rest + 4;
		*hlen = q - 4;
	}
	else
	{
		*host = rest;
		*hlen = q;
	}
	return true;
}

SiftStrView
sift_vx_strview(const uint8 *p, uint32 len)
{
	SiftStrView v = {.len = len};

	if (len == 0)
		return v;
	memcpy(v.prefix, p, Min(len, 4));
	if (len <= 12)
		memcpy(v.u.tail, p + 4, len > 4 ? len - 4 : 0);
	else
		v.u.ptr = (const char *) p;
	return v;
}

bool
sift_vx_extract_one(int64 ts, int64 unit_us, int64 mod, int64 *out)
{
	int64		q;

	if (ts == PG_INT64_MIN || ts == PG_INT64_MAX)
		return false;
	q = ts / unit_us - (ts % unit_us < 0);
	*out = ((q % mod) + mod) % mod;
	return true;
}

void
sift_vx_out_init(SiftVxOut *o, MemoryContext cx)
{
	*o = (SiftVxOut) {0};
	o->data = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
}

static inline bool
valid_at(const SiftVec *v, uint16 r)
{
	return v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);
}

static inline int64
int_of(const SiftVec *v, uint16 r)
{
	return v->type == SVEC_I32 ? ((const int32 *) v->data)[r] : v->type == SVEC_BOOL ? ((const uint8 *) v->data)[r] : ((const int64 *) v->data)[r];
}

#define FOR_ACTIVE(bs, r) \
	for (uint16 w_ = 0; w_ < ((bs)->n_raw + 63) / 64; w_++) \
		for (uint64 bits_ = (bs)->mask[w_]; bits_ != 0; bits_ &= bits_ - 1) \
			for (uint16 r = (uint16) (w_ * 64 + __builtin_ctzll(bits_)), once_ = 1; once_; once_ = 0)

static void
set_valid(SiftVxOut *o, uint16 r, bool valid)
{
	o->valid[r >> 6] = (o->valid[r >> 6] & ~(UINT64CONST(1) << (r & 63))) | ((uint64) valid << (r & 63));
}

/* extract of the unit and period of the call: the divisions are by constants, which the compiler turns into multiplications when it inlines this for the common units */
static inline __attribute__((always_inline)) void
extract_rows(const SiftVec *src, const SiftBatchSel *bs, SiftVxOut *o, int64 unit_us, int64 mod)
{
	int64	   *oi = o->data;

	FOR_ACTIVE(bs, r)
	{
		int64		x = 0;
		const bool	ok = valid_at(src, r) && sift_vx_extract_one(int_of(src, r), unit_us, mod, &x);

		oi[r] = x;
		set_valid(o, r, ok);
	}
}

static void
extract_batch(const SiftV6Derive *d, const SiftVec *src, const SiftBatchSel *bs, SiftVxOut *o)
{
	if (d->aux == 60000000)
		extract_rows(src, bs, o, 60000000, 60);
	else if (d->aux == INT64CONST(3600000000))
		extract_rows(src, bs, o, INT64CONST(3600000000), 24);
	else
		extract_rows(src, bs, o, d->aux, 24);
}

void
sift_vx_derive(const SiftV6Derive *d, const SiftVec *src, SiftVec *const *cond, const SiftBatchSel *bs, SiftVxOut *o)
{
	const SiftStrView *sv = (const SiftStrView *) src->data;
	SiftStrView *ov = o->data;
	int64	   *oi = o->data;
	const SiftStrView else_view = sift_vx_strview(d->else_bytes, d->else_len);

	memset(o->valid, 0, sizeof(o->valid));
	switch (d->kind)
	{
		case SVD_STRLEN:
			FOR_ACTIVE(bs, r)
			{
				const bool	ok = valid_at(src, r);

				oi[r] = ok ? (d->utf8 ? sift_vx_utf8_len(sift_strview_bytes(&sv[r]), sv[r].len) : sv[r].len) : 0;
				set_valid(o, r, ok);
			}
			o->vec = (SiftVec) {.data = o->data,.valid = o->valid,.len = bs->n_raw,.all_valid = false,.dense = true,.type = SVEC_I64};
			break;
		case SVD_URL_HOST:
			FOR_ACTIVE(bs, r)
			{
				const bool	ok = valid_at(src, r);
				const uint8 *host;
				uint32		hlen;

				if (ok && sift_vx_url_host(sift_strview_bytes(&sv[r]), sv[r].len, &host, &hlen))
					ov[r] = sift_vx_strview(host, hlen);
				else
					ov[r] = sv[r];
				set_valid(o, r, ok);
			}
			o->vec = (SiftVec) {.data = o->data,.valid = o->valid,.len = bs->n_raw,.all_valid = false,.dense = true,.type = SVEC_STRVIEW};
			break;
		case SVD_EXTRACT:
			extract_batch(d, src, bs, o);
			o->vec = (SiftVec) {.data = o->data,.valid = o->valid,.len = bs->n_raw,.all_valid = false,.dense = true,.type = SVEC_I64};
			break;
		case SVD_CASE_STR:
			FOR_ACTIVE(bs, r)
			{
				bool		hold = true;

				for (int i = 0; i < d->ncond && hold; i++)
					hold = valid_at(cond[i], r) && int_of(cond[i], r) == d->cval[i];
				ov[r] = hold ? sv[r] : else_view;
				set_valid(o, r, !hold || valid_at(src, r));
			}
			o->vec = (SiftVec) {.data = o->data,.valid = o->valid,.len = bs->n_raw,.all_valid = false,.dense = true,.type = SVEC_STRVIEW};
			break;
		default:
			break;
	}
}

void
sift_vx_affine_check(const SiftV6Spec *spec, const SiftVec *key, const SiftBatchSel *bs)
{
	FOR_ACTIVE(bs, r)
	{
		if (valid_at(key, r))
		{
			const int64 x = int_of(key, r);

			if (x < spec->aff_lo || x > spec->aff_hi)
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg(spec->aff_type == INT8OID ? "bigint out of range" : "integer out of range")));
		}
	}
}
