/*
 * agg/intids.c - the ids (1..) of 64-bit integer keys, in order of first sight
 * A hash of (key, id) that becomes pages of ids indexed by the key while the keys are in a small or dense range (agg/intids.h).
 */
#include "postgres.h"

#include "agg/intids.h"

/* the pages cover at most this many keys; beyond FREE_SPAN (default) a range must be at most four times the keys held, with a little slack */
#define SPAN_MAX		(UINT64CONST(1) << 26)
#define FREE_SPAN		(1 << 20)
#define DENSE_SLACK		1024
#define KEY_END			(PG_INT64_MAX - (INTIDS_PAGE - 1))	/* the pages end here at most, so that no page runs past int64: a key from here up is hashed */

static inline uint64
span_limit(const IntIds *x)
{
	return Max((uint64) x->free_span, Min(4 * (uint64) x->n + DENSE_SLACK, SPAN_MAX));
}

static inline int64
page_floor(int64 k)
{
	return (int64) ((uint64) k & ~(uint64) (INTIDS_PAGE - 1));
}

static inline bool
key_edge(int64 k)
{
	return k >= KEY_END;
}

void
intids_init(IntIds *x)
{
	memset(x, 0, sizeof(*x));
	x->cxt = CurrentMemoryContext;
	x->free_span = FREE_SPAN;
	x->nslot = 64;
	/* check-memory: allow the first 64 slots */
	x->tab = MemoryContextAllocZero(x->cxt, x->nslot * sizeof(IntIdEnt));
	x->kmin = PG_INT64_MAX;
	x->kmax = PG_INT64_MIN;
}

static void
free_pages(IntIds *x)
{
	for (uint32 i = 0; i < x->npg; i++)
		if (x->pg[i])
			pfree(x->pg[i]);
	pfree(x->pg);
	x->pg = NULL;
	x->npg = 0;
}

void
intids_free(IntIds *x)
{
	if (x->tab)
		pfree(x->tab);
	if (x->pg)
		free_pages(x);
	memset(x, 0, sizeof(*x));
}

static void
tab_put(IntIdEnt *tab, uint32 mask, int64 k, uint32 id)
{
	uint32		h = intids_hash(k) & mask;

	while (tab[h].id)
		h = (h + 1) & mask;
	tab[h] = (IntIdEnt) {k, id};
}

/* the keys are no longer in a range the pages serve: the hash again, of room for twice the keys it holds */
static void
to_hash(IntIds *x)
{
	uint32		nslot = 64;
	IntIdEnt   *tab;

	while (nslot < 4 * x->n)
		nslot *= 2;
	tab = MemoryContextAllocExtended(x->cxt, nslot * sizeof(IntIdEnt), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	x->kmin = PG_INT64_MAX;
	x->kmax = PG_INT64_MIN;
	for (uint32 i = 0; i < x->npg; i++)
		for (int j = 0; x->pg[i] && j < INTIDS_PAGE; j++)
		{
			int64		k;

			if (x->pg[i][j] == 0)
				continue;
			k = (int64) ((uint64) x->lo + ((uint64) i << INTIDS_PAGE_BITS) + j);
			tab_put(tab, nslot - 1, k, x->pg[i][j]);
			x->kmin = Min(x->kmin, k);
			x->kmax = Max(x->kmax, k);
		}
	free_pages(x);
	x->tab = tab;
	x->nslot = nslot;
}

/* a table of npg page pointers for the range from lo; the pages of the old one are kept */
static void
pages_table(IntIds *x, int64 lo, uint32 npg)
{
	uint32	  **pg = MemoryContextAllocZero(x->cxt, (Size) npg * sizeof(uint32 *));

	if (x->pg)
	{
		memcpy(pg + (((uint64) x->lo - (uint64) lo) >> INTIDS_PAGE_BITS), x->pg, (Size) x->npg * sizeof(uint32 *));
		pfree(x->pg);
	}
	x->pg = pg;
	x->npg = npg;
	x->lo = lo;
}

/* pages for [nlo, nhi) and as many again on the side the range grows (down: below nlo), within the limit and int64 */
static void
pages_cover(IntIds *x, int64 nlo, int64 nhi, bool down)
{
	uint64		need = (uint64) nhi - (uint64) nlo,
				want = Max(need, Min(2 * need, span_limit(x)));

	if (down)
	{
		want = Min(want, (uint64) nhi - (uint64) PG_INT64_MIN) & ~(uint64) (INTIDS_PAGE - 1);
		pages_table(x, (int64) ((uint64) nhi - want), (uint32) (want >> INTIDS_PAGE_BITS));
	}
	else
	{
		want = Min(want, (uint64) KEY_END - (uint64) nlo) & ~(uint64) (INTIDS_PAGE - 1);
		pages_table(x, nlo, (uint32) (want >> INTIDS_PAGE_BITS));
	}
}

/* the page of key off (from lo) is made when the first key falls in it */
static uint32 *
page_of(IntIds *x, uint64 off)
{
	uint32	  **pp = &x->pg[off >> INTIDS_PAGE_BITS];

	if (*pp == NULL)
		*pp = MemoryContextAllocZero(x->cxt, INTIDS_PAGE * sizeof(uint32));
	return *pp;
}

/* the hash holds keys of a range the pages serve: pages instead */
static bool
to_direct(IntIds *x)
{
	int64		lo = page_floor(x->kmin),
				hi = page_floor(x->kmax) + INTIDS_PAGE;
	IntIdEnt   *old = x->tab;
	uint32		onslot = x->nslot;

	if (key_edge(x->kmax) || (uint64) hi - (uint64) lo > span_limit(x))
		return false;
	x->tab = NULL;
	x->nslot = 0;
	pages_cover(x, lo, hi, false);
	for (uint32 h = 0; h < onslot; h++)
	{
		uint64		off;

		if (old[h].id == 0)
			continue;
		off = (uint64) old[h].key - (uint64) x->lo;
		page_of(x, off)[off & (INTIDS_PAGE - 1)] = old[h].id;
	}
	pfree(old);
	return true;
}

static void
grow(IntIds *x)
{
	IntIdEnt   *old = x->tab;
	uint32		onslot = x->nslot;

	if (to_direct(x))
		return;
	x->nslot *= 2;
	x->tab = MemoryContextAllocExtended(x->cxt, x->nslot * sizeof(IntIdEnt), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	for (uint32 h = 0; h < onslot; h++)
		if (old[h].id)
			tab_put(x->tab, x->nslot - 1, old[h].key, old[h].id);
	pfree(old);
}

/* the pages cover k as well; false when the range would be too sparse for pages */
static bool
extend(IntIds *x, int64 k)
{
	int64		wlo = x->lo,
				whi = (int64) ((uint64) x->lo + ((uint64) x->npg << INTIDS_PAGE_BITS)),	/* the end, exclusive */
				nlo = Min(wlo, page_floor(k)),
				nhi = Max(whi, page_floor(k) + INTIDS_PAGE);
	uint64		need = (uint64) nhi - (uint64) nlo;

	if (key_edge(k) || need > span_limit(x))
		return false;
	pages_cover(x, nlo, nhi, k < wlo);
	return true;
}

uint32
intids_slow(IntIds *x, int64 k, uint32 next)
{
	uint32		mask,
				h;

	if (x->pg)
	{
		uint64		off = (uint64) k - (uint64) x->lo;

		if (off < (uint64) x->npg << INTIDS_PAGE_BITS)
		{
			page_of(x, off)[off & (INTIDS_PAGE - 1)] = next;
			x->n++;
			return next;
		}
		if (extend(x, k))
			return intids_slow(x, k, next);
		to_hash(x);
	}
	mask = x->nslot - 1;
	for (h = intids_hash(k) & mask; x->tab[h].id; h = (h + 1) & mask)
		if (x->tab[h].key == k)
			return x->tab[h].id;
	x->tab[h] = (IntIdEnt) {k, next};
	x->n++;
	x->kmin = Min(x->kmin, k);
	x->kmax = Max(x->kmax, k);
	if ((uint64) x->n * 2 > x->nslot)
		grow(x);
	return next;
}
