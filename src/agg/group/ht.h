/*
 * agg/group/ht.h - the hash table of a high-cardinality GROUP BY: a control byte per slot (the 7-bit salt of the hash, 0 = empty) in
 * groups of 16 that one vector compare filters, and a slot of one 64-byte cache line that holds the whole hash, the key of up to 16
 * bytes and the first aggregates (count, sum, min, max), so that finding a group and updating it touch one line of the slot array.
 * The hash is kept in the slot: growing the table, or scattering its groups into radix partitions for a parallel merge, never hashes
 * a key again.
 *
 * Bits of the 64-bit hash: 0..31 the first group of the probe, 48..54 the salt (SIFT_HASH_SALT), 56..63 the radix partition
 * (SIFT_HASH_RADIX); the three never overlap. Groups are never deleted (a table is cleared or dropped whole), so there are no tombstones.
 *
 * A table made wide holds every key as bytes in an arena of the table (composite keys over 16 bytes, strings): the slot keeps the
 * offset, the length and the null flags of the key columns, and the arena only grows, so an offset stays valid while the table grows.
 * A table made with an extended state gives each group a zeroed block of ext_stride bytes next to the slot, for the aggregates that
 * do not fit its 32 bytes (agg/group/state.h); one of its words may be a count(DISTINCT) set (agg/group/dist.h) that the table frees.
 * Pointers into the state block are valid until the next upsert; the slots of a batch are valid after sift_ht_reserve.
 */
#ifndef SIFT_AGG_GROUP_HT_H
#define SIFT_AGG_GROUP_HT_H

#include "agg/kern6/kern6.h"

#if defined(__x86_64__)
#include <emmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define SIFT_HT_GROUP_WIDTH		16
#define SIFT_HT_MIN_SLOTS		64
#define SIFT_HT_MAX_LOAD_NUM	7
#define SIFT_HT_MAX_LOAD_DEN	8
#define SIFT_HT_NO_EXT			UINT32_MAX
#define SIFT_HT_EXT_ALIGN		64

/* The key of a group: up to 16 bytes at an 8 byte boundary of the slot (an int128, a uuid or a string view is copied in with memcpy: their 16 byte alignment would not fit the slot), or the reference to a wide key */
typedef union SiftInlineKey16
{
	uint64		u64[2];
	int64		i64[2];
	struct
	{
		uint32		arena_off;
		uint32		key_len;
		uint64		key_nulls;	/* the null flags of up to 64 key columns */
	}			wide;
} SiftInlineKey16;

/* [0..7] hash, [8..23] key, [24..27] flags (key nulls: bits 0..15, aggregate non-nulls: bits 16..31), [28..31] ext_idx, [32..63] aggregates */
typedef struct __attribute__((aligned(64))) SiftAggSlot64
{
	uint64		hash64;
	SiftInlineKey16 key;
	uint32		flags;
	uint32		ext_idx;
	union
	{
		int64		i64[4];
		double		f64[4];
		SiftI128	i128[2];
		uint8		raw[32];
	}			agg;
} SiftAggSlot64;

_Static_assert(sizeof(SiftAggSlot64) == 64, "SiftAggSlot64 is one cache line");

typedef struct SiftSaltedHT
{
	uint8	   *ctrl;			/* capacity + SIFT_HT_GROUP_WIDTH bytes: the first group is mirrored at the end, so that a group load never wraps */
	SiftAggSlot64 *slots;
	uint32		capacity;		/* a power of 2 */
	uint32		mask;
	uint32		count;
	uint32		grow_at;
	uint32		lock_at;		/* not 0: the batch upserts (sift_ht_upsert_batch, sift_ht_upsert_wide_lazy) make no group once the table has this many, and return NULL for a key it has not */
	uint32		gen;			/* never 0; changes whenever the slots move or the groups go (growth, clear): what a pointer to a slot was taken at */
	MemoryContext cxt;			/* what the table allocates in, whatever context is current when it grows */
	bool		key_is_wide;
	bool		key_is_text;	/* the wide keys are strings that are equal when their bytes are: made by sift_ht_init_str */
	uint32		ext_stride;		/* 0: no extended state; else a multiple of 16 */
	uint32		ext_dist_off;	/* byte offset in a state block of a SiftDistSet pointer, or SIFT_HT_NO_EXT */
	uint32		ext_used;
	uint32		ext_cap;
	uint8	   *ext_slab;
	uint32		arena_used;
	uint32		arena_cap;
	uint8	   *arena;
} SiftSaltedHT;

/* The bit i of the result is set when ctrl[i] == b, i < 16: the scalar specification and the level compiled for */
extern uint32 sift_ht_match16_scalar(const uint8 *ctrl, uint8 b);
extern uint32 sift_ht_match16(const uint8 *ctrl, uint8 b);

/*
 * The same compare for the probe loops, as the mask the instruction gives with no further step: SSE2 a bit a lane (PMOVMSKB), NEON four bits a lane (a
 * compare and one narrowing shift: no table of bit weights, no chain of pairwise adds). SIFT_HT_MASK_NEXT is the index of the first matching lane,
 * SIFT_HT_MASK_CLEAR drops it; a mask is empty when it is 0.
 */
#if defined(__x86_64__)
typedef uint32 SiftHtMask;

static inline SiftHtMask
sift_ht_match16_fast(const uint8 *ctrl, uint8 b)
{
	return (uint32) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *) ctrl), _mm_set1_epi8((char) b)));
}

#define SIFT_HT_MASK_NEXT(m)	((uint32) __builtin_ctz(m))
#define SIFT_HT_MASK_CLEAR(m)	((m) &= (m) - 1)
#elif defined(__aarch64__) && defined(__ARM_NEON)
typedef uint64 SiftHtMask;

static inline SiftHtMask
sift_ht_match16_fast(const uint8 *ctrl, uint8 b)
{
	const uint8x16_t eq = vceqq_u8(vld1q_u8(ctrl), vdupq_n_u8(b));

	return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
}

#define SIFT_HT_MASK_NEXT(m)	((uint32) (__builtin_ctzll(m) >> 2))
#define SIFT_HT_MASK_CLEAR(m)	((m) &= ~(UINT64CONST(0xF) << __builtin_ctzll(m)))
#else
typedef uint32 SiftHtMask;

static inline SiftHtMask
sift_ht_match16_fast(const uint8 *ctrl, uint8 b)
{
	return sift_ht_match16_scalar(ctrl, b);
}

#define SIFT_HT_MASK_NEXT(m)	((uint32) __builtin_ctz(m))
#define SIFT_HT_MASK_CLEAR(m)	((m) &= (m) - 1)
#endif

/* An empty table of at least min_slots slots (and min_slots * 7 / 8 groups) in the current memory context */
extern void sift_ht_init(SiftSaltedHT *ht, uint32 min_slots);
/* The same, with wide keys and/or a state block of ext_stride bytes (rounded up to 16) per group, a SiftDistSet pointer at ext_dist_off of it or SIFT_HT_NO_EXT */
extern void sift_ht_init_ex(SiftSaltedHT *ht, uint32 min_slots, bool wide_key, uint32 ext_stride, uint32 ext_dist_off);
/*
 * A wide-key table of the strings of a column of collation collid, grouped by their bytes. A deterministic collation makes two strings equal exactly when their bytes
 * are, so the bytes are the group; a nondeterministic one (an ICU collation of case or accent insensitive comparison) equates strings of different bytes, and
 * grouping them by bytes would split groups PostgreSQL has as one: 0A000. A caller decides before it builds a table with sift_text_bytes_equal.
 */
extern void sift_ht_init_str(SiftSaltedHT *ht, uint32 min_slots, Oid collid, uint32 ext_stride, uint32 ext_dist_off);
extern bool sift_text_bytes_equal(Oid collid);
extern void sift_ht_destroy(SiftSaltedHT *ht);
/* The bytes the table holds: its slots, its state blocks and its arena (not the distinct sets of its groups) */
static inline uint64
sift_ht_bytes(const SiftSaltedHT *ht)
{
	return (uint64) ht->capacity * (sizeof(SiftAggSlot64) + 1) + (uint64) ht->ext_cap * ht->ext_stride + ht->arena_cap;
}
/* Drops every group (and the sets and wide keys they own), keeping the memory of the slots */
extern void sift_ht_clear(SiftSaltedHT *ht);
/* A cleared table brought down to min_slots slots, with no state blocks and no key arena: the memory a spill gives back */
extern void sift_ht_shrink_to(SiftSaltedHT *ht, uint32 min_slots);
/* Room for n more groups without growing: the slots a batch of upserts returns stay valid */
extern void sift_ht_reserve(SiftSaltedHT *ht, uint32 n);
/* Room for n groups in the slots and in the state blocks at once, so that a scan that meets about n groups grows nothing */
extern void sift_ht_presize(SiftSaltedHT *ht, uint32 n);
/* The slot of the key (found = true), or a new one with the hash and key set, flags 0, ext_idx SIFT_HT_NO_EXT (a zeroed state block if the table has them) and the aggregates zero; grows when full. Inline-key tables only */
extern SiftAggSlot64 *sift_ht_upsert(SiftSaltedHT *ht, uint64 hash, const SiftInlineKey16 *key, bool *found);
/* The same for a wide-key table: the key is len bytes and its column null flags (part of the key) */
extern SiftAggSlot64 *sift_ht_upsert_wide(SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found);
/* The same, growing the table only when the key is new and the load limit is reached (the caller finds the slots it holds again when ht->gen changes) */
extern SiftAggSlot64 *sift_ht_upsert_wide_lazy(SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found);
/* The same for the key of a string: the 4 bytes of its length and its len bytes (what sift_wide_batch_build makes of it), which are copied only when the key is new */
extern SiftAggSlot64 *sift_ht_upsert_lenpfx_lazy(SiftSaltedHT *ht, uint64 hash, const void *p, uint32 len, bool *found);
extern SiftAggSlot64 *sift_ht_find_lenpfx(const SiftSaltedHT *ht, uint64 hash, const void *p, uint32 len);
/* The slot of the key, NULL when the table has no such group; never changes the table */
extern SiftAggSlot64 *sift_ht_find(const SiftSaltedHT *ht, uint64 hash, const SiftInlineKey16 *key);
extern SiftAggSlot64 *sift_ht_find_wide(const SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls);
/*
 * out[i] = the slot of keys[i] for i < n (hashes[i]); software prefetch of the group and the slot SIFT_HT_PREFETCH rows ahead. The table grows only when a key is new and the
 * load limit is reached, and the slots of the rows before it are then found again: the slots are valid when the call returns, and a batch of keys already there grows nothing.
 * Returns the groups created.
 */
extern uint32 sift_ht_upsert_batch(SiftSaltedHT *ht, const uint64 *hashes, const SiftInlineKey16 *keys, uint32 n, SiftAggSlot64 **out);

/* The hash of len bytes (a wide key); the same bytes give the same hash at every level and on every host */
extern uint64 sift_hash_bytes(const void *p, uint32 len);

/* The bytes of a string view: its 12 inline bytes (prefix and tail, zero padded), or the string it points to */
static inline const uint8 *
sift_strview_bytes(const SiftStrView *v)
{
	return v->len <= 12 ? (const uint8 *) v + offsetof(SiftStrView, prefix) : (const uint8 *) v->u.ptr;
}

/* The slot of a string, in a table of sift_ht_init_str: its bytes are the key, whatever the length of the view */
extern SiftAggSlot64 *sift_ht_upsert_str(SiftSaltedHT *ht, const SiftStrView *v, bool *found);

#define SIFT_HT_PREFETCH	16
#define SIFT_HT_PREFETCH_SLOT	8
#define SIFT_HT_PREFETCH_KEY	4

/*
 * The staged prefetch of a probe loop: the control group of the hash SIFT_HT_PREFETCH rows ahead, the first slot its salt matches, or else the first empty one (that slot, not the first of the group, is where the key is or goes)
 * SIFT_HT_PREFETCH_SLOT rows ahead, and, for a wide table, the bytes of the key of that slot SIFT_HT_PREFETCH_KEY ahead: each stage reads what the one before it brought in.
 */
static inline void
sift_ht_prefetch_group(const SiftSaltedHT *ht, uint64 hash)
{
	const uint32 g = (uint32) hash & ht->mask;

	__builtin_prefetch(&ht->ctrl[g]);
}

static inline const SiftAggSlot64 *
sift_ht_first_match(const SiftSaltedHT *ht, uint64 hash)
{
	const uint32 pos = (uint32) hash & ht->mask;
	const SiftHtMask m = sift_ht_match16_fast(ht->ctrl + pos, SIFT_HASH_SALT(hash));

	return m ? &ht->slots[(pos + SIFT_HT_MASK_NEXT(m)) & ht->mask] : NULL;
}

static inline void
sift_ht_prefetch_slot(const SiftSaltedHT *ht, uint64 hash)
{
	const SiftAggSlot64 *s = sift_ht_first_match(ht, hash);

	if (s != NULL)
		__builtin_prefetch(s);
	else
	{
		const uint32 pos = (uint32) hash & ht->mask;
		const SiftHtMask e = sift_ht_match16_fast(ht->ctrl + pos, 0);

		if (e)
			__builtin_prefetch(&ht->slots[(pos + SIFT_HT_MASK_NEXT(e)) & ht->mask], 1);
	}
}

static inline void
sift_ht_prefetch_key(const SiftSaltedHT *ht, uint64 hash)
{
	const SiftAggSlot64 *s = sift_ht_first_match(ht, hash);

	if (s != NULL)
		__builtin_prefetch(ht->arena + s->key.wide.arena_off);
}

/* The index of the first occupied slot at or after pos (capacity when there is none): 16 control bytes at a time, whose top bits are the occupied ones. ctrl has 16 bytes after the capacity */
static inline uint32
sift_ht_next_occupied(const SiftSaltedHT *ht, uint32 pos)
{
	while (pos < ht->capacity)
	{
#if defined(__x86_64__)
		const uint32 m = (uint32) _mm_movemask_epi8(_mm_loadu_si128((const __m128i *) (ht->ctrl + pos)));

		if (m != 0)
			return Min(pos + (uint32) __builtin_ctz(m), ht->capacity);
		pos += 16;
#else
		uint64		w;

		memcpy(&w, ht->ctrl + pos, sizeof(w));
		w &= UINT64CONST(0x8080808080808080);
		if (w != 0)
			return Min(pos + ((uint32) __builtin_ctzll(w) >> 3), ht->capacity);
		pos += 8;
#endif
	}
	return ht->capacity;
}

#define SIFT_HT_FOREACH(ht, s) \
	for (uint32 i_ = sift_ht_next_occupied(ht, 0); i_ < (ht)->capacity; i_ = sift_ht_next_occupied(ht, i_ + 1)) \
		if ((s) = &(ht)->slots[i_], true)

/* The bytes of the wide key of a slot */
static inline const uint8 *
sift_ht_wide_key(const SiftSaltedHT *ht, const SiftAggSlot64 *s, uint32 *len)
{
	*len = s->key.wide.key_len;
	return ht->arena + s->key.wide.arena_off;
}

/* The state block of group number idx (the ext_idx of its slot) of a table made with one */
static inline uint8 *
sift_ht_ext_idx(const SiftSaltedHT *ht, uint32 idx)
{
	return ht->ext_slab + (size_t) idx * ht->ext_stride;
}

/* The state block of a slot of a table made with one */
static inline uint8 *
sift_ht_ext(const SiftSaltedHT *ht, const SiftAggSlot64 *s)
{
	return sift_ht_ext_idx(ht, s->ext_idx);
}

/* The first four aggregates as count, sum, min, max */
static inline void
sift_slot_cmsm_init(SiftAggSlot64 *s)
{
	s->agg.i64[0] = 0;
	s->agg.i64[1] = 0;
	s->agg.i64[2] = INT64_MAX;
	s->agg.i64[3] = INT64_MIN;
}

static inline void
sift_slot_cmsm_add(SiftAggSlot64 *s, int64 v)
{
	s->agg.i64[0]++;
	s->agg.i64[1] = (int64) ((uint64) s->agg.i64[1] + (uint64) v);
	s->agg.i64[2] = Min(s->agg.i64[2], v);
	s->agg.i64[3] = Max(s->agg.i64[3], v);
}

static inline void
sift_slot_cmsm_merge(SiftAggSlot64 *dst, const SiftAggSlot64 *src)
{
	dst->agg.i64[0] += src->agg.i64[0];
	dst->agg.i64[1] = (int64) ((uint64) dst->agg.i64[1] + (uint64) src->agg.i64[1]);
	dst->agg.i64[2] = Min(dst->agg.i64[2], src->agg.i64[2]);
	dst->agg.i64[3] = Max(dst->agg.i64[3], src->agg.i64[3]);
}

/* The partitions of a parallel merge: a power of 2 in [16, 128], 4 per participant */
static inline uint32
sift_radix_parts(int participants)
{
	uint32		p = 16;

	while (p < (uint32) participants * 4 && p < 128)
		p <<= 1;
	return p;
}

#endif							/* SIFT_AGG_GROUP_HT_H */
