/*
 * agg/v6/par/distinct.c - the radix COUNT(DISTINCT) of agg/v6/par/distinct.h. Nothing here calls the server.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/v6/par/distinct.h"
#include "agg/v6/v6_distinct_u64.h"
#include "core/par/radix.h"
#include "core/popcount.h"

#define DIST_TILE		256
#define DIST_FIRST_CAP	1024
#define DIST_PART_MAX	16384

typedef struct DistWorker
{
	SiftRadixOut out;
	uint64		cnt;			/* the distinct values of the one set that phase 2 of this thread counted */
	uint64	   *gcnt;			/* with groups: the distinct values of each group it counted */
} DistWorker;

struct SiftParDistinct
{
	SiftParDistinctHooks hooks;
	SiftParArenas *ar;
	int			nw;
	uint32		ngroups;
	DistWorker *w;
};

static inline uint32
rec_bytes(const SiftParDistinct *d)
{
	return d->ngroups == 0 ? sizeof(uint64) : sizeof(SiftDuPair);
}

static bool
fail_mem(SiftErr *err, uint32 arg0, uint32 arg1, const char *msg)
{
	sift_err_fill(err, SIFT_ERR_WORK_MEM, arg0, arg1, msg);
	return false;
}

SiftParDistinct *
sift_par_distinct_create(SiftParArenas *ar, uint32 ngroups, const SiftParDistinctHooks *hooks, SiftErr *err)
{
	SiftArena  *boot = &ar->w[0].state;
	SiftParDistinct *d;

	if (ngroups == UINT32_MAX)
		return sift_err_fill(err, SIFT_ERR_UNSUPPORTED, ngroups, 0, "parallel distinct: too many groups"), NULL;
	d = sift_arena_alloc0(boot, sizeof(SiftParDistinct), SIFT_ARENA_ALIGN);
	if (d != NULL)
		d->w = sift_arena_alloc0(boot, sizeof(DistWorker) * ar->n, SIFT_ARENA_ALIGN);
	if (d == NULL || d->w == NULL)
		return fail_mem(err, 0, 0, "parallel distinct: no room for the state"), NULL;
	d->hooks = *hooks;
	d->ar = ar;
	d->nw = ar->n;
	d->ngroups = ngroups;
	for (int t = 0; t < d->nw; t++)
	{
		sift_radix_init(&d->w[t].out, &ar->w[t].state);
		if (ngroups > 0)
			d->w[t].gcnt = sift_arena_alloc0(&ar->w[t].work, (size_t) ngroups * sizeof(uint64), SIFT_ARENA_ALIGN);
		if (ngroups > 0 && d->w[t].gcnt == NULL)
			return fail_mem(err, t, ngroups, "parallel distinct: an arena is too small for the counts of the groups"), NULL;
	}
	return d;
}

/* ---- phase 1 ---- */

static bool
distinct_phase1(void *vd, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SiftParDistinct *d = vd;
	SiftRadixOut *o = &d->w[t].out;
	SiftParDistinctBatch b = {0};

	*done = 0;
	if (!d->hooks.fill(d->hooks.arg, t, lo, hi, scratch, &b, err))
		return false;
	if (!sift_radix_fits(o, (uint64) b.n * rec_bytes(d)))
		return fail_mem(err, t, b.n, "parallel distinct: the arena of a thread has no room for a batch");
	for (uint32 i = 0; i < b.n; i++)
	{
		bool		ok;

		if (d->ngroups == 0)
			ok = sift_radix_put(o, sift_radix_part(sift_du64_mix(b.val[i])), &b.val[i], sizeof(uint64));
		else
		{
			const uint64 h = sift_du64_pair_hash(b.val[i], b.gid[i]);
			const SiftDuPair rec = {.val = b.val[i],.gid1 = b.gid[i] + 1,.h32 = (uint32) h};

			ok = sift_radix_put(o, sift_radix_part(h), &rec, sizeof(rec));
		}
		if (!ok)
			return fail_mem(err, t, b.n, "parallel distinct: a batch that fits did not fit");
	}
	return true;
}

static bool
distinct_seal(void *vd, uint32 *nparts, SiftErr *err)
{
	*nparts = SIFT_RADIX_PARTS;
	return true;
}

/* ---- phase 2 ---- */

/* the table of a partition, in the arena of the thread: values, or pairs */
typedef struct DistTab
{
	SiftArena  *a;
	uint64	   *flat;
	SiftDuPair *pairs;
	uint64		cap;
	uint64		count;
	uint64	   *gcnt;
	bool		zero;			/* the value 0 (the empty entry of the table) is in the partition */
} DistTab;

static bool
tab_grow(DistTab *t)
{
	const uint64 ncap = t->cap * 2;

	if (t->flat != NULL)
	{
		uint64	   *n = sift_arena_alloc0(t->a, ncap * sizeof(uint64), SIFT_ARENA_ALIGN);

		if (n == NULL)
			return false;
		sift_du64_rehash(n, ncap, t->flat, t->cap);
		t->flat = n;
	}
	else
	{
		SiftDuPair *n = sift_arena_alloc0(t->a, ncap * sizeof(SiftDuPair), SIFT_ARENA_ALIGN);

		if (n == NULL)
			return false;
		sift_du64g_rehash(n, ncap, t->pairs, t->cap);
		t->pairs = n;
	}
	t->cap = ncap;
	return true;
}

static bool
tab_chunk(void *arg, const uint8 *data, uint32 len)
{
	DistTab    *t = arg;
	const uint32 rec = t->flat != NULL ? sizeof(uint64) : sizeof(SiftDuPair);

	for (uint32 off = 0; off < len;)
	{
		const uint32 m = Min(DIST_TILE, (len - off) / rec);

		while ((t->count + m) * 2 > t->cap)
			if (!tab_grow(t))
				return false;
		if (t->flat != NULL)
		{
			const uint64 *vals = (const uint64 *) (data + off);

			for (uint32 i = 0; i < m && !t->zero; i++)
				t->zero = vals[i] == 0;
			t->count += sift_du64_dedup_slice(t->flat, t->cap, vals, m);
		}
		else
		{
			SiftDuPair	tile[DIST_TILE];
			uint32		fresh;

			memcpy(tile, data + off, (size_t) m * rec);
			fresh = sift_du64g_dedup_bucket(t->pairs, t->cap, tile, m);
			for (uint32 i = 0; i < fresh; i++)
				t->gcnt[tile[i].gid1 - 1]++;
			t->count += fresh;
		}
		off += m * rec;
	}
	return true;
}

static bool
dedup_partition(SiftParDistinct *d, DistWorker *w, SiftArena *work, uint32 part)
{
	DistTab		t = {.a = work,.gcnt = w->gcnt};
	uint64		bound = 0;

	for (int v = 0; v < d->nw; v++)
		bound += sift_radix_nrec(&d->w[v].out, part);
	t.cap = pg_nextpower2_64(Max(DIST_FIRST_CAP, Min(bound, DIST_PART_MAX) * 2));
	if (d->ngroups == 0)
		t.flat = sift_arena_alloc0(work, t.cap * sizeof(uint64), SIFT_ARENA_ALIGN);
	else
		t.pairs = sift_arena_alloc0(work, t.cap * sizeof(SiftDuPair), SIFT_ARENA_ALIGN);
	if (t.flat == NULL && t.pairs == NULL)
		return false;
	for (int v = 0; v < d->nw; v++)
		if (!sift_radix_scan(&d->w[v].out, part, tab_chunk, &t))
			return false;
	w->cnt += t.count + (t.zero ? 1 : 0);
	return true;
}

static bool
distinct_phase2(void *vd, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	SiftParDistinct *d = vd;

	return dedup_partition(d, &d->w[t], work, part) || fail_mem(err, t, part, "parallel distinct: the arena of a partition is full");
}

/* ---- the answer ---- */

uint64
sift_par_distinct_count(const SiftParDistinct *d)
{
	uint64		n = 0;

	for (int t = 0; t < d->nw; t++)
		n += d->w[t].cnt;
	return n;
}

void
sift_par_distinct_groups(const SiftParDistinct *d, uint64 *out)
{
	for (uint32 g = 0; g < d->ngroups; g++)
	{
		uint64		n = 0;

		for (int v = 0; v < d->nw; v++)
			n += d->w[v].gcnt[g];
		out[g] = n;
	}
}

typedef struct Drain
{
	const SiftParDistinct *d;
} Drain;

static bool
drain_chunk(void *arg, const uint8 *data, uint32 len)
{
	const SiftParDistinct *d = ((const Drain *) arg)->d;

	for (uint32 off = 0; off < len; off += rec_bytes(d))
	{
		if (d->ngroups == 0)
		{
			uint64		v;

			memcpy(&v, data + off, sizeof(v));
			d->hooks.value(d->hooks.arg, 0, v);
		}
		else
		{
			SiftDuPair	pr;

			memcpy(&pr, data + off, sizeof(pr));
			d->hooks.value(d->hooks.arg, pr.gid1 - 1, pr.val);
		}
	}
	return true;
}

static void
distinct_overflow(void *vd, const SiftParRange *left, uint32 nleft)
{
	SiftParDistinct *d = vd;
	Drain		dr = {.d = d};

	for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
		for (int v = 0; v < d->nw; v++)
			sift_radix_scan(&d->w[v].out, p, drain_chunk, &dr);
	d->hooks.overflow(d->hooks.arg, left, nleft);
}

const SiftParOps sift_par_distinct_ops = {.phase1 = distinct_phase1,.seal = distinct_seal,.phase2 = distinct_phase2,.overflow_drain = distinct_overflow};

/* ---- dictionary codes ---- */

uint64
sift_par_distinct_bits_or(const uint64 *const *bits, int nbits, uint32 nwords, int t, int nthreads, uint64 *out)
{
	const uint64 lo = (uint64) nwords * t / nthreads,
				hi = (uint64) nwords * (t + 1) / nthreads;
	uint64		set = 0;

	for (uint64 i = lo; i < hi; i++)
	{
		uint64		x = 0;

		for (int b = 0; b < nbits; b++)
			x |= bits[b][i];
		if (out != NULL)
			out[i] = x;
		set += sift_pc64(x);
	}
	return set;
}
