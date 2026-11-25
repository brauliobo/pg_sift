/*
 * agg/group/fast.h - the fast paths of the groups of SiftAggV6 (CB-2):
 *   - a key of several fixed-width integer columns of at most 12 bytes in all (Q30, Q31, Q32, Q40, Q41) is packed into the 16 bytes of one inline slot key, so that the batch of a set of such keys
 *     takes the 16-ahead prefetching upsert of agg/ht/ht.h and builds no bytes, no arena and no hash of bytes;
 *   - the keys with a text column (Q10..Q18, Q33..Q39) build the bytes of the rows of a batch into one buffer, hash them in one pass, and upsert with the group and slot of a row SIFT_HT_PREFETCH
 *     rows ahead in the cache;
 *   - the minimum and the maximum of a text column (Q21, Q22, Q28) are one accumulator of the state of a group: the first 8 bytes of the string (big endian, so one compare of 64 bits orders two
 *     strings that differ in them) and the string, kept in a slab the groups share.
 */
#ifndef SIFT_AGG_V6_GROUP_FAST_H
#define SIFT_AGG_V6_GROUP_FAST_H

#include "postgres.h"

#include "agg/ht/ht.h"
#include "agg/kern/kern.h"
#include "agg/vec.h"
#include "core/scan_types.h"

/* The dictionary of a key column whose rows are codes (SVEC_DICT32): gen names the part it is of when it is the dictionary of the part (it is the same strings for every granule of the part), else 0 */
typedef struct SiftKeyDict
{
	SiftGranuleDict d;
	uint64		gen;
} SiftKeyDict;

/* ---- packed keys ---- */

#define SIFT_PACK_MAX_BYTES		12
#define SIFT_PACK_MAX_KEYS		12

typedef struct SiftPackSpec
{
	int			nk;
	uint8		width[SIFT_PACK_MAX_KEYS];
	bool		sign[SIFT_PACK_MAX_KEYS];
	uint8		shift[SIFT_PACK_MAX_KEYS];
} SiftPackSpec;

/* Whether the keys (the width of each in bytes: 1 bool, 2 int2, 4 int4 and date, 8 int8 and timestamps; sign: whether it is signed) fit a packed key; fills the spec */
extern bool sift_pack_plan(const uint8 *width, const bool *sign, int nk, SiftPackSpec *ps);
/* The keys and hashes of the rows sel[0 .. n-1] of the vectors kv (kv[i] is key i) */
extern void sift_pack_batch(const SiftPackSpec *ps, SiftVec *const *kv, const uint16 *sel, uint16 n, uint64 *hash, SiftInlineKey16 *keys);
/* Key i of a packed key: false when it is NULL */
extern bool sift_pack_get(const SiftPackSpec *ps, const SiftInlineKey16 *k, int i, int64 *x);
/* The hash of a packed key */
static inline uint64
sift_pack_hash(const SiftInlineKey16 *k)
{
	return sift_aggk6_hash1(k->u64[0] ^ sift_aggk6_hash1(k->u64[1]));
}
/* The packed key of the values (a NULL key has bit i of nulls set and its value ignored), and its hash */
extern uint64 sift_pack_key(const SiftPackSpec *ps, const int64 *x, uint64 nulls, SiftInlineKey16 *k);

/* ---- keys as bytes, a batch at a time ---- */

typedef struct SiftWideBatch
{
	uint8	   *buf;
	size_t		cap;
	uint32	   *off;
	uint32	   *len;
	uint64	   *nulls;
	uint64	   *hash;
	uint16		cap_rows;
} SiftWideBatch;

extern void sift_wide_batch_init(SiftWideBatch *wb, MemoryContext cx, uint16 rows);
extern void sift_wide_batch_free(SiftWideBatch *wb);
/* The key bytes of rows sel[0 .. n-1] (per column of kidx: 4 bytes of length and the bytes for a string, else 8 bytes; a NULL has none and a bit of the nulls) and their hashes */
extern void sift_wide_batch_build(SiftWideBatch *wb, const int *kidx, int nk, SiftVec *const *keyvec, const uint16 *sel, uint16 n);
/* slots[k] for the key of row k, in a wide table that has room for n groups (sift_ht_reserve) */
extern void sift_wide_batch_upsert(SiftSaltedHT *ht, const SiftWideBatch *wb, uint16 n, SiftAggSlot64 **slots);

/* slots[k] for the row sel[k] of the string vector kv, the one key of a wide table that has room for n groups (sift_ht_reserve): the bytes of a row are hashed where they are, and copied only when its group is new */
extern void sift_str_batch_upsert(SiftSaltedHT *ht, const SiftVec *kv, const uint16 *sel, uint16 n, uint64 *hash, SiftAggSlot64 **slots);

/* ---- the minimum and maximum of a text column ---- */

#define SIFT_STRMM_INLINE_MAX	16u
#define SIFT_STRMM_INLINE_CAP	UINT32_MAX

/*
 * the state of a group: the same 32 bytes as V6Acc (agg/group/int.h). A best string of at most 16 bytes is the state itself (the first 8 bytes in pfx, the rest in u.tail, cap
 * SIFT_STRMM_INLINE_CAP); a longer one is in the slab, in a block that doubles when a longer string comes (cap bytes, which the group may overwrite), or is another state's (cap 0).
 */
typedef struct SiftStrMM
{
	int64		n;				/* the strings offered: 0 for an empty state */
	uint64		pfx;			/* the first 8 bytes of the best, big endian, zero padded */
	union
	{
		uint8	   *ptr;		/* the best, when cap is not SIFT_STRMM_INLINE_CAP */
		uint8		tail[8];	/* bytes 8 .. 15 of the best, when it is */
	}			u;
	uint32		len;
	uint32		cap;
} SiftStrMM;

StaticAssertDecl(sizeof(SiftStrMM) == 32, "a text minimum is 32 bytes");

/* The bytes of the best string: the block of the state, or buf (16 bytes) filled from the state when it holds them */
static inline const uint8 *
sift_strmm_bytes(const SiftStrMM *a, uint8 *buf)
{
	const uint64 be = __builtin_bswap64(a->pfx);

	if (a->cap != SIFT_STRMM_INLINE_CAP)
		return a->u.ptr;
	memcpy(buf, &be, 8);
	memcpy(buf + 8, a->u.tail, 8);
	return buf;
}

typedef struct SiftStrSlab SiftStrSlab;

extern SiftStrSlab *sift_strslab_new(MemoryContext cx);
extern uint64 sift_strslab_bytes(const SiftStrSlab *sl);
extern void sift_strslab_free(SiftStrSlab *sl);
/* One string into the state: the least (is_min) or greatest of the strings under the order of their bytes (the C collation) */
extern void sift_strmm_offer(SiftStrSlab *sl, SiftStrMM *a, bool is_min, const uint8 *p, uint32 len);
/* The state s into d (s keeps its string, which d may point to) */
extern void sift_strmm_merge(SiftStrMM *d, const SiftStrMM *s, bool is_min);

#endif							/* SIFT_AGG_V6_GROUP_FAST_H */
