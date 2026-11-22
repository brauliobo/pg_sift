/*
 * agg/v6/v6_distinct_u64.c - the flat set of 64-bit values (agg/v6/v6_distinct_u64.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/v6/v6_distinct_u64.h"

#define DU64_FIRST_CAP	1024
#define DU64_AHEAD		16

struct SiftDistinctU64
{
	MemoryContext cx;
	uint64	   *key;
	uint8	   *tag;
	uint64		cap;
	uint64		count;
	uint64		budget;
};

static inline uint64
mix(uint64 x)
{
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	x ^= x >> 32;
	x *= UINT64CONST(0xd6e8feb86659fd93);
	return x ^ (x >> 32);
}

static inline uint8
tag_of(uint64 h)
{
	return (uint8) (0x80 | (h >> 57));
}

static void
table_alloc(SiftDistinctU64 *d, uint64 cap)
{
	d->cap = cap;
	d->key = MemoryContextAlloc(d->cx, cap * sizeof(uint64));
	d->tag = MemoryContextAllocZero(d->cx, cap);
}

SiftDistinctU64 *
sift_du64_create(MemoryContext cx, uint64 budget_bytes)
{
	SiftDistinctU64 *d = MemoryContextAllocZero(cx, sizeof(SiftDistinctU64));

	d->cx = cx;
	d->budget = budget_bytes;
	table_alloc(d, DU64_FIRST_CAP);
	return d;
}

static inline bool
insert(SiftDistinctU64 *d, uint64 v, uint64 h)
{
	const uint8 t = tag_of(h);
	uint64		i = h & (d->cap - 1);

	for (;; i = (i + 1) & (d->cap - 1))
	{
		if (d->tag[i] == 0)
		{
			d->tag[i] = t;
			d->key[i] = v;
			return true;
		}
		if (d->tag[i] == t && d->key[i] == v)
			return false;
	}
}

static void
grow(SiftDistinctU64 *d, uint64 cap)
{
	uint64	   *okey = d->key;
	uint8	   *otag = d->tag;
	const uint64 ocap = d->cap;

	table_alloc(d, cap);
	for (uint64 i = 0; i < ocap; i++)
		if (otag[i] != 0)
			insert(d, okey[i], mix(okey[i]));
	pfree(okey);
	pfree(otag);
}

bool
sift_du64_room(SiftDistinctU64 *d, uint32 n)
{
	const uint64 need = d->count + n;
	uint64		cap = d->cap;

	if (need * 10 <= cap * 7)
		return true;
	while (need * 10 > cap * 7)
		cap *= 2;
	if ((d->cap + cap) * (sizeof(uint64) + 1) > d->budget)
		return false;
	grow(d, cap);
	return true;
}

void
sift_du64_add(SiftDistinctU64 *d, const uint64 *vals, uint32 n)
{
	uint64		h[DU64_AHEAD];

	for (uint32 base = 0; base < n; base += DU64_AHEAD)
	{
		const uint32 m = Min(DU64_AHEAD, n - base);

		for (uint32 i = 0; i < m; i++)
		{
			h[i] = mix(vals[base + i]);
			__builtin_prefetch(&d->tag[h[i] & (d->cap - 1)]);
			__builtin_prefetch(&d->key[h[i] & (d->cap - 1)]);
		}
		for (uint32 i = 0; i < m; i++)
			d->count += insert(d, vals[base + i], h[i]);
	}
}

uint64
sift_du64_count(const SiftDistinctU64 *d)
{
	return d->count;
}

uint64
sift_du64_bytes(const SiftDistinctU64 *d)
{
	return d->cap * (sizeof(uint64) + 1);
}

void
sift_du64_each(const SiftDistinctU64 *d, void (*fn) (void *arg, uint64 v), void *arg)
{
	for (uint64 i = 0; i < d->cap; i++)
		if (d->tag[i] != 0)
			fn(arg, d->key[i]);
}

void
sift_du64_free(SiftDistinctU64 *d)
{
	pfree(d->key);
	pfree(d->tag);
	pfree(d);
}

/* ---- grouped ---- */

#define DU64G_FIRST_GROUPS	256

struct SiftDu64G
{
	MemoryContext cx;
	uint64		budget;
	uint32	   *gslot;			/* group id + 1 by the hash of the prefix, 0 empty */
	uint64		gslot_cap;
	uint64	   *ghash;
	uint32	   *goff;
	uint32	   *glen;
	uint64	   *gcount;
	uint32		ngroups;
	uint32		gcap;
	uint8	   *arena;
	uint64		arena_used;
	uint64		arena_cap;
	uint32		last;
	uint64	   *pv;
	uint32	   *pg;
	uint8	   *ptag;
	uint64		pcap;
	uint64		pcount;
};

static inline uint64
hash_bytes(const uint8 *p, uint32 n)
{
	uint64		h = UINT64CONST(0x9e3779b97f4a7c15) ^ n;
	uint64		w;

	for (; n >= 8; n -= 8, p += 8)
	{
		memcpy(&w, p, 8);
		h = mix(h ^ w);
	}
	w = 0;
	memcpy(&w, p, n);
	return mix(h ^ w);
}

static inline uint64
pair_hash(uint64 v, uint32 gid)
{
	return mix(v ^ ((uint64) gid + 1) * UINT64CONST(0x9e3779b97f4a7c15));
}

uint64
sift_du64g_bytes(const SiftDu64G *d)
{
	return d->gslot_cap * sizeof(uint32) + (uint64) d->gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) + d->arena_cap + d->pcap * (sizeof(uint64) + sizeof(uint32) + 1);
}

SiftDu64G *
sift_du64g_create(MemoryContext cx, uint64 budget_bytes)
{
	SiftDu64G  *d = MemoryContextAllocZero(cx, sizeof(SiftDu64G));

	d->cx = cx;
	d->budget = budget_bytes;
	d->gcap = DU64G_FIRST_GROUPS;
	d->gslot_cap = DU64G_FIRST_GROUPS * 2;
	d->gslot = MemoryContextAllocZero(cx, d->gslot_cap * sizeof(uint32));
	d->ghash = MemoryContextAlloc(cx, d->gcap * sizeof(uint64));
	d->gcount = MemoryContextAlloc(cx, d->gcap * sizeof(uint64));
	d->goff = MemoryContextAlloc(cx, d->gcap * sizeof(uint32));
	d->glen = MemoryContextAlloc(cx, d->gcap * sizeof(uint32));
	d->arena_cap = 4096;
	d->arena = MemoryContextAlloc(cx, d->arena_cap);
	d->pcap = DU64_FIRST_CAP;
	d->pv = MemoryContextAlloc(cx, d->pcap * sizeof(uint64));
	d->pg = MemoryContextAlloc(cx, d->pcap * sizeof(uint32));
	d->ptag = MemoryContextAllocZero(cx, d->pcap);
	d->last = UINT32_MAX;
	return d;
}

static void
groups_rehash(SiftDu64G *d)
{
	pfree(d->gslot);
	d->gslot = MemoryContextAllocZero(d->cx, d->gslot_cap * sizeof(uint32));
	for (uint32 g = 0; g < d->ngroups; g++)
	{
		uint64		i = d->ghash[g] & (d->gslot_cap - 1);

		while (d->gslot[i] != 0)
			i = (i + 1) & (d->gslot_cap - 1);
		d->gslot[i] = g + 1;
	}
}

/* room for one more group of plen bytes; false when that passes the budget */
static bool
groups_room(SiftDu64G *d, uint32 plen)
{
	if (d->ngroups == d->gcap || (d->ngroups + 1) * 10 > d->gslot_cap * 7 || d->arena_used + plen > d->arena_cap)
	{
		const uint32 gcap = d->ngroups == d->gcap ? d->gcap * 2 : d->gcap;
		const uint64 acap = d->arena_used + plen > d->arena_cap ? Max(d->arena_cap * 2, d->arena_used + plen) : d->arena_cap;
		const uint64 scap = (d->ngroups + 1) * 10 > d->gslot_cap * 7 ? d->gslot_cap * 2 : d->gslot_cap;
		const uint64 now = sift_du64g_bytes(d);
		const uint64 grown = now - d->gslot_cap * sizeof(uint32) - (uint64) d->gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) - d->arena_cap +
			scap * sizeof(uint32) + (uint64) gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) + acap;

		if (grown > d->budget)
			return false;
		if (gcap != d->gcap)
		{
			d->ghash = repalloc(d->ghash, gcap * sizeof(uint64));
			d->gcount = repalloc(d->gcount, gcap * sizeof(uint64));
			d->goff = repalloc(d->goff, gcap * sizeof(uint32));
			d->glen = repalloc(d->glen, gcap * sizeof(uint32));
			d->gcap = gcap;
		}
		if (acap != d->arena_cap)
		{
			d->arena = repalloc(d->arena, acap);
			d->arena_cap = acap;
		}
		if (scap != d->gslot_cap)
		{
			d->gslot_cap = scap;
			groups_rehash(d);
		}
	}
	return true;
}

static bool
group_of(SiftDu64G *d, const uint8 *prefix, uint32 plen, uint32 *gid)
{
	const uint64 h = hash_bytes(prefix, plen);
	uint64		i;

	if (d->last != UINT32_MAX && d->glen[d->last] == plen && d->ghash[d->last] == h && memcmp(d->arena + d->goff[d->last], prefix, plen) == 0)
		return (*gid = d->last, true);
	for (i = h & (d->gslot_cap - 1); d->gslot[i] != 0; i = (i + 1) & (d->gslot_cap - 1))
	{
		const uint32 g = d->gslot[i] - 1;

		if (d->ghash[g] == h && d->glen[g] == plen && memcmp(d->arena + d->goff[g], prefix, plen) == 0)
			return (*gid = d->last = g, true);
	}
	if (!groups_room(d, plen))
		return false;
	for (i = h & (d->gslot_cap - 1); d->gslot[i] != 0; i = (i + 1) & (d->gslot_cap - 1))
		;
	*gid = d->last = d->ngroups++;
	d->gslot[i] = *gid + 1;
	d->ghash[*gid] = h;
	d->gcount[*gid] = 0;
	d->goff[*gid] = (uint32) d->arena_used;
	d->glen[*gid] = plen;
	memcpy(d->arena + d->arena_used, prefix, plen);
	d->arena_used += plen;
	return true;
}

static bool
pairs_room(SiftDu64G *d)
{
	uint64		cap = d->pcap;

	if ((d->pcount + 1) * 10 <= cap * 7)
		return true;
	cap *= 2;
	if (sift_du64g_bytes(d) + cap * (sizeof(uint64) + sizeof(uint32) + 1) > d->budget)
		return false;
	{
		uint64	   *ov = d->pv;
		uint32	   *og = d->pg;
		uint8	   *ot = d->ptag;
		const uint64 ocap = d->pcap;

		d->pcap = cap;
		d->pv = MemoryContextAlloc(d->cx, cap * sizeof(uint64));
		d->pg = MemoryContextAlloc(d->cx, cap * sizeof(uint32));
		d->ptag = MemoryContextAllocZero(d->cx, cap);
		for (uint64 i = 0; i < ocap; i++)
			if (ot[i] != 0)
			{
				const uint64 h = pair_hash(ov[i], og[i]);
				uint64		j = h & (cap - 1);

				while (d->ptag[j] != 0)
					j = (j + 1) & (cap - 1);
				d->ptag[j] = ot[i];
				d->pv[j] = ov[i];
				d->pg[j] = og[i];
			}
		pfree(ov);
		pfree(og);
		pfree(ot);
	}
	return true;
}

bool
sift_du64g_add(SiftDu64G *d, const uint8 *prefix, uint32 plen, uint64 value)
{
	uint32		gid;
	uint64		h,
				i;
	uint8		t;

	if (!group_of(d, prefix, plen, &gid))
		return false;
	h = pair_hash(value, gid);
	t = tag_of(h);
	for (i = h & (d->pcap - 1); d->ptag[i] != 0; i = (i + 1) & (d->pcap - 1))
		if (d->ptag[i] == t && d->pv[i] == value && d->pg[i] == gid)
			return true;
	if (!pairs_room(d))
		return false;
	for (i = h & (d->pcap - 1); d->ptag[i] != 0; i = (i + 1) & (d->pcap - 1))
		;
	d->ptag[i] = t;
	d->pv[i] = value;
	d->pg[i] = gid;
	d->pcount++;
	d->gcount[gid]++;
	return true;
}

void
sift_du64g_groups(const SiftDu64G *d, SiftDu64GGroup fn, void *arg)
{
	for (uint32 g = 0; g < d->ngroups; g++)
		if (d->gcount[g] != 0)
			fn(arg, d->arena + d->goff[g], d->glen[g], d->gcount[g]);
}

void
sift_du64g_pairs(const SiftDu64G *d, SiftDu64GPair fn, void *arg)
{
	for (uint64 i = 0; i < d->pcap; i++)
		if (d->ptag[i] != 0)
			fn(arg, d->arena + d->goff[d->pg[i]], d->glen[d->pg[i]], d->pv[i]);
}

void
sift_du64g_free(SiftDu64G *d)
{
	pfree(d->gslot);
	pfree(d->ghash);
	pfree(d->gcount);
	pfree(d->goff);
	pfree(d->glen);
	pfree(d->arena);
	pfree(d->pv);
	pfree(d->pg);
	pfree(d->ptag);
	pfree(d);
}
