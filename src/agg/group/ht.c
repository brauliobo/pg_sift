/*
 * agg/group/ht.c - the salted hash table of agg/group/ht.h.
 */
#include "core/host.h"

#include "utils/lsyscache.h"

#include "agg/group/dist.h"
#include "agg/group/ht.h"

uint32
sift_ht_match16_scalar(const uint8 *ctrl, uint8 b)
{
	uint32		m = 0;

	for (int i = 0; i < SIFT_HT_GROUP_WIDTH; i++)
		m |= (uint32) (ctrl[i] == b) << i;
	return m;
}

uint32
sift_ht_match16(const uint8 *ctrl, uint8 b)
{
#if defined(__x86_64__)
	return sift_ht_match16_fast(ctrl, b);
#elif defined(SIFT_HAVE_NEON)
	SiftHtMask	m = sift_ht_match16_fast(ctrl, b);
	uint32		bits = 0;

	for (; m; SIFT_HT_MASK_CLEAR(m))
		bits |= (uint32) 1 << SIFT_HT_MASK_NEXT(m);
	return bits;
#else
	return sift_ht_match16_scalar(ctrl, b);
#endif
}

uint64
sift_hash_bytes(const void *p, uint32 len)
{
	return sift_aggk6_hash_bytes(p, len);
}

static void
ht_alloc(SiftSaltedHT *ht, uint32 capacity)
{
	ht->capacity = capacity;
	ht->mask = capacity - 1;
	ht->count = 0;
	ht->grow_at = (uint32) ((uint64) capacity * SIFT_HT_MAX_LOAD_NUM / SIFT_HT_MAX_LOAD_DEN);
	ht->ctrl = MemoryContextAllocZero(ht->cxt, capacity + SIFT_HT_GROUP_WIDTH);
	ht->slots = MemoryContextAllocAligned(ht->cxt, sizeof(SiftAggSlot64) * (size_t) capacity, 64, MCXT_ALLOC_ZERO);
}

void
sift_ht_init_ex(SiftSaltedHT *ht, uint32 min_slots, bool wide_key, uint32 ext_stride, uint32 ext_dist_off)
{
	uint32		cap = SIFT_HT_MIN_SLOTS;

	while (cap < min_slots)
		cap <<= 1;
	memset(ht, 0, sizeof(*ht));
	ht->cxt = CurrentMemoryContext;
	ht->key_is_wide = wide_key;
	ht->ext_stride = TYPEALIGN(16, ext_stride);
	ht->ext_dist_off = ext_dist_off;
	ht_alloc(ht, cap);
}

bool
sift_text_bytes_equal(Oid collid)
{
	return OidIsValid(collid) && get_collation_isdeterministic(collid);
}

void
sift_ht_init_str(SiftSaltedHT *ht, uint32 min_slots, Oid collid, uint32 ext_stride, uint32 ext_dist_off)
{
	if (!sift_text_bytes_equal(collid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("strings of collation \"%s\" are not grouped by their bytes", get_collation_name(collid)),
						errdetail("The collation is not deterministic: strings of different bytes can be equal."),
						errhint("Group them in PostgreSQL, or with a deterministic collation.")));
	sift_ht_init_ex(ht, min_slots, true, ext_stride, ext_dist_off);
	ht->key_is_text = true;
}

void
sift_ht_init(SiftSaltedHT *ht, uint32 min_slots)
{
	sift_ht_init_ex(ht, min_slots, false, 0, SIFT_HT_NO_EXT);
}

static void
ht_free_sets(SiftSaltedHT *ht)
{
	if (ht->ext_dist_off == SIFT_HT_NO_EXT)
		return;
	for (uint32 i = 0; i < ht->ext_used; i++)
	{
		SiftDistSet **set = (SiftDistSet **) (ht->ext_slab + (size_t) i * ht->ext_stride + ht->ext_dist_off);

		if (*set)
			sift_dist_free(*set);
	}
}

void
sift_ht_destroy(SiftSaltedHT *ht)
{
	ht_free_sets(ht);
	pfree(ht->ctrl);
	pfree(ht->slots);
	if (ht->ext_slab)
		pfree(ht->ext_slab);
	if (ht->arena)
		pfree(ht->arena);
	memset(ht, 0, sizeof(*ht));
}

void
sift_ht_clear(SiftSaltedHT *ht)
{
	ht_free_sets(ht);
	memset(ht->ctrl, 0, ht->capacity + SIFT_HT_GROUP_WIDTH);
	ht->count = 0;
	ht->ext_used = 0;
	ht->arena_used = 0;
}

static inline void
ctrl_set(SiftSaltedHT *ht, uint32 i, uint8 salt)
{
	ht->ctrl[i] = salt;
	if (i < SIFT_HT_GROUP_WIDTH)
		ht->ctrl[ht->capacity + i] = salt;
}

/* The first empty slot of the probe sequence of hash: the key is known to be absent */
static inline uint32
ht_find_empty(const SiftSaltedHT *ht, uint64 hash)
{
	uint32		pos = (uint32) hash & ht->mask;

	for (uint32 step = SIFT_HT_GROUP_WIDTH;; step += SIFT_HT_GROUP_WIDTH)
	{
		const SiftHtMask e = sift_ht_match16_fast(ht->ctrl + pos, 0);

		if (e)
			return (pos + SIFT_HT_MASK_NEXT(e)) & ht->mask;
		pos = (pos + step) & ht->mask;
	}
}

static void
ht_grow(SiftSaltedHT *ht, uint32 capacity)
{
	SiftSaltedHT old = *ht;

	ht_alloc(ht, capacity);
	for (uint32 i = 0; i < old.capacity; i++)
		if (old.ctrl[i] & 0x80)
		{
			const uint32 at = ht_find_empty(ht, old.slots[i].hash64);

			ht->slots[at] = old.slots[i];
			ctrl_set(ht, at, SIFT_HASH_SALT(old.slots[i].hash64));
			ht->count++;
		}
	pfree(old.ctrl);
	pfree(old.slots);
}

void
sift_ht_reserve(SiftSaltedHT *ht, uint32 n)
{
	uint32		cap = ht->capacity;

	while ((uint64) (ht->count + n) > (uint64) cap * SIFT_HT_MAX_LOAD_NUM / SIFT_HT_MAX_LOAD_DEN)
		cap <<= 1;
	if (cap != ht->capacity)
		ht_grow(ht, cap);
}

/* A zeroed state block for a new group */
static uint32
ht_ext_new(SiftSaltedHT *ht)
{
	if (ht->ext_used == ht->ext_cap)
	{
		const uint32 cap = ht->ext_cap ? ht->ext_cap << 1 : SIFT_HT_MIN_SLOTS;
		uint8	   *slab = MemoryContextAllocAligned(ht->cxt, (size_t) cap * ht->ext_stride, SIFT_HT_EXT_ALIGN, MCXT_ALLOC_ZERO);

		if (ht->ext_slab)
		{
			memcpy(slab, ht->ext_slab, (size_t) ht->ext_used * ht->ext_stride);
			pfree(ht->ext_slab);
		}
		ht->ext_slab = slab;
		ht->ext_cap = cap;
	}
	memset(ht->ext_slab + (size_t) ht->ext_used * ht->ext_stride, 0, ht->ext_stride);
	return ht->ext_used++;
}

static uint32
ht_arena_put(SiftSaltedHT *ht, const void *key, uint32 len)
{
	const uint32 off = ht->arena_used;

	if ((uint64) off + len > UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the group keys of an aggregate are over 4 GB"),
						errdetail("A table of wide keys addresses its keys with 32 bits."), errhint("Group by fewer or shorter key columns.")));
	if (off + len > ht->arena_cap)
	{
		uint32		cap = Max(ht->arena_cap, 4096);

		while (cap < off + len)
			cap = cap > UINT32_MAX / 2 ? UINT32_MAX : cap << 1;
		ht->arena = ht->arena ? repalloc(ht->arena, cap) : MemoryContextAlloc(ht->cxt, cap);
		ht->arena_cap = cap;
	}
	if (len > 0)
		memcpy(ht->arena + off, key, len);
	ht->arena_used = off + len;
	return off;
}

/* The key of a probe: 16 inline bytes, or len bytes and the null flags */
typedef struct HtKey
{
	const SiftInlineKey16 *inl;
	const void *bytes;
	uint32		len;
	uint64		nulls;
} HtKey;

static inline bool
ht_key_eq(const SiftSaltedHT *ht, const SiftAggSlot64 *s, const HtKey *k)
{
	if (!ht->key_is_wide)
		return s->key.u64[0] == k->inl->u64[0] && s->key.u64[1] == k->inl->u64[1];
	return s->key.wide.key_len == k->len && s->key.wide.key_nulls == k->nulls &&
		(k->len == 0 || memcmp(ht->arena + s->key.wide.arena_off, k->bytes, k->len) == 0);
}

static inline SiftAggSlot64 *
ht_new_slot(SiftSaltedHT *ht, uint32 at, uint64 hash, const HtKey *k)
{
	SiftAggSlot64 *s = &ht->slots[at];

	memset(s, 0, sizeof(*s));
	s->hash64 = hash;
	s->ext_idx = SIFT_HT_NO_EXT;
	if (ht->key_is_wide)
	{
		s->key.wide.arena_off = ht_arena_put(ht, k->bytes, k->len);
		s->key.wide.key_len = k->len;
		s->key.wide.key_nulls = k->nulls;
	}
	else
		s->key = *k->inl;
	if (ht->ext_stride)
		s->ext_idx = ht_ext_new(ht);
	ctrl_set(ht, at, SIFT_HASH_SALT(hash));
	ht->count++;
	return s;
}

static inline SiftAggSlot64 *
ht_upsert(SiftSaltedHT *ht, uint64 hash, const HtKey *k, bool *found)
{
	const uint8 salt = SIFT_HASH_SALT(hash);
	uint32		pos = (uint32) hash & ht->mask;

	for (uint32 step = SIFT_HT_GROUP_WIDTH;; step += SIFT_HT_GROUP_WIDTH)
	{
		SiftHtMask	m = sift_ht_match16_fast(ht->ctrl + pos, salt),
					e;

		for (; m; SIFT_HT_MASK_CLEAR(m))
		{
			SiftAggSlot64 *s = &ht->slots[(pos + SIFT_HT_MASK_NEXT(m)) & ht->mask];

			if (ht_key_eq(ht, s, k))
			{
				*found = true;
				return s;
			}
		}
		if ((e = sift_ht_match16_fast(ht->ctrl + pos, 0)) != 0)
		{
			*found = false;
			return ht_new_slot(ht, (pos + SIFT_HT_MASK_NEXT(e)) & ht->mask, hash, k);
		}
		pos = (pos + step) & ht->mask;
	}
}

SiftAggSlot64 *
sift_ht_upsert(SiftSaltedHT *ht, uint64 hash, const SiftInlineKey16 *key, bool *found)
{
	const HtKey k = {.inl = key};

	Assert(!ht->key_is_wide);
	if (ht->count >= ht->grow_at)
		ht_grow(ht, ht->capacity << 1);
	return ht_upsert(ht, hash, &k, found);
}

SiftAggSlot64 *
sift_ht_upsert_wide(SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found)
{
	const HtKey k = {.bytes = key, .len = len, .nulls = nulls};

	Assert(ht->key_is_wide);
	if (ht->count >= ht->grow_at)
		ht_grow(ht, ht->capacity << 1);
	return ht_upsert(ht, hash, &k, found);
}

SiftAggSlot64 *
sift_ht_upsert_str(SiftSaltedHT *ht, const SiftStrView *v, bool *found)
{
	const uint8 *b = sift_strview_bytes(v);

	SIFT_INVARIANT(ht->key_is_text, "sift: sift_ht_upsert_str on a table not made by sift_ht_init_str");
	return sift_ht_upsert_wide(ht, sift_hash_bytes(b, v->len), b, v->len, 0, found);
}

/* The probe loop of a batch */
static void
ht_upsert_run(SiftSaltedHT *ht, const uint64 *hashes, const SiftInlineKey16 *keys, uint32 n, SiftAggSlot64 **out)
{
	for (uint32 i = 0; i < n; i++)
	{
		const HtKey k = {.inl = &keys[i]};
		bool		found;

		if (i + SIFT_HT_PREFETCH < n)
		{
			const uint32 ahead = (uint32) hashes[i + SIFT_HT_PREFETCH] & ht->mask;

			__builtin_prefetch(ht->ctrl + ahead);
			__builtin_prefetch(&ht->slots[ahead]);
		}
		out[i] = ht_upsert(ht, hashes[i], &k, &found);
	}
}

uint32
sift_ht_upsert_batch(SiftSaltedHT *ht, const uint64 *hashes, const SiftInlineKey16 *keys, uint32 n, SiftAggSlot64 **out)
{
	const uint32 before = ht->count;

	Assert(!ht->key_is_wide);
	sift_ht_reserve(ht, n);
	ht_upsert_run(ht, hashes, keys, n, out);
	return ht->count - before;
}
