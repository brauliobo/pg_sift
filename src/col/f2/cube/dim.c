/*
 * col/f2/cube/dim.c - the group tables of the dims and rollups of a scope cube. A table is ascending by key (binary search,
 * appends at the end stay ascending) until a key lands in the middle of more than SDIM_SORTED_MAX groups: then it is append-only
 * and indexed by an open-addressed hash, and sift_sdim_sort puts it back in key order before anything reads it in order.
 * Merge and subtract of two ascending tables are one pass. Portable (core/host.h only).
 */
#include "core/host.h"
#include "col/f2/cube/int.h"

#define SDIM_SORTED_MAX 32
#define SDIM_HT_MIN		128

static int
key_cmp(const int64 *x, const int64 *y, uint32 nk)
{
	for (uint32 i = 0; i < nk; i++)
		if (x[i] != y[i])
			return x[i] < y[i] ? -1 : 1;
	return 0;
}

static int
rec_cmp(const void *a, const void *b, void *arg)
{
	return key_cmp(a, b, *(const uint32 *) arg);
}

static inline uint32
key_hash(const int64 *key, uint32 nk)
{
	uint64		h = 0x9E3779B97F4A7C15ULL;

	for (uint32 i = 0; i < nk; i++)
	{
		h ^= (uint64) key[i];
		h *= 0xBF58476D1CE4E5B9ULL;
		h ^= h >> 31;
	}
	return (uint32) (h ^ (h >> 32));
}

uint32
sift_sdim_search(const SiftScopeShape *s, const SiftScopeDim *d, const int64 *key, bool *found)
{
	uint32		lo = 0,
				hi = d->ngroups;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		int			c = key_cmp(sift_rec_key(sift_dim_rec(s, d, mid)), key, d->nk);

		if (c == 0)
		{
			*found = true;
			return mid;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = false;
	return lo;
}

static void
ht_put(const SiftScopeShape *s, SiftScopeDim *d, uint32 idx)
{
	uint32		mask = d->ht_cap - 1,
				at = key_hash(sift_rec_key(sift_dim_rec(s, d, idx)), d->nk) & mask;

	while (d->ht[at] != 0)
		at = (at + 1) & mask;
	d->ht[at] = (uint16) (idx + 1);
}

static void
ht_build(const SiftScopeShape *s, SiftScopeDim *d, uint32 want)
{
	uint32		cap = SDIM_HT_MIN;

	while (cap < want * 2)
		cap *= 2;
	if (d->ht != NULL)
		pfree(d->ht);
	d->ht = palloc0((Size) cap * sizeof(uint16));
	d->ht_cap = cap;
	for (uint32 i = 0; i < d->ngroups; i++)
		ht_put(s, d, i);
}

static char *
ht_find(const SiftScopeShape *s, const SiftScopeDim *d, const int64 *key)
{
	uint32		mask = d->ht_cap - 1,
				at = key_hash(key, d->nk) & mask;

	for (; d->ht[at] != 0; at = (at + 1) & mask)
	{
		char	   *rec = sift_dim_rec(s, d, d->ht[at] - 1);

		if (key_cmp(sift_rec_key(rec), key, d->nk) == 0)
			return rec;
	}
	return NULL;
}

void
sift_sdim_release(SiftScopeDim *d)
{
	if (d->groups != NULL)
		pfree(d->groups);
	if (d->ht != NULL)
		pfree(d->ht);
	d->groups = NULL;
	d->ht = NULL;
	d->ht_cap = d->cap = d->ngroups = 0;
	d->unsorted = false;
}

void
sift_sdim_sort(const SiftScopeShape *s, SiftScopeDim *d)
{
	uint32		nk = d->nk;

	if (!d->unsorted)
		return;
	qsort_arg(d->groups, d->ngroups, sift_rec_size(s, nk), rec_cmp, &nk);
	pfree(d->ht);
	d->ht = NULL;
	d->ht_cap = 0;
	d->unsorted = false;
}

static void
dim_grow(const SiftScopeShape *s, SiftScopeDim *d, uint32 need)
{
	if (need <= d->cap)
		return;
	d->cap = Max(need, d->cap == 0 ? 16 : d->cap * 2);
	d->groups = d->groups == NULL ? palloc((Size) d->cap * sift_rec_size(s, d->nk)) : repalloc(d->groups, (Size) d->cap * sift_rec_size(s, d->nk));
}

void
sift_sdim_overflow(SiftScopeDim *d)
{
	d->overflow = true;
	d->ngroups = 0;
	d->unsorted = false;
	if (d->ht != NULL)
		pfree(d->ht);
	d->ht = NULL;
	d->ht_cap = 0;
}

/* The record of key, created empty when it is new; NULL when the table has overflowed */
char *
sift_sdim_group_for(const SiftScopeShape *s, SiftScopeDim *d, const int64 *key)
{
	Size		gs = sift_rec_size(s, d->nk);
	uint32		at;
	char	   *rec;

	if (d->overflow)
		return NULL;
	if (d->unsorted)
	{
		if ((rec = ht_find(s, d, key)) != NULL)
			return rec;
		at = d->ngroups;
	}
	else
	{
		bool		found;

		at = sift_sdim_search(s, d, key, &found);
		if (found)
			return sift_dim_rec(s, d, at);
	}
	if (d->ngroups >= sift_dim_limit(d))
	{
		sift_sdim_overflow(d);
		return NULL;
	}
	dim_grow(s, d, d->ngroups + 1);
	if (!d->unsorted && at < d->ngroups)
	{
		if (d->ngroups < SDIM_SORTED_MAX)
			memmove(d->groups + (Size) (at + 1) * gs, d->groups + (Size) at * gs, (Size) (d->ngroups - at) * gs);
		else
		{
			d->unsorted = true;
			ht_build(s, d, d->ngroups + 1);
			at = d->ngroups;
		}
	}
	d->ngroups++;
	rec = sift_dim_rec(s, d, at);
	memset(rec, 0, gs);
	memcpy(rec, key, sizeof(int64) * d->nk);
	if (d->unsorted)
	{
		if (d->ngroups * 2 > d->ht_cap)
			ht_build(s, d, d->ngroups);
		else
			ht_put(s, d, at);
	}
	return rec;
}

/* dd += sd, both ascending and the keys not remapped: one pass into a table of the union */
void
sift_sdim_merge(const SiftScopeCubeAcc *a, SiftScopeDim *dd, SiftScopeDim *sd, SiftGroupFn fn)
{
	const SiftScopeShape *s = &a->shape;
	Size		gs = sift_rec_size(s, dd->nk);
	uint32		limit = sift_dim_limit(dd),
				i = 0,
				j = 0,
				n = 0;
	char	   *out;

	sift_sdim_sort(s, dd);
	sift_sdim_sort(s, sd);
	if (sd->ngroups == 0)
		return;
	out = palloc((Size) (dd->ngroups + sd->ngroups) * gs);
	while (i < dd->ngroups || j < sd->ngroups)
	{
		char	   *x = i < dd->ngroups ? sift_dim_rec(s, dd, i) : NULL,
				   *y = j < sd->ngroups ? sift_dim_rec(s, sd, j) : NULL;
		int			c = x == NULL ? 1 : y == NULL ? -1 : key_cmp(sift_rec_key(x), sift_rec_key(y), dd->nk);

		if (n >= limit)
		{
			pfree(out);
			sift_sdim_overflow(dd);
			return;
		}
		memcpy(out + (Size) n * gs, c <= 0 ? x : y, gs);
		if (c == 0)
			fn(a, out + (Size) n * gs, y, dd->nk);
		i += c <= 0;
		j += c >= 0;
		n++;
	}
	if (dd->groups != NULL)
		pfree(dd->groups);
	dd->groups = out;
	dd->cap = dd->ngroups + sd->ngroups;
	dd->ngroups = n;
}

/* dd -= sd, both ascending, every key of sd in dd: one pass; a group that is left with no rows goes. Returns the or of what fn says */
uint32
sift_sdim_sub(const SiftScopeCubeAcc *a, SiftScopeDim *dd, SiftScopeDim *sd, SiftGroupSubFn fn)
{
	const SiftScopeShape *s = &a->shape;
	Size		gs = sift_rec_size(s, dd->nk);
	uint32		mask = 0,
				w = 0,
				j = 0;

	sift_sdim_sort(s, dd);
	sift_sdim_sort(s, sd);
	for (uint32 r = 0; r < dd->ngroups; r++)
	{
		char	   *drec = sift_dim_rec(s, dd, r);

		if (j < sd->ngroups && key_cmp(sift_rec_key(drec), sift_rec_key(sift_dim_rec(s, sd, j)), dd->nk) == 0)
		{
			mask |= fn(a, drec, sift_dim_rec(s, sd, j), dd->nk);
			j++;
			if (*sift_rec_cnt(drec, dd->nk) == 0)
				continue;
		}
		if (w != r)
			memcpy(dd->groups + (Size) w * gs, drec, gs);
		w++;
	}
	SIFT_INVARIANT(j == sd->ngroups, "scope cube subtract: a group that is not in the cube");
	dd->ngroups = w;
	return mask;
}

/* An open-addressed index of the entries of an array that start with an int32 id (the month tables): ht holds slot + 1 */
static inline uint32
id_hash(int32 id)
{
	return (uint32) id * 2654435761U;
}

void
sift_idix_rebuild(uint16 **ht, uint32 *cap, const void *base, Size stride, uint32 n)
{
	uint32		c = SDIM_HT_MIN;

	while (c < n * 2)
		c *= 2;
	if (*ht != NULL)
		pfree(*ht);
	*ht = palloc0((Size) c * sizeof(uint16));
	*cap = c;
	for (uint32 i = 0; i < n; i++)
		sift_idix_note(*ht, *cap, base, stride, i);
}

void
sift_idix_note(uint16 *ht, uint32 cap, const void *base, Size stride, uint32 slot)
{
	uint32		at = id_hash(*(const int32 *) ((const char *) base + slot * stride)) & (cap - 1);

	while (ht[at] != 0)
		at = (at + 1) & (cap - 1);
	ht[at] = (uint16) (slot + 1);
}

int32
sift_idix_find(const uint16 *ht, uint32 cap, const void *base, Size stride, int32 id)
{
	for (uint32 at = id_hash(id) & (cap - 1); ht[at] != 0; at = (at + 1) & (cap - 1))
		if (*(const int32 *) ((const char *) base + (Size) (ht[at] - 1) * stride) == id)
			return ht[at] - 1;
	return -1;
}
