/*
 * agg/group/tier.c - the tiers of agg/group/tier.h.
 */
#include "core/host.h"

#include "agg/group/tier.h"
#include "core/memtrack.h"
#include "core/popcount.h"

#define DIRECT_ACC_BYTES	(4 * sizeof(int64))
#define DIRECT_MIN_CAP		1024

struct SiftTierGroups
{
	SiftGroupTier tier;
	uint32		visited;
	uint64		budget;
	int64		base;
	uint32		span;
	uint32		cap;			/* direct: the keys [base, base + cap) the arrays hold: they grow with the highest key, up to span */
	int64	   *acc;			/* direct: cap * 4 */
	uint64	   *seen;			/* direct: cap bits */
	SiftSaltedHT ht;
	SiftRadixShape shape;
	SiftGroupSpill *spill;
	MemoryContext cxt;
};

static void
shape_init(const SiftRadixShape *sh, SiftAggSlot64 *s, uint8 *ext)
{
	sift_slot_cmsm_init(s);
}

static void
shape_merge(const SiftRadixShape *sh, SiftAggSlot64 *dst, uint8 *dst_ext, const SiftAggSlot64 *src, const uint8 *src_ext)
{
	sift_slot_cmsm_merge(dst, src);
}

static bool
over_soft(uint64 bytes, uint64 budget)
{
	return bytes * 100 > budget * SIFT_SPILL_SOFT_PERCENT;
}

static void
enter(SiftTierGroups *g, SiftGroupTier tier)
{
	g->tier = tier;
	g->visited |= UINT32_C(1) << tier;
}

static void
table_new(SiftTierGroups *g, uint32 min_slots)
{
	MemoryContext old = MemoryContextSwitchTo(g->cxt);

	sift_ht_init(&g->ht, min_slots);
	MemoryContextSwitchTo(old);
}

/* The arrays for keys [base, base + cap), the ones of cap_old kept; false when they would be past their share of the memory */
static bool
direct_resize(SiftTierGroups *g, uint32 cap)
{
	MemoryContext old;
	int64	   *acc;
	uint64	   *seen;

	if (over_soft((uint64) cap * DIRECT_ACC_BYTES, g->budget))
		return false;
	old = MemoryContextSwitchTo(g->cxt);
	acc = palloc0((Size) cap * DIRECT_ACC_BYTES);
	seen = palloc0(sizeof(uint64) * ((cap + 63) / 64));
	MemoryContextSwitchTo(old);
	if (g->acc)
	{
		memcpy(acc, g->acc, (Size) g->cap * DIRECT_ACC_BYTES);
		memcpy(seen, g->seen, sizeof(uint64) * ((g->cap + 63) / 64));
		pfree(g->acc);
		pfree(g->seen);
	}
	g->acc = acc;
	g->seen = seen;
	g->cap = cap;
	return true;
}

SiftTierGroups *
sift_tier_new(SiftGroupTier start, int64 base, uint32 span, uint64 budget)
{
	MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "group tiers");
	SiftTierGroups *g = MemoryContextAllocZero(cxt, sizeof(SiftTierGroups));

	g->cxt = cxt;
	g->budget = budget;
	g->base = base;
	g->span = span;
	g->shape = (SiftRadixShape) {.wide_key = false,.ext_dist_off = SIFT_HT_NO_EXT,.init = shape_init,.merge = shape_merge};
	if ((start == SIFT_GRP_DICT_DIRECT || start == SIFT_GRP_DIRECT_INT) && span > 0 && span <= SIFT_TIER_DIRECT_MAX &&
		direct_resize(g, Min(span, DIRECT_MIN_CAP)))
		enter(g, start);
	else
	{
		enter(g, SIFT_GRP_SALTED_HT);
		table_new(g, SIFT_HT_MIN_SLOTS);
	}
	return g;
}

static SiftAggSlot64 *
upsert_key(SiftTierGroups *g, int64 key)
{
	SiftInlineKey16 k = {.i64 = {key, 0}};
	bool		found;
	SiftAggSlot64 *s = sift_ht_upsert(&g->ht, sift_aggk6_hash1((uint64) key), &k, &found);

	if (!found)
		sift_slot_cmsm_init(s);
	return s;
}

/* A key outside the span, or an array past its share of the memory: every group of the array into the table, which the rest are added to */
static void
promote_to_table(SiftTierGroups *g)
{
	const uint32 words = (g->cap + 63) >> 6;
	uint64		live = 0;

	for (uint32 w = 0; w < words; w++)
		live += sift_pc64(g->seen[w]);
	table_new(g, (uint32) Min((live * SIFT_HT_MAX_LOAD_DEN) / SIFT_HT_MAX_LOAD_NUM + 1, (uint64) UINT32_MAX / 2));
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = g->seen[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 i = (w << 6) + (uint32) __builtin_ctzll(bits);
			SiftAggSlot64 *s = upsert_key(g, g->base + (int64) i);

			memcpy(s->agg.i64, &g->acc[(Size) i * 4], DIRECT_ACC_BYTES);
		}
	pfree(g->acc);
	pfree(g->seen);
	g->acc = NULL;
	g->seen = NULL;
	enter(g, SIFT_GRP_SALTED_HT);
}

/* The table is full: the groups go to the files and the table is a small one, which is emptied each time it is full from then on */
static void
flush_table(SiftTierGroups *g)
{
	sift_gspill_put_ht(g->spill, &g->ht);
	sift_ht_clear(&g->ht);
}

static void
spill_table(SiftTierGroups *g)
{
	MemoryContext old = MemoryContextSwitchTo(g->cxt);

	g->spill = sift_gspill_new(&g->shape);
	sift_gspill_put_ht(g->spill, &g->ht);
	sift_ht_destroy(&g->ht);
	sift_ht_init(&g->ht, SIFT_SPILL_LOCAL_CAP);
	MemoryContextSwitchTo(old);
	enter(g, SIFT_GRP_RADIX_SPILL);
}

void
sift_tier_add(SiftTierGroups *g, int64 key, int64 val)
{
	SiftAggSlot64 *s;

	if (g->tier == SIFT_GRP_DICT_DIRECT || g->tier == SIFT_GRP_DIRECT_INT)
	{
		const uint64 i = (uint64) key - (uint64) g->base;

		if (i < g->span && (i < g->cap || direct_resize(g, Min(g->span, Max((uint32) i + 1, g->cap * 2)))))
		{
			int64	   *a = &g->acc[(Size) i * 4];

			if (!((g->seen[i >> 6] >> (i & 63)) & 1))
			{
				g->seen[i >> 6] |= UINT64CONST(1) << (i & 63);
				a[0] = 0;
				a[1] = 0;
				a[2] = INT64_MAX;
				a[3] = INT64_MIN;
			}
			a[0]++;
			a[1] = (int64) ((uint64) a[1] + (uint64) val);
			a[2] = Min(a[2], val);
			a[3] = Max(a[3], val);
			return;
		}
		promote_to_table(g);
	}
	if (g->tier == SIFT_GRP_SALTED_HT && g->ht.count >= g->ht.grow_at && over_soft(sift_ht_bytes(&g->ht) * 2, g->budget))
		spill_table(g);
	if (g->tier == SIFT_GRP_RADIX_SPILL && g->ht.count >= g->ht.grow_at)
		flush_table(g);
	s = upsert_key(g, key);
	sift_slot_cmsm_add(s, val);
}

typedef struct EmitArg
{
	SiftTierEmit emit;
	void	   *arg;
} EmitArg;

static void
emit_table(void *arg, SiftSaltedHT *ht)
{
	const EmitArg *e = arg;
	SiftAggSlot64 *s;

	SIFT_HT_FOREACH(ht, s)
		e->emit(e->arg, s->key.i64[0], s->agg.i64);
}

void
sift_tier_emit(SiftTierGroups *g, SiftTierEmit emit, void *arg)
{
	EmitArg		e = {emit, arg};

	if (g->tier == SIFT_GRP_RADIX_SPILL)
	{
		flush_table(g);
		sift_gspill_scan(g->spill, Max(g->budget / 2, 1), emit_table, &e);
	}
	else if (g->tier == SIFT_GRP_SALTED_HT)
		emit_table(&e, &g->ht);
	else
		for (uint32 i = 0; i < g->cap; i++)
			if ((g->seen[i >> 6] >> (i & 63)) & 1)
				emit(arg, g->base + (int64) i, &g->acc[(Size) i * 4]);
}

SiftGroupTier
sift_tier_of(const SiftTierGroups *g)
{
	return g->tier;
}

uint32
sift_tier_visited(const SiftTierGroups *g)
{
	return g->visited;
}

uint64
sift_tier_spilled_bytes(const SiftTierGroups *g)
{
	return g->spill ? sift_gspill_bytes(g->spill) : 0;
}

uint32
sift_tier_spill_depth(const SiftTierGroups *g)
{
	return g->spill ? sift_gspill_depth(g->spill) : 0;
}

void
sift_tier_free(SiftTierGroups *g)
{
	if (g->spill)
		sift_gspill_free(g->spill);
	MemoryContextDelete(g->cxt);
}
