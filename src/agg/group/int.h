/*
 * agg/group/int.h - what the files of agg/group/ share
 * The lookahead of the prefetch and the functions the table, the ident ids, the key ids and
 * the group ids call in one another.
 */
#ifndef SIFT_AGG_GROUP_DIR_INT_H
#define SIFT_AGG_GROUP_DIR_INT_H

#include "utils/float.h"

#include "agg/agg_int.h"

/* tuples ahead whose slot of a group or value table is prefetched: the tables outgrow the cache at millions of groups */
#define GROUP_AHEAD 16

/* the group of an id tuple in hash mode (h: its hash), a new one if it has none; where nk is a constant the hash and the comparison unroll */
static pg_attribute_always_inline uint32
hash_probe(AggExec *ex, const uint32 *ids, const int nk, uint32 h)
{
	uint32		pos = h & (ex->nhslot - 1);

	for (uint32 g; (g = ex->hslot[pos]) != 0; pos = (pos + 1) & (ex->nhslot - 1))
	{
		const uint32 *key = &ex->hkey[(Size) (g - 1) * nk];
		int			j = 0;

		while (j < nk && key[j] == ids[j])
			j++;
		if (j == nk)
			return g - 1;
	}
	return hash_add(ex, ids, pos);
}

/* The group of a float: its bits, with the one zero and the one NaN of float8_eq / hashfloat8 (-0 = 0, NaN = NaN) */
static inline int64
float_key(double d)
{
	int64		bits;

	d = d == 0 ? 0.0 : isnan(d) ? get_float8_nan() : d;
	memcpy(&bits, &d, sizeof(bits));
	return bits;
}

/* the top bits of the product: a float with a short mantissa (1.5, 100.0) has nothing in its low 40 bits, and the middle bits of the product have nothing either */
static inline uint32
wide_hash(int64 v, uint32 cap)
{
	return (uint32) (((uint64) v * UINT64CONST(0x9E3779B97F4A7C15)) >> (64 - __builtin_ctz(cap)));
}

/* the id of v in the open addressing table of an ident key, 0 when it has none yet (ident_wide_id gives it one) */
static inline uint32
wide_find(const KeyState *k, int64 v)
{
	uint32		h = wide_hash(v, k->wcap);

	while (k->wid[h] != 0 && k->wval[h] != v)
		h = (h + 1) & (k->wcap - 1);
	return k->wid[h];
}

extern bool bucket_numbers_utc(int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx);
extern bool bucket_numbers_zoned(ZoneOff *z, int64 us, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *out, int32 *mn, int32 *mx);
extern void bucket_hash_ids(AggExec *ex, KeyState *k, const SiftSrcVec *v, const uint32 *idx, int nsel, uint32 *ids);

extern void key_prepare(AggExec *ex, int ki, int nsel, const uint32 *idx, uint32 **map_out, int32 *base_out, uint32 *vals_out);
extern void key_null_fix(AggExec *ex, int ki, int nsel, const uint32 *vals, uint32 *gid, uint32 mult, uint32 *ids);

#endif							/* SIFT_AGG_GROUP_DIR_INT_H */
