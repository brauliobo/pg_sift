/*
 * agg/distinct/distinct.c - the exact COUNT(DISTINCT) set (v6_distinct.h).
 */
#include "agg/dictseen.h"
#include "agg/distinct/distinct.h"
#include "agg/spill.h"

#define DIST_NO_PART		UINT32_MAX
#define DIST_MIN_CAP		256
#define DIST_MAX_CHUNK		(64 * 1024)
#define DIST_MIN_CHUNK		1024
#define DIST_MIN_BUDGET		(32 * 1024)

typedef struct DistEntry
{
	uint64		hash;
	const uint8 *key;
	uint32		len;
	uint32		slot;
} DistEntry;

typedef struct DistChunk
{
	struct DistChunk *next;
	uint32		size;
	uint32		used;
	uint8		data[];
} DistChunk;

struct SiftDistinct
{
	MemoryContext cx;
	uint32		level;
	uint64		budget;
	uint64		expect;
	DistEntry  *tab;
	uint32		cap;
	uint64		nent;
	DistChunk  *chunks;
	uint64		arena_bytes;
	uint32		chunk_size;
	uint64	   *count;
	uint32		ncount;
	SiftSpillSet *spill;
	uint32		resident;
	bool		by_prefix;		/* the partitions of the spill are those of the groups (a grouped set that held more than one when it first spilled) */
	uint8	   *scratch;
	uint32		scratch_cap;
	bool		finished;
	SiftDistinctGroupEmit emit;	/* a grouped set: the keys are [plen][prefix][value] and the counts go to emit */
	void	   *emit_arg;
	uint32		value_slots;	/* the slots whose keys go to value_visit when the set is finished */
	SiftDistinctVisit value_visit;
	void	   *value_arg;
	SiftDistinctStats st;
};

/* the count of the keys of one (slot, prefix) the keys of a table hold, until it is handed to emit */
typedef struct DistGroupCount
{
	uint64		hash;
	const uint8 *prefix;
	uint32		plen;
	uint32		slot;
	uint64		count;
} DistGroupCount;

/* the table a reset keeps for the next use of the set: a larger one is given back */
#define DIST_RESET_CAP		4096
#define DIST_GROUP_CAP		512
#define DIST_GROUP_FLUSH	(DIST_GROUP_CAP * 7 / 10)

/* MEM[MEM-AGG-08]: SiftDistinct budget accounting (table, arena, counts, the partition files of the spill set at 8KB a BufFile, and the spill scratch; was without the last two) */
static uint64
dist_bytes(const SiftDistinct *d)
{
	return (uint64) d->cap * sizeof(DistEntry) + d->arena_bytes + (uint64) d->ncount * sizeof(uint64) +
		(d->spill != NULL ? (uint64) sift_spill_nparts_of(d->spill) * 8192 : 0) + d->scratch_cap;
}

static void
dist_note_peak(SiftDistinct *d)
{
	d->st.peak_bytes = Max(d->st.peak_bytes, dist_bytes(d));
}

static uint64
dist_hash(const SiftDistinct *d, uint32 slot, const void *key, uint32 len)
{
	uint64		h = hash_bytes_extended((const unsigned char *) key, (int) len, d->level);

	h ^= ((uint64) slot + 1) * UINT64_C(0x9E3779B97F4A7C15);
	h *= UINT64_C(0xD6E8FEB86659FD93);
	return h ^ (h >> 32);
}

static SiftDistinct *
dist_new(MemoryContext cx, uint32 level, uint64 budget, uint64 expect)
{
	SiftDistinct *d = MemoryContextAllocZero(cx, sizeof(SiftDistinct));

	d->cx = cx;
	d->level = level;
	d->budget = Max(budget, DIST_MIN_BUDGET);
	d->expect = expect;
	d->resident = DIST_NO_PART;
	d->chunk_size = (uint32) Min(DIST_MAX_CHUNK, Max(DIST_MIN_CHUNK, d->budget / 16));
	return d;
}

SiftDistinct *
sift_distinct_create(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes)
{
	return dist_new(cx, 0, budget_bytes, expect_bytes);
}

SiftDistinct *
sift_distinct_create_grouped(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes, SiftDistinctGroupEmit emit, void *emit_arg)
{
	SiftDistinct *d = dist_new(cx, 0, budget_bytes, expect_bytes);

	d->emit = emit;
	d->emit_arg = emit_arg;
	return d;
}

static void
dist_free_arena(SiftDistinct *d)
{
	while (d->chunks != NULL)
	{
		DistChunk  *c = d->chunks;

		d->chunks = c->next;
		pfree(c);
	}
	d->arena_bytes = 0;
}

static uint32
dist_chunk_alloc_size(const SiftDistinct *d, uint32 len)
{
	return Max(d->chunk_size, (uint32) sizeof(DistChunk) + len);
}

static const uint8 *
dist_arena_copy(SiftDistinct *d, const void *key, uint32 len)
{
	DistChunk  *c = d->chunks;
	uint8	   *dst;

	if (c == NULL || c->size - c->used < len)
	{
		uint32		sz = dist_chunk_alloc_size(d, len);

		c = MemoryContextAlloc(d->cx, sz);
		c->size = sz - (uint32) sizeof(DistChunk);
		c->used = 0;
		c->next = d->chunks;
		d->chunks = c;
		d->arena_bytes += sz;
	}
	dst = c->data + c->used;
	if (len > 0)
		memcpy(dst, key, len);
	c->used += len;
	return dst;
}

static void
dist_count_grow(SiftDistinct *d, uint32 slot)
{
	uint32		ncap = Max(Max(d->ncount * 2, slot + 1), 64);
	uint64	   *n = MemoryContextAllocZero(d->cx, sizeof(uint64) * ncap);

	if (d->count != NULL)
	{
		memcpy(n, d->count, sizeof(uint64) * d->ncount);
		pfree(d->count);
	}
	d->count = n;
	d->ncount = ncap;
}

static void
dist_tab_insert_raw(DistEntry *tab, uint32 cap, const DistEntry *e)
{
	uint32		i = (uint32) e->hash & (cap - 1);

	while (tab[i].key != NULL)
		i = (i + 1) & (cap - 1);
	tab[i] = *e;
}

static void
dist_tab_resize(SiftDistinct *d, uint32 ncap)
{
	DistEntry  *n = MemoryContextAllocZero(d->cx, sizeof(DistEntry) * ncap);

	for (uint32 i = 0; i < d->cap; i++)
		if (d->tab[i].key != NULL)
			dist_tab_insert_raw(n, ncap, &d->tab[i]);
	if (d->tab != NULL)
		pfree(d->tab);
	d->tab = n;
	d->cap = ncap;
}

#define DIST_PACK_MAX		254

/* A spilled record is its slot and key without the zero bytes (the integers of a key are mostly zeros): its length, a bitmap of the bytes that are not zero, and those. A longer one is kept whole after a byte of 255 */
static uint32
dist_pack(uint8 *out, const uint8 *in, uint32 n)
{
	const uint32 m = (n + 7) / 8;
	uint32		o = 1 + m;

	if (n > DIST_PACK_MAX)
	{
		out[0] = UINT8_MAX;
		memcpy(out + 1, in, n);
		return n + 1;
	}
	out[0] = (uint8) n;
	memset(out + 1, 0, m);
	for (uint32 i = 0; i < n; i++)
		if (in[i] != 0)
		{
			out[1 + i / 8] |= (uint8) (1 << (i % 8));
			out[o++] = in[i];
		}
	return o;
}

/* The n bytes of the record of len bytes dist_pack made: in rec (DIST_PACK_MAX bytes), or where they are when it is whole */
static const uint8 *
dist_unpack(const uint8 *in, uint32 len, uint8 *rec, uint32 *n)
{
	uint32		o;

	if (in[0] == UINT8_MAX)
	{
		*n = len - 1;
		return in + 1;
	}
	*n = in[0];
	o = 1 + (*n + 7) / 8;
	memset(rec, 0, *n);
	for (uint32 i = 0; i < *n; i++)
		if (in[1 + i / 8] & (1 << (i % 8)))
			rec[i] = in[o++];
	return rec;
}

static void
dist_scratch(SiftDistinct *d, uint32 need)
{
	if (need > d->scratch_cap)
	{
		if (d->scratch != NULL)
			pfree(d->scratch);
		d->scratch_cap = Max(need, 256);
		d->scratch = MemoryContextAlloc(d->cx, d->scratch_cap);
	}
}

/* The partition of a key: a grouped set that holds several groups when it first spills keeps the keys of a group together, so that its count reaches emit once and a pass has few groups to count; one that holds a single group splits it by the whole key */
static uint32
dist_part(const SiftDistinct *d, uint64 h, const void *key)
{
	uint32		plen;

	if (d->by_prefix)
	{
		memcpy(&plen, key, sizeof(plen));
		h = hash_bytes_extended((const unsigned char *) key + sizeof(plen), (int) plen, d->level);
	}
	return sift_spill_part_of(h, sift_spill_nparts_of(d->spill));
}

static bool
dist_has_groups(const DistEntry *tab, uint32 cap)
{
	const DistEntry *first = NULL;

	for (uint32 i = 0; i < cap; i++)
	{
		uint32		plen;
		uint32		fplen;

		if (tab[i].key == NULL)
			continue;
		memcpy(&plen, tab[i].key, sizeof(plen));
		if (first == NULL)
			first = &tab[i];
		memcpy(&fplen, first->key, sizeof(fplen));
		if (plen != fplen || memcmp(tab[i].key + sizeof(plen), first->key + sizeof(fplen), plen) != 0)
			return true;
	}
	return false;
}

static void
dist_put_spill(SiftDistinct *d, uint32 part, uint32 slot, const void *key, uint32 len)
{
	const uint32 n = len + (uint32) sizeof(uint32);

	dist_scratch(d, 2 * n + n / 8 + 2);
	memcpy(d->scratch, &slot, sizeof(uint32));
	if (len > 0)
		memcpy(d->scratch + sizeof(uint32), key, len);
	sift_spill_put(d->spill, part, 0, d->scratch + n, dist_pack(d->scratch + n, d->scratch, n));
}

/*
 * The set is over its budget: the keys that are not in the resident partition go to their partition files (and are no longer counted: the partition pass counts them), and the others stay in a
 * smaller table. When the set already spills, the resident partition goes too and every row spills from now on.
 */
/* MEM[MEM-AGG-08]: dist_evict resident table (the old table and arena are live while the kept entries are copied into the new ones: old + new at once) */
static void
dist_evict(SiftDistinct *d)
{
	uint32		keep;
	DistEntry  *old = d->tab;
	uint32		oldcap = d->cap;
	DistChunk  *oldchunks = d->chunks;
	uint64		kept = 0;

	if (d->spill == NULL)
	{
		d->spill = sift_spill_create(d->cx, sift_spill_nparts(d->expect ? d->expect : d->budget * 4, d->budget));
		d->by_prefix = d->emit != NULL && dist_has_groups(old, oldcap);		d->resident = 0;
	}
	else
		d->resident = DIST_NO_PART;
	keep = d->resident;
	d->st.max_level = Max(d->st.max_level, d->level + 1);

	for (uint32 i = 0; i < oldcap; i++)
		if (old[i].key != NULL && dist_part(d, old[i].hash, old[i].key) == keep)
			kept++;
	d->tab = NULL;
	d->cap = 0;
	d->chunks = NULL;
	d->arena_bytes = 0;
	if (kept > 0)
	{
		uint32		ncap = DIST_MIN_CAP;

		while ((uint64) ncap * 7 < kept * 10 + 10)
			ncap <<= 1;
		dist_tab_resize(d, ncap);
	}
	d->nent = 0;
	for (uint32 i = 0; i < oldcap; i++)
	{
		DistEntry  *e = &old[i];

		if (e->key == NULL)
			continue;
		if (dist_part(d, e->hash, e->key) == keep)
		{
			DistEntry	ne = *e;

			ne.key = dist_arena_copy(d, e->key, e->len);
			dist_tab_insert_raw(d->tab, d->cap, &ne);
			d->nent++;
		}
		else
		{
			dist_put_spill(d, dist_part(d, e->hash, e->key), e->slot, e->key, e->len);
			if (d->emit == NULL)
				d->count[e->slot]--;
		}
	}
	if (old != NULL)
		pfree(old);
	while (oldchunks != NULL)
	{
		DistChunk  *c = oldchunks;

		oldchunks = c->next;
		pfree(c);
	}
	dist_note_peak(d);
}

/* Makes room for one more key of len bytes; false: the row went to a partition file instead of the table */
static bool
dist_reserve(SiftDistinct *d, uint64 hash, uint32 slot, const void *key, uint32 len)
{
	for (;;)
	{
		bool		grow = d->cap == 0 || (d->nent + 1) * 10 > (uint64) d->cap * 7;
		bool		chunk = d->chunks == NULL || d->chunks->size - d->chunks->used < len;
		uint64		extra;

		if (!grow && !chunk)
			return true;
		extra = (grow ? (uint64) Max(d->cap * 2, DIST_MIN_CAP) * sizeof(DistEntry) : 0) + (chunk ? dist_chunk_alloc_size(d, len) : 0);
		if (d->level < SIFT_DISTINCT_MAX_LEVEL && d->nent > 0 && dist_bytes(d) + extra > d->budget)
		{
			uint32		part;

			dist_evict(d);
			part = dist_part(d, hash, key);
			if (part != d->resident)
			{
				dist_put_spill(d, part, slot, key, len);
				return false;
			}
			continue;
		}
		if (grow)
			dist_tab_resize(d, Max(d->cap * 2, DIST_MIN_CAP));
		if (chunk)
		{
			uint32		sz = dist_chunk_alloc_size(d, len);
			DistChunk  *c = MemoryContextAlloc(d->cx, sz);

			c->size = sz - (uint32) sizeof(DistChunk);
			c->used = 0;
			c->next = d->chunks;
			d->chunks = c;
			d->arena_bytes += sz;
		}
		dist_note_peak(d);
	}
}

/* The key with its hash h, which is the same for the same bytes whatever adds them to this set */
static void
dist_add_with(SiftDistinct *d, uint64 h, uint32 slot, const void *key, uint32 len)
{
	uint32		i;
	DistEntry	e;

	SIFT_INVARIANT(!d->finished, "count distinct: row added after the set was finished");
	d->st.adds++;
	if (d->spill != NULL)
	{
		uint32		part = dist_part(d, h, key);

		if (part != d->resident)
		{
			dist_put_spill(d, part, slot, key, len);
			return;
		}
	}
	if (d->cap > 0)
	{
		for (i = (uint32) h & (d->cap - 1); d->tab[i].key != NULL; i = (i + 1) & (d->cap - 1))
			if (d->tab[i].hash == h && d->tab[i].slot == slot && d->tab[i].len == len && (len == 0 || memcmp(d->tab[i].key, key, len) == 0))
				return;
	}
	if (!dist_reserve(d, h, slot, key, len))
		return;
	e.hash = h;
	e.key = dist_arena_copy(d, key, len);
	e.len = len;
	e.slot = slot;
	dist_tab_insert_raw(d->tab, d->cap, &e);
	d->nent++;
	if (d->emit != NULL)
		return;
	if (slot >= d->ncount)
		dist_count_grow(d, slot);
	d->count[slot]++;
}

static void
dist_add_hashed(SiftDistinct *d, uint32 slot, const void *key, uint32 len)
{
	dist_add_with(d, dist_hash(d, slot, key, len), slot, key, len);
}

void
sift_distinct_add(SiftDistinct *d, uint32 slot, const void *key, uint32 len)
{
	dist_add_hashed(d, slot, key, len);
}

void
sift_distinct_add_prehashed(SiftDistinct *d, uint32 slot, const void *key, uint32 len, uint64 hash)
{
	dist_add_with(d, hash, slot, key, len);
}

void
sift_distinct_foreach(const SiftDistinct *d, SiftDistinctVisit visit, void *arg)
{
	for (uint32 i = 0; i < d->cap; i++)
		if (d->tab[i].key != NULL)
			visit(arg, d->tab[i].slot, d->tab[i].key, d->tab[i].len);
}

void
sift_distinct_visit_values(SiftDistinct *d, uint32 slots, SiftDistinctVisit visit, void *arg)
{
	d->value_slots = slots;
	d->value_visit = visit;
	d->value_arg = arg;
}

void
sift_distinct_foreach_hashed(const SiftDistinct *d, SiftDistinctVisitHashed visit, void *arg)
{
	for (uint32 i = 0; i < d->cap; i++)
		if (d->tab[i].key != NULL)
			visit(arg, d->tab[i].slot, d->tab[i].key, d->tab[i].len, d->tab[i].hash);
}

void
sift_distinct_reset(SiftDistinct *d)
{
	if (d->spill != NULL)
		sift_spill_free(d->spill);
	d->spill = NULL;
	d->resident = DIST_NO_PART;
	d->finished = false;
	if (d->tab != NULL && d->cap > DIST_RESET_CAP)
	{
		pfree(d->tab);
		d->tab = NULL;
		d->cap = 0;
	}
	else if (d->tab != NULL)
		memset(d->tab, 0, (Size) d->cap * sizeof(DistEntry));
	d->nent = 0;
	dist_free_arena(d);
	if (d->count != NULL)
		memset(d->count, 0, (Size) d->ncount * sizeof(uint64));
	d->st = (SiftDistinctStats) {0};
}

void
sift_distinct_add_u64(SiftDistinct *d, uint32 slot, uint64 v)
{
	dist_add_hashed(d, slot, &v, sizeof(v));
}

static void
dist_merge_child(SiftDistinct *d, SiftDistinct *c)
{
	for (uint32 s = 0; s < c->ncount; s++)
	{
		if (c->count[s] == 0)
			continue;
		if (s >= d->ncount)
			dist_count_grow(d, s);
		d->count[s] += c->count[s];
	}
	d->st.passes += c->st.passes + 1;
	d->st.max_level = Max(d->st.max_level, c->st.max_level);
	d->st.spilled_records += c->st.spilled_records;
	d->st.spilled_bytes += c->st.spilled_bytes;
	d->st.peak_bytes = Max(d->st.peak_bytes, c->st.peak_bytes);
}

static void
dist_free_table(SiftDistinct *d)
{
	if (d->tab != NULL)
		pfree(d->tab);
	d->tab = NULL;
	d->cap = 0;
	d->nent = 0;
	dist_free_arena(d);
}

static void
dist_group_flush(SiftDistinct *d, DistGroupCount *tab, uint32 *n)
{
	for (uint32 i = 0; i < DIST_GROUP_CAP; i++)
		if (tab[i].prefix != NULL)
		{
			d->emit(d->emit_arg, tab[i].slot, tab[i].prefix, tab[i].plen, tab[i].count);
			tab[i].prefix = NULL;
		}
	*n = 0;
}

/* The keys of the table counted by (slot, prefix) and handed to emit; the prefixes point into the arena, which outlives this */
static void
dist_group_emit(SiftDistinct *d)
{
	DistGroupCount *tab = MemoryContextAllocZero(d->cx, sizeof(DistGroupCount) * DIST_GROUP_CAP);
	uint32		n = 0;

	for (uint32 k = 0; k < d->cap; k++)
	{
		const DistEntry *e = &d->tab[k];
		uint32		plen;
		const uint8 *prefix;
		uint64		h;
		uint32		i;

		if (e->key == NULL)
			continue;
		SIFT_INVARIANT(e->len >= sizeof(uint32), "count distinct: a grouped key of %u bytes", e->len);
		memcpy(&plen, e->key, sizeof(plen));
		SIFT_INVARIANT(plen <= e->len - sizeof(uint32), "count distinct: a prefix of %u bytes in a key of %u", plen, e->len);
		prefix = e->key + sizeof(plen);
		h = hash_bytes_extended(prefix, (int) plen, e->slot);
		for (i = (uint32) h & (DIST_GROUP_CAP - 1); tab[i].prefix != NULL; i = (i + 1) & (DIST_GROUP_CAP - 1))
			if (tab[i].hash == h && tab[i].slot == e->slot && tab[i].plen == plen && (plen == 0 || memcmp(tab[i].prefix, prefix, plen) == 0))
				break;
		if (tab[i].prefix == NULL)
		{
			tab[i] = (DistGroupCount) {.hash = h,.prefix = prefix,.plen = plen,.slot = e->slot};
			n++;
		}
		tab[i].count++;
		if (n >= DIST_GROUP_FLUSH)
			dist_group_flush(d, tab, &n);
	}
	dist_group_flush(d, tab, &n);
	pfree(tab);
}

static void
dist_visit_values(const SiftDistinct *d)
{
	for (uint32 k = 0; k < d->cap; k++)
		if (d->tab[k].key != NULL && (d->value_slots >> d->tab[k].slot) & 1)
			d->value_visit(d->value_arg, d->tab[k].slot, d->tab[k].key, d->tab[k].len);
}

void
sift_distinct_finish(SiftDistinct *d)
{
	uint8	   *buf = NULL;
	uint32		bufcap = 0;
	uint32		nparts;

	if (d->finished)
		return;
	d->finished = true;
	if (d->emit != NULL)
		dist_group_emit(d);
	if (d->value_slots != 0)
		dist_visit_values(d);
	if (d->spill == NULL)
		return;
	dist_free_table(d);
	nparts = sift_spill_nparts_of(d->spill);
	d->st.spilled_records = sift_spill_total_records(d->spill);
	d->st.spilled_bytes = sift_spill_total_bytes(d->spill);
	for (uint32 p = 0; p < nparts; p++)
	{
		SiftDistinct *c;
		uint64		tag;
		uint32		len;

		if (sift_spill_part_records(d->spill, p) == 0)
			continue;
		sift_spill_rewind(d->spill, p);
		c = dist_new(d->cx, d->level + 1, d->budget, 0);
		c->emit = d->emit;
		c->emit_arg = d->emit_arg;
		c->value_slots = d->value_slots;
		c->value_visit = d->value_visit;
		c->value_arg = d->value_arg;
		while (sift_spill_get(d->spill, p, &tag, &buf, &bufcap, &len))
		{
			uint8		rec[DIST_PACK_MAX];
			uint32		n;
			const uint8 *key = dist_unpack(buf, len, rec, &n);
			uint32		slot;

			memcpy(&slot, key, sizeof(uint32));
			dist_add_hashed(c, slot, key + sizeof(uint32), n - (uint32) sizeof(uint32));
		}
		sift_spill_drop(d->spill, p);
		sift_distinct_finish(c);
		dist_merge_child(d, c);
		sift_distinct_free(c);
	}
	if (buf != NULL)
		pfree(buf);
	sift_spill_free(d->spill);
	d->spill = NULL;
}

uint64
sift_distinct_count(const SiftDistinct *d, uint32 slot)
{
	return slot < d->ncount ? d->count[slot] : 0;
}

uint32
sift_distinct_nslots(const SiftDistinct *d)
{
	return d->ncount;
}

uint64
sift_distinct_bytes(const SiftDistinct *d)
{
	return dist_bytes(d);
}

const SiftDistinctStats *
sift_distinct_stats(const SiftDistinct *d)
{
	return &d->st;
}

void
sift_distinct_free(SiftDistinct *d)
{
	dist_free_table(d);
	if (d->spill != NULL)
		sift_spill_free(d->spill);
	if (d->count != NULL)
		pfree(d->count);
	if (d->scratch != NULL)
		pfree(d->scratch);
	pfree(d);
}

void
sift_dkey_init(SiftDistinctKey *k, MemoryContext cx)
{
	sift_bytes_init(k, cx);
}

void
sift_dkey_reset(SiftDistinctKey *k)
{
	sift_bytes_reset(k);
}

void
sift_dkey_free(SiftDistinctKey *k)
{
	sift_bytes_free(k);
}

void
sift_dkey_null(SiftDistinctKey *k)
{
	sift_bytes_put1(k, 0);
}

void
sift_dkey_i64(SiftDistinctKey *k, int64 v)
{
	sift_bytes_put1(k, 1);
	sift_bytes_put(k, &v, sizeof(v));
}

void
sift_dkey_f64(SiftDistinctKey *k, double v)
{
	int64		bits;

	if (v != v)
		bits = INT64_C(0x7FF8000000000000);
	else if (v == 0)
		bits = 0;
	else
		memcpy(&bits, &v, sizeof(bits));
	sift_dkey_i64(k, bits);
}

void
sift_dkey_bytes(SiftDistinctKey *k, const void *p, uint32 len)
{
	sift_bytes_put1(k, 2);
	sift_bytes_put(k, &len, sizeof(len));
	sift_bytes_put(k, p, len);
}

void
sift_dkey_uuid(SiftDistinctKey *k, const SiftUuid16 *u)
{
	sift_bytes_put1(k, 1);
	sift_bytes_put(k, u, sizeof(*u));
}

void
sift_dkey_group(SiftDistinctKey *k, const void *prefix, uint32 plen)
{
	sift_bytes_reset(k);
	sift_bytes_put(k, &plen, sizeof(plen));
	sift_bytes_put(k, prefix, plen);
}

void
sift_dkey_vec(SiftDistinctKey *k, const SiftVec *v, uint32 row, const char *slab)
{
	if (!sift_vec_valid_at(v, row))
	{
		sift_dkey_null(k);
		return;
	}
	switch (v->type)
	{
		case SVEC_I32:
			sift_dkey_i64(k, ((const int32 *) v->data)[row]);
			break;
		case SVEC_BOOL:
			sift_dkey_i64(k, ((const uint8 *) v->data)[row]);
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			sift_dkey_i64(k, ((const int64 *) v->data)[row]);
			break;
		case SVEC_F64:
			sift_dkey_f64(k, ((const double *) v->data)[row]);
			break;
		case SVEC_UUID:
			sift_dkey_uuid(k, &((const SiftUuid16 *) v->data)[row]);
			break;
		case SVEC_DICT32:
			{
				const SiftDictSrc *ds = v->dict;
				const uint32 c = ((const uint32 *) v->data)[row];

				sift_dkey_bytes(k, ds->bytes + ds->off[c], ds->off[c + 1] - ds->off[c]);
			}
			break;
		case SVEC_STRVIEW:
			{
				const SiftStrView *s = &((const SiftStrView *) v->data)[row];

				sift_dkey_bytes(k, s->len <= 12 ? (const void *) s->prefix : slab != NULL ? (const void *) (slab + s->u.offset) : (const void *) s->u.ptr, s->len);
			}
			break;
		default:
			SIFT_INVARIANT(false, "count distinct: a key from a vector of type %d", (int) v->type);
	}
}

void
sift_distinct_add_dkey(SiftDistinct *d, uint32 slot, const SiftDistinctKey *k)
{
	dist_add_hashed(d, slot, k->buf, k->len);
}

static inline uint32
dist_slot(const uint32 *gid, uint32 base, uint32 r)
{
	return base + (gid != NULL ? gid[r] : 0);
}

void
sift_distinct_add_i64(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v)
{
	const int64 *x = v->data;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		if (sift_vec_valid_at(v, r))
			sift_distinct_add_u64(d, dist_slot(gid, slot_base, r), (uint64) x[r]);
}

void
sift_distinct_add_i32(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v)
{
	const int32 *x = v->data;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		if (sift_vec_valid_at(v, r))
			sift_distinct_add_u64(d, dist_slot(gid, slot_base, r), (uint64) (int64) x[r]);
}

void
sift_distinct_add_uuid(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v)
{
	const SiftUuid16 *x = v->data;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		if (sift_vec_valid_at(v, r))
			sift_distinct_add(d, dist_slot(gid, slot_base, r), &x[r], sizeof(SiftUuid16));
}

void
sift_distinct_add_str(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v, const char *slab)
{
	const SiftStrView *x = v->data;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		const SiftStrView *s = &x[r];
		const void *bytes;

		if (!sift_vec_valid_at(v, r))
			continue;
		if (s->len <= 12)
			bytes = s->prefix;
		else
			bytes = slab != NULL ? slab + s->u.offset : (const void *) s->u.ptr;
		sift_distinct_add(d, dist_slot(gid, slot_base, r), bytes, s->len);
	}
}
