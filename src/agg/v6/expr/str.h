/*
 * agg/v6/expr/str.h - the keys and arguments the scan of SiftAggV6 computes from its columns (SiftV6Derive of agg/v6/v6.h), over the active rows of a batch:
 *   length(text)          the characters of each string (AVX2 over UTF-8: the bytes that are not continuation bytes), the bytes in a single-byte encoding;
 *   regexp_replace(c, '^https?://(?:www\.)?([^/]+)/.*$', '\1')   the host of an URL as a view into the bytes of the column (a string that does not match is itself, as for regexp_replace);
 *   extract(minute | hour FROM timestamp)   the unit of the microseconds, NULL for an infinite timestamp;
 *   CASE WHEN c1 = k1 AND .. THEN text ELSE constant END   the view of the text of a row whose conditions hold and the constant's else, no byte copied.
 */
#ifndef SIFT_AGG_V6_EXPR_STR_H
#define SIFT_AGG_V6_EXPR_STR_H

#include "postgres.h"

#include "agg/v6/v6.h"
#include "agg/vec.h"

/* The characters of len bytes of UTF-8: those that are not continuation bytes (10xxxxxx) */
extern uint32 sift_vx_utf8_len(const uint8 *p, uint32 len);

/* The result of the regexp of SVD_URL_HOST on len bytes at p, in *host and *hlen (the host when the string matches, else the string itself); whether it matched */
extern bool sift_vx_url_host(const uint8 *p, uint32 len, const uint8 **host, uint32 *hlen);

/* length(text) of len bytes at p: the characters of a UTF-8 string, the bytes in a single-byte encoding */
static inline uint32
sift_vx_strlen(const SiftV6Derive *d, const uint8 *p, uint32 len)
{
	return d->utf8 ? sift_vx_utf8_len(p, len) : len;
}

/* The view of len bytes at p (the bytes of a short string are in the view, a long one is a pointer) */
extern SiftStrView sift_vx_strview(const uint8 *p, uint32 len);

/* The period of the unit of extract: the minutes of an hour, else the hours of a day */
static inline int64
sift_vx_extract_mod(int64 unit_us)
{
	return unit_us == 60000000 ? 60 : 24;
}

/* extract(minute or hour FROM ts) of a timestamp: its unit of unit_us microseconds from the floor; false for an infinite timestamp */
static inline bool
sift_vx_extract_one(int64 ts, int64 unit_us, int64 *out)
{
	const int64 mod = sift_vx_extract_mod(unit_us);
	int64		q;

	if (ts == PG_INT64_MIN || ts == PG_INT64_MAX)
		return false;
	q = ts / unit_us - (ts % unit_us < 0);
	*out = ((q % mod) + mod) % mod;
	return true;
}

/* The first key of a statement with elided keys is in the range where none of them overflows */
static inline bool
sift_vx_affine_holds(const SiftV6Spec *spec, int64 first)
{
	return first >= spec->aff_lo && first <= spec->aff_hi;
}

/* The vector a derived key or argument is, for the rows of bs: out is the room the vector's data and validity are in */
typedef struct SiftVxOut
{
	SiftVec		vec;
	void	   *data;
	uint64		valid[SIFT_VEC_WORDS];
	uint64		nan[SIFT_VEC_WORDS];	/* a numeric's NaN rows (vec.nan points here when there are any) */
} SiftVxOut;

extern void sift_vx_out_init(SiftVxOut *o, MemoryContext cx);
/* src is the vector of d->src, cond[i] that of d->cond[i] */
extern void sift_vx_derive(const SiftV6Derive *d, const SiftVec *src, SiftVec *const *cond, const SiftBatchSel *bs, SiftVxOut *out);

/* The first key (of the keys left after an affine collapse) of every active row is in [aff_lo, aff_hi] of the spec: else one of the keys that were elided overflows, and raises as PostgreSQL does */
extern void sift_vx_affine_check(const SiftV6Spec *spec, const SiftVec *key, const SiftBatchSel *bs);

#endif							/* SIFT_AGG_V6_EXPR_STR_H */
