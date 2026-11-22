/*
 * col/scope_v6/route.c - SEC_SCOPE_ENT_ROUTE: where the rows of an entity are stored in a scope clustered by primary key.
 *
 * A query for one entity cannot use the key order of the parts: it asks this section, which holds for every entity the key its rows are stored
 * under (nearly always one, the entity's first key), and reads the granules of that key only. An entity whose rows are stored under several keys
 * (some were written before a mapping change moved the entity and have not been compacted yet) lists all of them in the overflow pool, so the
 * answer is never narrower than the rows. The section is sorted by entity with a fence every SIFT_ENT_ROUTE_FENCE entities: a lookup is a search
 * of the fences, then of one block of 256 ids. Portable.
 */
/* check-memory: allow-allocs the arrays follow the entities of one scope, which the build holds by design (16 bytes a run, 12 an entity) */
#include "core/host.h"

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/wire.h"

typedef struct RouteView
{
	SiftEntRouteHeader h;
	const char *keys;			/* int64[nhome_keys] */
	const char *fences;			/* int64[nfences] */
	const char *ids;			/* int64[nentities] */
	const char *refs;			/* uint32[nentities] */
	const char *over;			/* uint32[noverflow] */
} RouteView;

static inline uint64
pad8(uint64 n)
{
	return (n + 7) & ~UINT64CONST(7);
}

static int
home_cmp(const void *a, const void *b)
{
	const SiftEntHome *x = a,
			   *y = b;

	if (x->entity_id != y->entity_id)
		return x->entity_id < y->entity_id ? -1 : 1;
	return x->home < y->home ? -1 : x->home > y->home;
}

void
sift_homes_init(SiftHomeSet *s, MemoryContext mcxt)
{
	memset(s, 0, sizeof(*s));
	s->mcxt = mcxt;
	s->runs = MemoryContextAlloc(mcxt, sizeof(SiftEntHome) << SIFT_GRANULE_ROW_BITS);
}

static void
home_note(SiftHomeSet *s, int64 entity, int64 home)
{
	if (s->nhomes > 0 && s->homes[s->nhomes - 1].entity_id == entity && s->homes[s->nhomes - 1].home == home)
		return;
	if (s->nhomes == s->caphomes)
	{
		s->caphomes = s->caphomes == 0 ? 256 : s->caphomes * 2;
		s->homes = s->homes == NULL ? MemoryContextAlloc(s->mcxt, s->caphomes * sizeof(SiftEntHome)) : repalloc(s->homes, s->caphomes * sizeof(SiftEntHome));
	}
	s->homes[s->nhomes++] = (SiftEntHome) {entity, home};
}

void
sift_homes_runs(SiftHomeSet *s, const SiftScopeGranSource *src, uint32 gidx, const SiftScopeGranHead *h, MemoryContext caller)
{
	if (h->member.has_min_max && h->member.nnull == 0 && h->member.min == h->member.max && h->has_key_zone && h->key.nnull == 0 && h->key.min == h->key.max)
	{
		s->runs[0] = (SiftEntHome) {h->member.min, h->key.min};
		s->nruns = 1;
	}
	else
	{
		MemoryContext old = MemoryContextSwitchTo(caller);

		SIFT_INVARIANT(src->runs != NULL, "scope build: a clustered scope reads the entity runs of its granules");
		s->nruns = src->runs(src->arg, gidx, s->runs);
		MemoryContextSwitchTo(old);
		SIFT_INVARIANT(s->nruns <= h->nrows, "scope build: %u entity runs in a granule of %u rows", s->nruns, h->nrows);
	}
}

void
sift_homes_keep(SiftHomeSet *s)
{
	for (uint32 i = 0; i < s->nruns; i++)
		home_note(s, s->runs[i].entity_id, s->runs[i].home);
}

uint32
sift_homes_fit(const SiftHomeSet *s, SiftScopePairMap *pm, uint32 ord)
{
	uint32		fit = 0;

	for (uint32 i = 0; i < s->nruns; i++)
	{
		uint32		lo,
					hi;
		bool		has = false;

		if (sift_spm_find(pm, s->runs[i].entity_id, &lo, &hi))
		{
			for (uint32 q = lo; q < hi; q++)
			{
				has |= pm->key_ordinals[q] == ord;
				fit |= pm->key_ordinals[q] != ord ? SIFT_HOMES_OTHER : 0;
			}
		}
		if (!has && ord != UINT32_MAX)
			fit |= SIFT_HOMES_LACK;
	}
	return fit;
}

static uint32
key_index(const int64 *keys, uint32 nkeys, int64 key)
{
	const int64 *at = bsearch(&key, keys, nkeys, sizeof(int64), sift_cmp_i64);

	SIFT_INVARIANT(at != NULL, "scope route: a home that is not among the keys");
	return (uint32) (at - keys);
}

void
sift_ent_route_write(StringInfo out, SiftEntHome *h, uint32 n, uint64 max_part_gen)
{
	uint32		u = 0,
				nkeys = 0,
				nent = 0,
				nover = 0,
				e = 0,
				o = 0;
	int64	   *keys = palloc(sizeof(int64) * Max(n, 1)),
			   *ids;
	uint32	   *refs,
			   *over;
	SiftEntRouteHeader hd = {SIFT_ENT_ROUTE_MAGIC, SIFT_ENT_ROUTE_VER, 0, 0, 0, 0, 0, max_part_gen};

	qsort(h, n, sizeof(*h), home_cmp);
	for (uint32 i = 0; i < n; i++)
		if (i == 0 || home_cmp(&h[i - 1], &h[i]) != 0)
			h[u++] = h[i];
	n = u;
	for (uint32 i = 0; i < n; i++)
		keys[i] = h[i].home;
	qsort(keys, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (nkeys == 0 || keys[nkeys - 1] != keys[i])
			keys[nkeys++] = keys[i];
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && h[j].entity_id == h[i].entity_id; j++)
			;
		nent++;
		nover += j - i > 1 ? 1 + (j - i) : 0;
	}
	SIFT_INVARIANT(nkeys < SIFT_ENT_ROUTE_MULTI && nover < SIFT_ENT_ROUTE_MULTI, "scope route: more keys or homes than a reference names");
	ids = palloc(sizeof(int64) * Max(nent, 1));
	refs = palloc(sizeof(uint32) * Max(nent, 1));
	over = palloc(sizeof(uint32) * Max(nover, 1));
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && h[j].entity_id == h[i].entity_id; j++)
			;
		ids[e] = h[i].entity_id;
		if (j - i == 1)
			refs[e] = key_index(keys, nkeys, h[i].home);
		else
		{
			refs[e] = o | SIFT_ENT_ROUTE_MULTI;
			over[o++] = j - i;
			for (uint32 q = i; q < j; q++)
				over[o++] = key_index(keys, nkeys, h[q].home);
		}
		e++;
	}
	hd.nentities = nent;
	hd.noverflow = nover;
	hd.nhome_keys = nkeys;
	hd.nfences = (nent + SIFT_ENT_ROUTE_FENCE - 1) / SIFT_ENT_ROUTE_FENCE;
	appendBinaryStringInfo(out, (char *) &hd, sizeof(hd));
	appendBinaryStringInfo(out, (char *) keys, (Size) nkeys * sizeof(int64));
	for (uint32 i = 0; i < nent; i += SIFT_ENT_ROUTE_FENCE)
		appendBinaryStringInfo(out, (char *) &ids[i], sizeof(int64));
	appendBinaryStringInfo(out, (char *) ids, (Size) nent * sizeof(int64));
	appendBinaryStringInfo(out, (char *) refs, (Size) nent * sizeof(uint32));
	wire_pad8(out);
	appendBinaryStringInfo(out, (char *) over, (Size) nover * sizeof(uint32));
	wire_pad8(out);
	pfree(keys);
	pfree(ids);
	pfree(refs);
	pfree(over);
}

static bool
view_open(const SiftScopeSection *s, RouteView *v)
{
	uint64		len;

	if (s->p == NULL || s->len < sizeof(v->h))
		return false;
	memcpy(&v->h, s->p, sizeof(v->h));
	if (v->h.magic != SIFT_ENT_ROUTE_MAGIC || v->h.version != SIFT_ENT_ROUTE_VER || v->h.flags != 0 ||
		v->h.nfences != ((uint64) v->h.nentities + SIFT_ENT_ROUTE_FENCE - 1) / SIFT_ENT_ROUTE_FENCE)
		return false;
	len = sizeof(v->h) + (uint64) 8 * (v->h.nhome_keys + v->h.nfences + v->h.nentities) + pad8((uint64) 4 * v->h.nentities) +
		pad8((uint64) 4 * v->h.noverflow);
	if (len != s->len)
		return false;
	v->keys = s->p + sizeof(v->h);
	v->fences = v->keys + (uint64) 8 * v->h.nhome_keys;
	v->ids = v->fences + (uint64) 8 * v->h.nfences;
	v->refs = v->ids + (uint64) 8 * v->h.nentities;
	v->over = v->refs + pad8((uint64) 4 * v->h.nentities);
	return true;
}

bool
sift_ent_route_check(const SiftScopeSection *s)
{
	RouteView	v;

	return view_open(s, &v);
}

static inline int64
get_i64(const char *base, uint32 i)
{
	int64		x;

	memcpy(&x, base + (uint64) i * 8, 8);
	return x;
}

static inline uint32
get_u32(const char *base, uint32 i)
{
	uint32		x;

	memcpy(&x, base + (uint64) i * 4, 4);
	return x;
}

/* The homes of the entity at index at: how many, and the first max of them */
static uint32
view_homes(const RouteView *v, uint32 at, int64 *homes, uint32 max)
{
	uint32		ref = get_u32(v->refs, at),
				cnt = 1,
				off = 0;

	if (ref & SIFT_ENT_ROUTE_MULTI)
	{
		off = ref & ~SIFT_ENT_ROUTE_MULTI;
		SIFT_INVARIANT(off < v->h.noverflow, "scope route: a reference outside the overflow pool");
		cnt = get_u32(v->over, off++);
		SIFT_INVARIANT(cnt > 1 && (uint64) off + cnt <= v->h.noverflow, "scope route: an overflow record that does not fit the pool");
	}
	for (uint32 i = 0; i < cnt && i < max; i++)
	{
		uint32		k = ref & SIFT_ENT_ROUTE_MULTI ? get_u32(v->over, off + i) : ref;

		SIFT_INVARIANT(k < v->h.nhome_keys, "scope route: a home that is not among the keys");
		homes[i] = get_i64(v->keys, k);
	}
	return cnt;
}

SiftEntRouteKind
sift_scope_ent_route(const SiftScopeMeta *m, int64 entity_id, int64 *homes, uint32 max, uint32 *nhomes)
{
	RouteView	v;
	uint32		lo = 0,
				hi,
				f;

	*nhomes = 0;
	if (!view_open(&m->ent_route, &v) || v.h.nentities == 0)
		return SIFT_ROUTE_NONE;
	hi = v.h.nfences;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (get_i64(v.fences, mid) <= entity_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return SIFT_ROUTE_NONE;
	f = lo - 1;
	lo = f * SIFT_ENT_ROUTE_FENCE;
	hi = Min(v.h.nentities, lo + SIFT_ENT_ROUTE_FENCE);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		int64		id = get_i64(v.ids, mid);

		if (id == entity_id)
		{
			*nhomes = view_homes(&v, mid, homes, max);
			return *nhomes > 1 ? SIFT_ROUTE_SPILL : SIFT_ROUTE_PURE;
		}
		if (id < entity_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return SIFT_ROUTE_NONE;
}

bool
sift_scope_ent_route_gen(const SiftScopeMeta *m, uint64 *max_part_gen)
{
	RouteView	v;

	if (!view_open(&m->ent_route, &v))
		return false;
	*max_part_gen = v.h.max_part_gen;
	return true;
}

/* Every (entity, home) of a route, appended to *out (grown as needed) */
static void
view_expand(const RouteView *v, SiftEntHome **out, uint32 *n, uint32 *cap)
{
	for (uint32 i = 0; i < v->h.nentities; i++)
	{
		int64		homes[16],
				   *hp = homes;
		uint32		cnt = view_homes(v, i, homes, 0);

		if (cnt > lengthof(homes))
			hp = palloc(sizeof(int64) * cnt);
		(void) view_homes(v, i, hp, cnt);
		if (*n + cnt > *cap)
		{
			*cap = Max(*cap * 2, *n + cnt);
			*out = *out == NULL ? palloc(sizeof(SiftEntHome) * *cap) : repalloc(*out, sizeof(SiftEntHome) * *cap);
		}
		for (uint32 q = 0; q < cnt; q++)
			(*out)[(*n)++] = (SiftEntHome) {get_i64(v->ids, i), hp[q]};
		if (hp != homes)
			pfree(hp);
	}
}

void
sift_ent_route_merge(const SiftScopeSection *a, const SiftScopeSection *b, uint64 max_part_gen, StringInfo out)
{
	SiftEntHome *all = NULL;
	uint32		n = 0,
				cap = 0;
	RouteView	v;

	if (view_open(a, &v))
		view_expand(&v, &all, &n, &cap);
	if (view_open(b, &v))
		view_expand(&v, &all, &n, &cap);
	sift_ent_route_write(out, all, n, max_part_gen);
	if (all != NULL)
		pfree(all);
}
