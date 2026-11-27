/*
 * agg/ht/ht.c - the salted hash table of agg/ht/ht.h.
 */
#include "core/host.h"

#include "utils/lsyscache.h"

#include "agg/ht/dist.h"
#include "agg/ht/ht.h"
#include "core/memtrack.h"

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
	ht->ctrl = MemoryContextAllocExtended(ht->cxt, (size_t) capacity + SIFT_HT_GROUP_WIDTH, MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	ht->slots = MemoryContextAllocAligned(ht->cxt, sizeof(SiftAggSlot64) * (size_t) capacity, 64, MCXT_ALLOC_HUGE);
}

void
sift_ht_init_ex(SiftSaltedHT *ht, uint32 min_slots, bool wide_key, uint32 ext_stride, uint32 ext_dist_off)
{
	uint32		cap = SIFT_HT_MIN_SLOTS;

	while (cap < min_slots)
		cap <<= 1;
	memset(ht, 0, sizeof(*ht));
	ht->gen = 1;
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
	const uint64 bytes = sift_ht_bytes(ht);

	ht_free_sets(ht);
	pfree(ht->ctrl);
	pfree(ht->slots);
	if (ht->ext_slab)
		pfree(ht->ext_slab);
	if (ht->arena)
		pfree(ht->arena);
	memset(ht, 0, sizeof(*ht));
	sift_mem_release_os(bytes);
}

static inline void
ht_new_gen(SiftSaltedHT *ht)
{
	if (++ht->gen == 0)
		ht->gen = 1;
}

/* MEM[MEM-AGG-01]: SiftSaltedHT primary table (capacity * 65B: 64B SiftAggSlot64 + 1B ctrl; a grow holds old + 2x new, so 3x the old table, for the length of the rehash) */
void
sift_ht_clear(SiftSaltedHT *ht)
{
	ht_free_sets(ht);
	ht_new_gen(ht);
	memset(ht->ctrl, 0, ht->capacity + SIFT_HT_GROUP_WIDTH);
	ht->count = 0;
	ht->ext_used = 0;
	ht->arena_used = 0;
}

/* A cleared table given back all but min_slots of its slots and all its state blocks and key arena: what a spill leaves resident is a table of the size that gathers the next groups */
void
sift_ht_shrink_to(SiftSaltedHT *ht, uint32 min_slots)
{
	uint32		cap = SIFT_HT_MIN_SLOTS;

	SIFT_INVARIANT(ht->count == 0, "sift: a table shrinks only when it is cleared");
	while (cap < min_slots)
		cap <<= 1;
	if (ht->ext_slab)
		pfree(ht->ext_slab);
	if (ht->arena)
		pfree(ht->arena);
	ht->ext_slab = NULL;
	ht->ext_cap = 0;
	ht->arena = NULL;
	ht->arena_cap = 0;
	if (ht->capacity <= cap)
		return;
	pfree(ht->ctrl);
	pfree(ht->slots);
	ht_alloc(ht, cap);
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
	ht_new_gen(ht);
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

/* the capacity of a table that holds n groups: the power of 2 of cap or above it, at most the largest one a uint32 mask covers */
static uint32
ht_capacity_for(uint32 cap, uint64 n)
{
	while (n > (uint64) cap * SIFT_HT_MAX_LOAD_NUM / SIFT_HT_MAX_LOAD_DEN)
	{
		if (cap > PG_UINT32_MAX / 2)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("an aggregate has more groups than one group table holds"),
							errhint("Group by fewer keys, or run the statement with sift.enable_agg = off.")));
		cap <<= 1;
	}
	return cap;
}

void
sift_ht_reserve(SiftSaltedHT *ht, uint32 n)
{
	const uint32 cap = ht_capacity_for(ht->capacity, (uint64) ht->count + n);

	if (cap != ht->capacity)
		ht_grow(ht, cap);
}

/* MEM[MEM-AGG-01]: SiftSaltedHT ext_slab (ext_cap * ext_stride, doubles while the old slab is live: 3x for the copy) */
static void
ext_resize(SiftSaltedHT *ht, uint32 cap)
{
	uint8	   *slab = MemoryContextAllocAligned(ht->cxt, (size_t) cap * ht->ext_stride, SIFT_HT_EXT_ALIGN, MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	if (ht->ext_slab)
	{
		memcpy(slab, ht->ext_slab, (size_t) ht->ext_used * ht->ext_stride);
		pfree(ht->ext_slab);
	}
	ht->ext_slab = slab;
	ht->ext_cap = cap;
}

void
sift_ht_presize(SiftSaltedHT *ht, uint32 n)
{
	sift_ht_reserve(ht, n);
	if (n > ht->ext_cap)
		ext_resize(ht, n);
}

/* A zeroed state block for a new group */
static uint32
ht_ext_new(SiftSaltedHT *ht)
{
	if (ht->ext_used == ht->ext_cap)
		ext_resize(ht, ht->ext_cap ? ht->ext_cap << 1 : SIFT_HT_MIN_SLOTS);
	memset(ht->ext_slab + (size_t) ht->ext_used * ht->ext_stride, 0, ht->ext_stride);
	return ht->ext_used++;
}

/* MEM[MEM-AGG-01]: SiftSaltedHT key arena (the bytes of the wide keys, doubling up to MaxAllocSize with the old arena live; the spill check counts arena_cap) */
static uint32
ht_arena_put(SiftSaltedHT *ht, const void *key, uint32 len, const void *tail, uint32 tail_len)
{
	const uint32 off = ht->arena_used;
	const uint64 total = (uint64) len + tail_len;

	if ((uint64) off + total > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the group keys of an aggregate are over 1 GB"),
						errdetail("A table of wide keys holds its keys in one allocation."), errhint("Group by fewer or shorter key columns.")));
	if (off + total > ht->arena_cap)
	{
		uint32		cap = Max(ht->arena_cap, 4096);

		while (cap < off + total)
			cap = cap > MaxAllocSize / 2 ? MaxAllocSize : cap << 1;
		ht->arena = ht->arena ? repalloc(ht->arena, cap) : MemoryContextAlloc(ht->cxt, cap);
		ht->arena_cap = cap;
	}
	if (len > 0)
		memcpy(ht->arena + off, key, len);
	if (tail_len > 0)
		memcpy(ht->arena + off + len, tail, tail_len);
	ht->arena_used = off + (uint32) total;
	return off;
}

/* The key of a probe: 16 inline bytes, or len bytes (and tail_len more after them: a string behind its length, whose bytes are copied only when the key is new) and the null flags */
typedef struct HtKey
{
	const SiftInlineKey16 *inl;
	const void *bytes;
	uint32		len;
	const void *tail;
	uint32		tail_len;
	uint64		nulls;
} HtKey;

static inline bool
ht_key_eq(const SiftSaltedHT *ht, const SiftAggSlot64 *s, const HtKey *k)
{
	const uint8 *at;

	if (!ht->key_is_wide)
		return s->key.u64[0] == k->inl->u64[0] && s->key.u64[1] == k->inl->u64[1];
	if (s->key.wide.key_len != k->len + k->tail_len || s->key.wide.key_nulls != k->nulls)
		return false;
	at = ht->arena + s->key.wide.arena_off;
	return (k->len == 0 || memcmp(at, k->bytes, k->len) == 0) && (k->tail_len == 0 || memcmp(at + k->len, k->tail, k->tail_len) == 0);
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
		s->key.wide.arena_off = ht_arena_put(ht, k->bytes, k->len, k->tail, k->tail_len);
		s->key.wide.key_len = k->len + k->tail_len;
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

/* grow_on_miss: a key that is new when the load limit is reached grows the table first (a batch, whose hits grow nothing); else the caller has made room */
static inline SiftAggSlot64 *
ht_upsert(SiftSaltedHT *ht, uint64 hash, const HtKey *k, bool *found, bool grow_on_miss)
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
			if (grow_on_miss && ht->lock_at != 0 && ht->count >= ht->lock_at)
				return (*found = false, NULL);
			if (grow_on_miss && ht->count >= ht->grow_at)
			{
				ht_grow(ht, ht_capacity_for(ht->capacity, (uint64) ht->count + 1));
				return ht_upsert(ht, hash, k, found, false);
			}
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
		ht_grow(ht, ht_capacity_for(ht->capacity, (uint64) ht->count + 1));
	return ht_upsert(ht, hash, &k, found, false);
}

SiftAggSlot64 *
sift_ht_upsert_wide(SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found)
{
	const HtKey k = {.bytes = key, .len = len, .nulls = nulls};

	Assert(ht->key_is_wide);
	if (ht->count >= ht->grow_at)
		ht_grow(ht, ht_capacity_for(ht->capacity, (uint64) ht->count + 1));
	return ht_upsert(ht, hash, &k, found, false);
}

SiftAggSlot64 *
sift_ht_upsert_wide_lazy(SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found)
{
	const HtKey k = {.bytes = key, .len = len, .nulls = nulls};

	Assert(ht->key_is_wide);
	return ht_upsert(ht, hash, &k, found, true);
}

SiftAggSlot64 *
sift_ht_upsert_str(SiftSaltedHT *ht, const SiftStrView *v, bool *found)
{
	const uint8 *b = sift_strview_bytes(v);

	SIFT_INVARIANT(ht->key_is_text, "sift: sift_ht_upsert_str on a table not made by sift_ht_init_str");
	return sift_ht_upsert_wide(ht, sift_hash_bytes(b, v->len), b, v->len, 0, found);
}

static inline SiftAggSlot64 *
ht_find(const SiftSaltedHT *ht, uint64 hash, const HtKey *k)
{
	const uint8 salt = SIFT_HASH_SALT(hash);
	uint32		pos = (uint32) hash & ht->mask;

	for (uint32 step = SIFT_HT_GROUP_WIDTH;; step += SIFT_HT_GROUP_WIDTH)
	{
		SiftHtMask	m = sift_ht_match16_fast(ht->ctrl + pos, salt);

		for (; m; SIFT_HT_MASK_CLEAR(m))
		{
			SiftAggSlot64 *s = &ht->slots[(pos + SIFT_HT_MASK_NEXT(m)) & ht->mask];

			if (ht_key_eq(ht, s, k))
				return s;
		}
		if (sift_ht_match16_fast(ht->ctrl + pos, 0) != 0)
			return NULL;
		pos = (pos + step) & ht->mask;
	}
}

SiftAggSlot64 *
sift_ht_find(const SiftSaltedHT *ht, uint64 hash, const SiftInlineKey16 *key)
{
	const HtKey k = {.inl = key};

	return ht_find(ht, hash, &k);
}

SiftAggSlot64 *
sift_ht_find_wide(const SiftSaltedHT *ht, uint64 hash, const void *key, uint32 len, uint64 nulls)
{
	const HtKey k = {.bytes = key, .len = len, .nulls = nulls};

	return ht_find(ht, hash, &k);
}

SiftAggSlot64 *
sift_ht_upsert_lenpfx_lazy(SiftSaltedHT *ht, uint64 hash, const void *p, uint32 len, bool *found)
{
	const HtKey k = {.bytes = &len,.len = sizeof(len),.tail = p,.tail_len = len};

	Assert(ht->key_is_wide);
	return ht_upsert(ht, hash, &k, found, true);
}

SiftAggSlot64 *
sift_ht_find_lenpfx(const SiftSaltedHT *ht, uint64 hash, const void *p, uint32 len)
{
	const HtKey k = {.bytes = &len,.len = sizeof(len),.tail = p,.tail_len = len};

	return ht_find(ht, hash, &k);
}

/* The probe loop of a batch */
static void
ht_upsert_run(SiftSaltedHT *ht, const uint64 *hashes, const SiftInlineKey16 *keys, uint32 n, SiftAggSlot64 **out)
{
	uint32		gen = ht->gen;

	for (uint32 i = 0; i < n; i++)
	{
		const HtKey k = {.inl = &keys[i]};
		bool		found;

		if (i + SIFT_HT_PREFETCH < n)
			sift_ht_prefetch_group(ht, hashes[i + SIFT_HT_PREFETCH]);
		if (i + SIFT_HT_PREFETCH_SLOT < n)
			sift_ht_prefetch_slot(ht, hashes[i + SIFT_HT_PREFETCH_SLOT]);
		out[i] = ht_upsert(ht, hashes[i], &k, &found, true);
		if (ht->gen != gen)
		{
			for (uint32 j = 0; j < i; j++)
				out[j] = sift_ht_find(ht, hashes[j], &keys[j]);
			gen = ht->gen;
		}
	}
}

uint32
sift_ht_upsert_batch(SiftSaltedHT *ht, const uint64 *hashes, const SiftInlineKey16 *keys, uint32 n, SiftAggSlot64 **out)
{
	const uint32 before = ht->count;

	Assert(!ht->key_is_wide);
	ht_upsert_run(ht, hashes, keys, n, out);
	return ht->count - before;
}
