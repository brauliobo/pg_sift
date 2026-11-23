/*
 * agg/v6/v6_distinct_u64.c - the flat set of 64-bit values (agg/v6/v6_distinct_u64.h).
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/v6/v6_distinct_u64.h"

/* MEM[MEM-AGG-09]: SiftDistinctU64 and SiftDu64G first tables (DU64_FIRST_CAP=64 values = 576B; grow checks live + new against the budget; was 1024 values = 9KB and 25KB for the grouped set) */
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

#define DU64G_TILE		256
#define DU64G_AHEAD		32
#define DU64G_FIRST_GIDS	64

/*
 * One cache line holds four: a probe reads the value and the group of an entry together, and the group number 0 is an empty entry (a group g is stored as g + 1). In a bucket of the radix
 * mode h32 is the low half of the hash of the pair, so that the bucket is deduplicated with no hash made again.
 */
typedef struct DuPair
{
	uint64		val;
	uint32		gid1;
	uint32		h32;
} DuPair;

/*
 * A table that outgrows DU64G_RADIX_CAP entries is past the cache, and each pair it takes is a miss (twice again while it grows). It becomes DU64G_BUCKETS buckets of pairs, chosen by the top
 * bits of the hash: a pair is appended to its bucket, and a bucket is deduplicated in a table of its own that fits the cache, when the pairs that came since the last time are as many as the
 * distinct ones then (the work is linear in the pairs), and at the end. A bucket then holds each pair it has seen once, and a group's count is the number of its pairs in the buckets.
 */
#define DU64G_BUCKET_BITS	6
#define DU64G_BUCKETS		(1 << DU64G_BUCKET_BITS)
#define DU64G_RADIX_CAP		(UINT64CONST(1) << 17)
#define DU64G_DEDUP_SHIFT	3		/* the pairs that make a first deduplication worth it: this many table fulls */
#define DU64G_BUCKET_FIRST	256

typedef struct DuBucket
{
	DuPair	   *p;
	uint32		n;
	uint32		cap;
} DuBucket;

struct SiftDu64G
{
	MemoryContext cx;
	uint64		budget;
	DuPair	   *tab;
	uint64		cap;
	uint64		count;
	uint64	   *gcount;
	uint32		gcap;
	uint64		radix_cap;		/* the entries at which the table becomes buckets */
	DuBucket   *bk;				/* not NULL: the buckets, and tab and gcount are gone */
	uint64		kept;			/* the pairs the buckets held when they were last deduplicated */
	uint64		fresh;			/* the pairs appended since */
	uint64		bk_bytes;		/* the bytes the buckets hold */
	uint32		max_gid;
	DuPair	   *scratch;		/* the table a bucket is deduplicated in, and its capacity */
	uint64		scratch_cap;
};

static inline uint64
pair_hash(uint64 v, uint32 gid)
{
	return mix(v ^ ((uint64) gid + 1) * UINT64CONST(0x9e3779b97f4a7c15));
}

uint64
sift_du64g_bytes(const SiftDu64G *d)
{
	const uint64 scratch = d->scratch_cap * sizeof(DuPair);

	if (d->bk != NULL)
		return scratch + d->bk_bytes + ((uint64) d->max_gid + 1) * sizeof(uint64);
	return d->cap * sizeof(DuPair) + (uint64) d->gcap * sizeof(uint64);
}

SiftDu64G *
sift_du64g_create_ex(MemoryContext cx, uint64 budget_bytes, uint64 radix_cap)
{
	SiftDu64G  *d = MemoryContextAllocZero(cx, sizeof(SiftDu64G));

	d->cx = cx;
	d->budget = budget_bytes;
	d->radix_cap = radix_cap;
	d->cap = DU64_FIRST_CAP;
	d->tab = MemoryContextAllocZero(cx, d->cap * sizeof(DuPair));
	d->gcap = DU64G_FIRST_GIDS;
	d->gcount = MemoryContextAllocZero(cx, (uint64) d->gcap * sizeof(uint64));
	return d;
}

SiftDu64G *
sift_du64g_create(MemoryContext cx, uint64 budget_bytes)
{
	return sift_du64g_create_ex(cx, budget_bytes, DU64G_RADIX_CAP);
}

/* room for the counter of group gid: false when that passes the budget */
static bool
gid_room(SiftDu64G *d, uint32 gid)
{
	uint32		gcap;

	if (gid < d->gcap)
		return true;
	gcap = Max(d->gcap * 2, pg_nextpower2_32(gid + 1));
	if (sift_du64g_bytes(d) + (uint64) (gcap - d->gcap) * sizeof(uint64) > d->budget)
		return false;
	d->gcount = repalloc(d->gcount, (uint64) gcap * sizeof(uint64));
	memset(d->gcount + d->gcap, 0, (uint64) (gcap - d->gcap) * sizeof(uint64));
	d->gcap = gcap;
	return true;
}

static inline uint64
empty_slot(const SiftDu64G *d, uint64 h)
{
	uint64		i = h & (d->cap - 1);

	while (d->tab[i].gid1 != 0)
		i = (i + 1) & (d->cap - 1);
	return i;
}

/* the table of twice the capacity, the old and the new one held while the pairs move: false when the budget does not hold both */
static bool
pairs_grow(SiftDu64G *d)
{
	DuPair	   *old = d->tab;
	const uint64 ocap = d->cap;

	if (sift_du64g_bytes(d) + ocap * 2 * sizeof(DuPair) > d->budget)
		return false;
	d->cap = ocap * 2;
	d->tab = MemoryContextAllocZero(d->cx, d->cap * sizeof(DuPair));
	for (uint64 i = 0; i < ocap; i++)
	{
		if (i + DU64_AHEAD < ocap && old[i + DU64_AHEAD].gid1 != 0)
			__builtin_prefetch(&d->tab[pair_hash(old[i + DU64_AHEAD].val, old[i + DU64_AHEAD].gid1 - 1) & (d->cap - 1)], 1);
		if (old[i].gid1 != 0)
			d->tab[empty_slot(d, pair_hash(old[i].val, old[i].gid1 - 1))] = old[i];
	}
	pfree(old);
	return true;
}

/* ---- the buckets ---- */

static inline DuBucket *
bucket_of(const SiftDu64G *d, uint64 h)
{
	return &d->bk[h >> (64 - DU64G_BUCKET_BITS)];
}

/* the pair at the end of its bucket, which grows with no look at the budget: the caller has seen to it */
static inline void
bucket_push(SiftDu64G *d, uint32 gid, uint64 value, uint64 h)
{
	DuBucket   *b = bucket_of(d, h);

	if (b->n == b->cap)
	{
		const uint32 cap = Max(DU64G_BUCKET_FIRST, b->cap * 2);

		b->p = b->p == NULL ? MemoryContextAlloc(d->cx, (uint64) cap * sizeof(DuPair)) : repalloc(b->p, (uint64) cap * sizeof(DuPair));
		d->bk_bytes += (uint64) (cap - b->cap) * sizeof(DuPair);
		b->cap = cap;
	}
	b->p[b->n++] = (DuPair) {.val = value,.gid1 = gid + 1,.h32 = (uint32) h};
	d->max_gid = Max(d->max_gid, gid);
}

/* the bucket without a pair twice, in a table that holds its pairs twice over: that is the size of a cache when the buckets are many */
static void
bucket_dedup(SiftDu64G *d, DuBucket *b)
{
	const uint64 cap = pg_nextpower2_64(Max(64, (uint64) b->n * 2));
	uint32		out = 0;

	if (cap > d->scratch_cap)
	{
		if (d->scratch != NULL)
			pfree(d->scratch);
		d->scratch = MemoryContextAlloc(d->cx, cap * sizeof(DuPair));
		d->scratch_cap = cap;
	}
	memset(d->scratch, 0, cap * sizeof(DuPair));
	for (uint32 i = 0; i < b->n; i++)
	{
		const DuPair e = b->p[i];

		for (uint64 j = e.h32 & (cap - 1);; j = (j + 1) & (cap - 1))
		{
			if (d->scratch[j].gid1 == 0)
			{
				d->scratch[j] = e;
				b->p[out++] = e;
				break;
			}
			if (d->scratch[j].val == e.val && d->scratch[j].gid1 == e.gid1)
				break;
		}
	}
	b->n = out;
}

static void
buckets_dedup(SiftDu64G *d)
{
	uint64		kept = 0;

	for (int i = 0; i < DU64G_BUCKETS; i++)
	{
		bucket_dedup(d, &d->bk[i]);
		kept += d->bk[i].n;
	}
	d->kept = kept;
	d->fresh = 0;
}

/* a pair into its bucket: false, with nothing added, when the bucket must grow and the budget does not hold it (once the buckets have been deduplicated, which may leave room) */
static bool
bucket_put(SiftDu64G *d, uint32 gid, uint64 value, uint64 h)
{
	DuBucket   *b = bucket_of(d, h);

	if (b->n == b->cap)
	{
		const uint64 more = (uint64) (Max(DU64G_BUCKET_FIRST, b->cap * 2) - b->cap) * sizeof(DuPair);

		if (sift_du64g_bytes(d) + more > d->budget)
		{
			buckets_dedup(d);
			if (b->n == b->cap)
				return false;
		}
	}
	bucket_push(d, gid, value, h);
	if (++d->fresh >= Max(d->radix_cap << DU64G_DEDUP_SHIFT, d->kept))
		buckets_dedup(d);
	return true;
}

/* the pairs of the table into buckets, and the table and the counters gone: false when the budget does not hold the table and its pairs twice over, with nothing changed */
static bool
to_buckets(SiftDu64G *d)
{
	DuPair	   *old = d->tab;

	if (sift_du64g_bytes(d) + d->count * 2 * sizeof(DuPair) > d->budget)
		return false;
	d->bk = MemoryContextAllocZero(d->cx, DU64G_BUCKETS * sizeof(DuBucket));
	for (uint64 i = 0; i < d->cap; i++)
		if (old[i].gid1 != 0)
			bucket_push(d, old[i].gid1 - 1, old[i].val, pair_hash(old[i].val, old[i].gid1 - 1));
	d->kept = d->count;
	pfree(old);
	pfree(d->gcount);
	d->tab = NULL;
	d->gcount = NULL;
	d->cap = d->gcap = 0;
	return true;
}

/* The pair (gid, value) of hash h: true when it is in the set (it is new or it was there), false when it is new and does not fit the budget */
static bool
pair_put(SiftDu64G *d, uint32 gid, uint64 value, uint64 h)
{
	uint64		i;

	for (i = h & (d->cap - 1); d->tab[i].gid1 != 0; i = (i + 1) & (d->cap - 1))
		if (d->tab[i].val == value && d->tab[i].gid1 == gid + 1)
			return true;
	if (!gid_room(d, gid))
		return false;
	if ((d->count + 1) * 10 > d->cap * 7)
	{
		if (d->cap >= d->radix_cap)
			return to_buckets(d) && bucket_put(d, gid, value, h);
		if (!pairs_grow(d))
			return false;
		i = empty_slot(d, h);
	}
	d->tab[i] = (DuPair) {.val = value,.gid1 = gid + 1};
	d->count++;
	d->gcount[gid]++;
	return true;
}

static inline bool
put_one(SiftDu64G *d, uint32 gid, uint64 value, uint64 h)
{
	return d->bk != NULL ? bucket_put(d, gid, value, h) : pair_put(d, gid, value, h);
}

bool
sift_du64g_add(SiftDu64G *d, uint32 gid, uint64 value)
{
	return put_one(d, gid, value, pair_hash(value, gid));
}

uint32
sift_du64g_add_batch(SiftDu64G *d, const uint32 *gids, const uint64 *vals, uint32 n)
{
	uint64		h[DU64G_TILE];

	for (uint32 base = 0; base < n; base += DU64G_TILE)
	{
		const uint32 m = Min(DU64G_TILE, n - base);

		for (uint32 i = 0; i < m; i++)
			h[i] = pair_hash(vals[base + i], gids[base + i]);
		for (uint32 i = 0; d->bk == NULL && i < Min(m, DU64G_AHEAD); i++)
			__builtin_prefetch(&d->tab[h[i] & (d->cap - 1)], 1);
		for (uint32 i = 0; i < m; i++)
		{
			if (d->bk == NULL && i + DU64G_AHEAD < m)
				__builtin_prefetch(&d->tab[h[i + DU64G_AHEAD] & (d->cap - 1)], 1);
			if (!put_one(d, gids[base + i], vals[base + i], h[i]))
				return base + i;
		}
	}
	return n;
}

void
sift_du64g_groups(SiftDu64G *d, SiftDu64GGroup fn, void *arg)
{
	if (d->bk == NULL)
	{
		for (uint32 g = 0; g < d->gcap; g++)
			if (d->gcount[g] != 0)
				fn(arg, g, d->gcount[g]);
		return;
	}
	if (d->gcount != NULL)
		pfree(d->gcount);
	d->gcount = MemoryContextAllocZero(d->cx, ((uint64) d->max_gid + 1) * sizeof(uint64));
	buckets_dedup(d);
	for (int i = 0; i < DU64G_BUCKETS; i++)
		for (uint32 k = 0; k < d->bk[i].n; k++)
			d->gcount[d->bk[i].p[k].gid1 - 1]++;
	for (uint32 g = 0; g <= d->max_gid; g++)
		if (d->gcount[g] != 0)
			fn(arg, g, d->gcount[g]);
}

void
sift_du64g_pairs(SiftDu64G *d, SiftDu64GPair fn, void *arg)
{
	if (d->bk == NULL)
	{
		for (uint64 i = 0; i < d->cap; i++)
			if (d->tab[i].gid1 != 0)
				fn(arg, d->tab[i].gid1 - 1, d->tab[i].val);
		return;
	}
	buckets_dedup(d);
	for (int i = 0; i < DU64G_BUCKETS; i++)
		for (uint32 k = 0; k < d->bk[i].n; k++)
			fn(arg, d->bk[i].p[k].gid1 - 1, d->bk[i].p[k].val);
}

void
sift_du64g_free(SiftDu64G *d)
{
	for (int i = 0; d->bk != NULL && i < DU64G_BUCKETS; i++)
		if (d->bk[i].p != NULL)
			pfree(d->bk[i].p);
	if (d->bk != NULL)
		pfree(d->bk);
	if (d->tab != NULL)
		pfree(d->tab);
	if (d->gcount != NULL)
		pfree(d->gcount);
	if (d->scratch != NULL)
		pfree(d->scratch);
	pfree(d);
}
