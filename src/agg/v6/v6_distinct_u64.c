/*
 * agg/v6/v6_distinct_u64.c - the flat set of 64-bit values (agg/v6/v6_distinct_u64.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/v6/v6_distinct_u64.h"

/* MEM[MEM-AGG-09]: SiftDistinctU64 and SiftDu64G first tables (DU64_FIRST_CAP=64 values = 576B, 512B arena; grow checks live + new against the budget; was 1024 values = 9KB and 25KB for the grouped set) */
#define DU64_FIRST_CAP	64
#define DU64_AHEAD		16
#define DU64_TILE		64

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

static inline void
du64_prefetch(const SiftDistinctU64 *d, uint64 h)
{
	__builtin_prefetch(&d->tag[h & (d->cap - 1)]);
	__builtin_prefetch(&d->key[h & (d->cap - 1)]);
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

/* 1 when v is new (and put in), 0 when it is there, -1 when it is new and the table is at its load limit */
static inline int
probe(SiftDistinctU64 *d, uint64 v, uint64 h)
{
	const uint8 t = tag_of(h);
	uint64		i = h & (d->cap - 1);

	for (;; i = (i + 1) & (d->cap - 1))
	{
		if (d->tag[i] == 0)
		{
			if ((d->count + 1) * 10 > d->cap * 7)
				return -1;
			d->tag[i] = t;
			d->key[i] = v;
			return 1;
		}
		if (d->tag[i] == t && d->key[i] == v)
			return 0;
	}
}

/* the table of twice the capacity, when the budget holds both while the values move: false when it does not */
static bool
grow(SiftDistinctU64 *d)
{
	uint64	   *okey = d->key;
	uint8	   *otag = d->tag;
	const uint64 ocap = d->cap;

	if (d->cap * 3 * (sizeof(uint64) + 1) > d->budget)
		return false;
	table_alloc(d, ocap * 2);
	for (uint64 i = 0; i < ocap; i++)
		if (otag[i] != 0)
			probe(d, okey[i], mix(okey[i]));
	pfree(okey);
	pfree(otag);
	return true;
}

uint32
sift_du64_add(SiftDistinctU64 *d, const uint64 *vals, uint32 n)
{
	uint64		h[DU64_TILE];

	for (uint32 base = 0; base < n; base += DU64_TILE)
	{
		const uint32 m = Min(DU64_TILE, n - base);

		for (uint32 i = 0; i < m; i++)
			h[i] = mix(vals[base + i]);
		for (uint32 i = 0; i < Min(m, DU64_AHEAD); i++)
			du64_prefetch(d, h[i]);
		for (uint32 i = 0; i < m; i++)
		{
			int			got;

			if (i + DU64_AHEAD < m)
				du64_prefetch(d, h[i + DU64_AHEAD]);
			while ((got = probe(d, vals[base + i], h[i])) < 0)
				if (!grow(d))
					return base + i;
			d->count += got;
		}
	}
	return n;
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

#define DU64G_FIRST_GROUPS	64

struct SiftDu64G
{
	MemoryContext cx;
	uint64		budget;
	uint32	   *gslot;			/* group id + 1 by the hash of the prefix, 0 empty */
	uint8	   *gtag;			/* the tag of the hash of the group of each gslot: a probe step reads the 64 bit hash of a group only when this agrees */
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
	return d->gslot_cap * (sizeof(uint32) + 1) + (uint64) d->gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) + d->arena_cap + d->pcap * (sizeof(uint64) + sizeof(uint32) + 1);
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
	d->gtag = MemoryContextAllocZero(cx, d->gslot_cap);
	d->ghash = MemoryContextAlloc(cx, d->gcap * sizeof(uint64));
	d->gcount = MemoryContextAlloc(cx, d->gcap * sizeof(uint64));
	d->goff = MemoryContextAlloc(cx, d->gcap * sizeof(uint32));
	d->glen = MemoryContextAlloc(cx, d->gcap * sizeof(uint32));
	d->arena_cap = 512;
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
	pfree(d->gtag);
	d->gslot = MemoryContextAllocZero(d->cx, d->gslot_cap * sizeof(uint32));
	d->gtag = MemoryContextAllocZero(d->cx, d->gslot_cap);
	for (uint32 g = 0; g < d->ngroups; g++)
	{
		uint64		i = d->ghash[g] & (d->gslot_cap - 1);

		while (d->gslot[i] != 0)
			i = (i + 1) & (d->gslot_cap - 1);
		d->gslot[i] = g + 1;
		d->gtag[i] = tag_of(d->ghash[g]);
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
		const uint64 grown = now - d->gslot_cap * (sizeof(uint32) + 1) - (uint64) d->gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) - d->arena_cap +
			scap * (sizeof(uint32) + 1) + (uint64) gcap * (sizeof(uint64) * 2 + sizeof(uint32) * 2) + acap;

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

/* The group of the prefix (hash h), made when it is new: one probe finds it or the empty slot it goes in, which is looked for again only when making room moved the slots */
static bool
group_of(SiftDu64G *d, const uint8 *prefix, uint32 plen, uint64 h, uint32 *gid)
{
	const uint8 t = tag_of(h);
	uint64		i;
	uint64		cap = d->gslot_cap;

	if (d->last != UINT32_MAX && d->glen[d->last] == plen && d->ghash[d->last] == h && memcmp(d->arena + d->goff[d->last], prefix, plen) == 0)
		return (*gid = d->last, true);
	for (i = h & (cap - 1); d->gslot[i] != 0; i = (i + 1) & (cap - 1))
	{
		const uint32 g = d->gslot[i] - 1;

		if (d->gtag[i] == t && d->ghash[g] == h && d->glen[g] == plen && memcmp(d->arena + d->goff[g], prefix, plen) == 0)
			return (*gid = d->last = g, true);
	}
	if (!groups_room(d, plen))
		return false;
	if (d->gslot_cap != cap)
		for (i = h & (d->gslot_cap - 1); d->gslot[i] != 0; i = (i + 1) & (d->gslot_cap - 1))
			;
	*gid = d->last = d->ngroups++;
	d->gslot[i] = *gid + 1;
	d->gtag[i] = t;
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

/* The pair (group gid, value) of hash h: true when it is in the set (it is new or it was there), false when it is new and does not fit the budget */
static bool
pair_put(SiftDu64G *d, uint32 gid, uint64 value, uint64 h)
{
	const uint8 t = tag_of(h);
	const uint64 cap = d->pcap;
	uint64		i;

	for (i = h & (cap - 1); d->ptag[i] != 0; i = (i + 1) & (cap - 1))
		if (d->ptag[i] == t && d->pv[i] == value && d->pg[i] == gid)
			return true;
	if (!pairs_room(d))
		return false;
	if (d->pcap != cap)
		for (i = h & (d->pcap - 1); d->ptag[i] != 0; i = (i + 1) & (d->pcap - 1))
			;
	d->ptag[i] = t;
	d->pv[i] = value;
	d->pg[i] = gid;
	d->pcount++;
	d->gcount[gid]++;
	return true;
}

bool
sift_du64g_add(SiftDu64G *d, const uint8 *prefix, uint32 plen, uint64 value)
{
	uint32		gid;

	if (!group_of(d, prefix, plen, hash_bytes(prefix, plen), &gid))
		return false;
	return pair_put(d, gid, value, pair_hash(value, gid));
}

static inline void
du64g_prefetch_group(const SiftDu64G *d, uint64 h)
{
	__builtin_prefetch(&d->gtag[h & (d->gslot_cap - 1)]);
	__builtin_prefetch(&d->gslot[h & (d->gslot_cap - 1)]);
}

static inline void
du64g_prefetch_pair(const SiftDu64G *d, uint64 h)
{
	__builtin_prefetch(&d->ptag[h & (d->pcap - 1)]);
	__builtin_prefetch(&d->pv[h & (d->pcap - 1)]);
}

uint32
sift_du64g_add_batch(SiftDu64G *d, const uint8 *prefixes, const uint32 *off, const uint32 *plen, const uint64 *vals, uint32 n)
{
	uint64		gh[DU64_TILE],
				ph[DU64_TILE];
	uint32		gid[DU64_TILE];

	for (uint32 base = 0; base < n; base += DU64_TILE)
	{
		const uint32 m = Min(DU64_TILE, n - base);
		uint32		ok = m;

		for (uint32 i = 0; i < m; i++)
			gh[i] = hash_bytes(prefixes + off[base + i], plen[base + i]);
		for (uint32 i = 0; i < Min(m, DU64_AHEAD); i++)
			du64g_prefetch_group(d, gh[i]);
		for (uint32 i = 0; i < m && ok == m; i++)
		{
			if (i + DU64_AHEAD < m)
				du64g_prefetch_group(d, gh[i + DU64_AHEAD]);
			if (!group_of(d, prefixes + off[base + i], plen[base + i], gh[i], &gid[i]))
				ok = i;
			else
				ph[i] = pair_hash(vals[base + i], gid[i]);
		}
		for (uint32 i = 0; i < Min(ok, DU64_AHEAD); i++)
			du64g_prefetch_pair(d, ph[i]);
		for (uint32 i = 0; i < ok; i++)
		{
			if (i + DU64_AHEAD < ok)
				du64g_prefetch_pair(d, ph[i + DU64_AHEAD]);
			if (!pair_put(d, gid[i], vals[base + i], ph[i]))
				return base + i;
		}
		if (ok != m)
			return base + ok;
	}
	return n;
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
	pfree(d->gtag);
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
