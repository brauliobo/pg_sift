/*
 * agg/par/distinct_p2.c - phase 2 of the radix COUNT(DISTINCT) of agg/par/distinct.h: a thread deduplicates the partition it took, in the arena `work` that is rewound after it. Nothing here
 * calls the server.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/par/distinct_int.h"
#include "agg/distinct/u64.h"

#define DIST_TILE		256
#define DIST_FIRST_CAP	1024
#define DIST_PART_MAX	16384
#define TILE_AHEAD		8

/* The partition: its keys and its texts numbered across the threads, and the table of its values, or of its pairs of a key and a value */
typedef struct P2
{
	SiftParDistinct *d;
	int			t;				/* the thread */
	DistWorker *src;			/* the thread whose records are read */
	SiftArena  *work;
	SiftKeyTab	k2;
	SiftKeyTab	v2;
	uint64	   *flat;
	SiftDuPair *pairs;
	uint64		cap;
	uint64		count;
	bool		zero;			/* the value 0 (the empty entry of the flat table) is in the partition */
} P2;

static bool
tab_open(P2 *p, uint32 part)
{
	uint64		bound = 0;

	for (int v = 0; v < p->d->nw; v++)
		bound += sift_radix_nrec(&p->d->w[v].raw, part);
	p->cap = pg_nextpower2_64(Max(DIST_FIRST_CAP, Min(bound, p->d->grouped ? 4 * DIST_PART_MAX : DIST_PART_MAX) * 2));
	if (p->d->grouped)
		return (p->pairs = sift_arena_alloc0(p->work, p->cap * sizeof(SiftDuPair), SIFT_ARENA_ALIGN)) != NULL;
	return (p->flat = sift_arena_alloc0(p->work, p->cap * sizeof(uint64), SIFT_ARENA_ALIGN)) != NULL;
}

static bool
tab_grow(P2 *p)
{
	const uint64 ncap = p->cap * 2;

	if (p->d->grouped)
	{
		SiftDuPair *n = sift_arena_alloc0(p->work, ncap * sizeof(SiftDuPair), SIFT_ARENA_ALIGN);

		if (n == NULL)
			return false;
		sift_du64g_rehash(n, ncap, p->pairs, p->cap);
		p->pairs = n;
	}
	else
	{
		uint64	   *n = sift_arena_alloc0(p->work, ncap * sizeof(uint64), SIFT_ARENA_ALIGN);

		if (n == NULL)
			return false;
		sift_du64_rehash(n, ncap, p->flat, p->cap);
		p->flat = n;
	}
	p->cap = ncap;
	return true;
}

static bool
tab_room(P2 *p, uint32 more)
{
	while ((p->count + more) * 2 > p->cap)
		if (!tab_grow(p))
			return false;
	return true;
}

/* No key, a word value: the words of the chunk into the flat table */
static bool
flat_chunk(void *arg, const uint8 *data, uint32 len)
{
	P2		   *p = arg;

	for (uint32 off = 0; off < len;)
	{
		const uint32 m = Min(DIST_TILE, (len - off) / sizeof(uint64));
		const uint64 *vals = (const uint64 *) (data + off);

		if (!tab_room(p, m))
			return false;
		for (uint32 i = 0; i < m && !p->zero; i++)
			p->zero = vals[i] == 0;
		p->count += sift_du64_dedup_slice(p->flat, p->cap, vals, m);
		off += m * sizeof(uint64);
	}
	return true;
}

/* No key, a text value: the texts of the thread, numbered by what they say */
static bool
text_chunk(void *arg, const uint8 *data, uint32 len)
{
	P2		   *p = arg;

	for (uint32 off = 0; off < len; off += sizeof(uint64))
	{
		uint64		v;

		memcpy(&v, data + off, sizeof(v));
		if (sift_keytab_intern_of(&p->v2, sift_keytab_at(&p->src->vals, (uint32) v)) == SIFT_KEYTAB_NONE)
			return false;
	}
	return true;
}

/* The rows, sums and texts of the keys of a thread, into the keys of the partition, which tell the thread's key the number it has here */
static bool
agg_chunk(void *arg, const uint8 *data, uint32 len)
{
	P2		   *p = arg;
	const SiftParDistinct *d = p->d;
	const uint32 words = 1 + dist_pay_last(d);

	for (uint32 off = 0; off < len; off += words * sizeof(uint64))
	{
		const uint64 *rec = (const uint64 *) (data + off);
		uint64	   *e = sift_keytab_at(&p->src->keys, (uint32) rec[0]);
		const uint32 id = sift_keytab_intern_of(&p->k2, e);
		uint64	   *pay;

		if (id == SIFT_KEYTAB_NONE)
			return false;
		pay = sift_keytab_pay(&p->k2, sift_keytab_at(&p->k2, id));
		for (uint32 m = 0; m <= d->sp.nsum; m++)
			pay[m] += rec[1 + m];
		for (int m = 0; m < d->sp.nmm; m++)
		{
			const uint64 *s = rec + 1 + dist_pay_mm(d, m);

			if (s[1] != 0)
				dist_mm_offer(pay + dist_pay_mm(d, m), (d->sp.mm_max >> m) & 1, (const uint8 *) (uintptr_t) s[0], (uint32) (s[1] - 1), NULL);
		}
		sift_keytab_pay(&p->src->keys, e)[dist_pay_last(d)] = id;
	}
	return true;
}

/* The pairs of the records with the number of their key in this partition and the number of their text in this partition; those of a direct table are what phase 1 wrote */
static bool
tile_remap(P2 *p, const SiftDuPair *rec, uint32 m, SiftDuPair *tile)
{
	for (uint32 i = 0; i < m; i++)
	{
		const uint32 kid = rec[i].gid1 - 1;
		const uint32 g = p->d->direct ? kid : (uint32) sift_keytab_pay(&p->src->keys, sift_keytab_at(&p->src->keys, kid))[dist_pay_last(p->d)];
		uint64		v = rec[i].val;

		if (!p->d->direct && i + TILE_AHEAD < m)
			__builtin_prefetch(sift_keytab_at(&p->src->keys, rec[i + TILE_AHEAD].gid1 - 1));
		if (p->d->sp.vstr && (v = sift_keytab_intern_of(&p->v2, sift_keytab_at(&p->src->vals, (uint32) v))) == SIFT_KEYTAB_NONE)
			return false;
		tile[i] = (SiftDuPair) {.val = v,.gid1 = g + 1,.h32 = (uint32) sift_du64_pair_hash(v, g)};
	}
	return true;
}

/* A key, and the values of its rows: the pairs of the chunk new to the table add one to the distinct values of their key */
static bool
pair_chunk(void *arg, const uint8 *data, uint32 len)
{
	P2		   *p = arg;
	const bool	as_written = p->d->direct && !p->d->sp.vstr;
	const SiftKeyTab *tab = p->d->direct ? &p->d->w[p->t].keys : &p->k2;
	uint64	   *distinct = sift_keytab_pay(tab, sift_keytab_at(tab, 0)) + dist_pay_last(p->d);
	const size_t step = tab->ew;

	for (uint32 off = 0; off < len;)
	{
		const uint32 m = Min(DIST_TILE, (len - off) / sizeof(SiftDuPair));
		SiftDuPair	tile[DIST_TILE];
		uint32		fresh;

		if (!tab_room(p, m))
			return false;
		if (as_written)
			memcpy(tile, data + off, (size_t) m * sizeof(SiftDuPair));
		else if (!tile_remap(p, (const SiftDuPair *) (data + off), m, tile))
			return false;
		fresh = sift_du64g_dedup_bucket(p->pairs, p->cap, tile, m);
		for (uint32 i = 0; i < fresh; i++)
			distinct[(tile[i].gid1 - 1) * step]++;
		p->count += fresh;
		off += m * sizeof(SiftDuPair);
	}
	return true;
}

static bool
scan_threads(P2 *p, uint32 part, bool agg, SiftRadixChunkFn fn)
{
	for (int v = 0; v < p->d->nw; v++)
	{
		p->src = &p->d->w[v];
		if (!sift_radix_scan(agg ? &p->src->agg : &p->src->raw, part, fn, p))
			return false;
	}
	return true;
}

/* The keys of the partition, with their rows, sums and distinct values, kept as the answer of the thread: blocks of its state arena */
static bool
keys_out(const P2 *p, int t)
{
	DistWorker *w = &p->d->w[t];
	const size_t bytes = (size_t) p->k2.count * p->k2.ew * sizeof(uint64);
	DistOut    *o;

	if (p->k2.count == 0)
		return true;
	if ((o = sift_arena_alloc(&p->d->ar->w[t].state, offsetof(DistOut, ent) + bytes, sizeof(uint64))) == NULL)
		return false;
	o->next = NULL;
	o->n = p->k2.count;
	memcpy(o->ent, p->k2.ent, bytes);
	if (w->out == NULL)
		w->out = o;
	else
		w->out_tail->next = o;
	w->out_tail = o;
	return true;
}

static bool
grouped_partition(P2 *p, int t, uint32 part)
{
	if (p->d->direct)
		return tab_open(p, part) && scan_threads(p, part, false, pair_chunk);
	return tab_open(p, part) && scan_threads(p, part, true, agg_chunk) && scan_threads(p, part, false, pair_chunk) && keys_out(p, t);
}

bool
sift_dist_phase2(SiftParDistinct *d, int t, uint32 part, SiftArena *work)
{
	P2			p = {.d = d,.t = t,.work = work};

	if ((d->grouped && !d->direct && !sift_keytab_init(&p.k2, work, d->ks, true)) || (d->sp.vstr && !sift_keytab_init(&p.v2, work, d->vs, true)))
		return false;
	if (d->grouped)
		return grouped_partition(&p, t, part);
	if (d->sp.vstr)
	{
		if (!scan_threads(&p, part, false, text_chunk))
			return false;
		d->w[t].cnt += p.v2.count;
		return true;
	}
	if (!tab_open(&p, part) || !scan_threads(&p, part, false, flat_chunk))
		return false;
	d->w[t].cnt += p.count + (p.zero ? 1 : 0);
	return true;
}
