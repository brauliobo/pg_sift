/*
 * agg/combo/ents.c - combos: the cache of the combinations
 * The hash and the table of the entries of a combo, the entry of a combination, and the
 * reset and trim of the caches.
 */
#include "postgres.h"

#include "agg/combo/int.h"

/* the ids are small and dense: every input is multiplied in and folded, or the entries of a few hundred thousand combinations pile up in a few slots */
static inline uint32
combo_hash(const uint64 *v, int ninputs, bool null_input)
{
	uint64		h = UINT64CONST(0x9E3779B97F4A7C15);

	for (int p = 0; p < ninputs; p++)
	{
		h = (h ^ v[p]) * UINT64CONST(0xD6E8FEB86659FD93);
		h ^= h >> 32;
	}
	h *= UINT64CONST(0x9E3779B97F4A7C15);
	return (uint32) (h >> 32) ^ (null_input ? 0x85EBCA6BU : 0);
}

/* the few inputs of an entry compared inline: a memcmp call per probe costs more than the loop */
static inline bool
combo_same(const uint64 *a, const uint64 *b, int n)
{
	for (int p = 0; p < n; p++)
		if (a[p] != b[p])
			return false;
	return true;
}

/* room for one more entry */
static void
combo_grow_ent(ComboSt *cs)
{
	if (cs->nent < cs->entcap)
		return;
	cs->entcap = Max(cs->entcap * 2, 64);
	cs->vid = cs->vid ? repalloc(cs->vid, (size_t) cs->entcap * cs->ninputs * sizeof(uint64)) :
		MemoryContextAlloc(cs->cxt, (size_t) cs->entcap * cs->ninputs * sizeof(uint64));
	cs->ent = cs->ent ? repalloc(cs->ent, (size_t) cs->entcap * sizeof(ComboEnt)) :
		MemoryContextAlloc(cs->cxt, (size_t) cs->entcap * sizeof(ComboEnt));
	cs->null_input = cs->null_input ? repalloc(cs->null_input, cs->entcap * sizeof(bool)) :
		MemoryContextAlloc(cs->cxt, cs->entcap * sizeof(bool));
}

/* the entry of a value id of a by_id combo: the entries up to it exist (ids come in order from 1, 0 is the NULL) */
static uint32
combo_ent_by_id(AggExec *ex, int ci, uint32 id)
{
	ComboSt    *cs = &ex->cs[ci];

	while (cs->nent <= id)
	{
		uint64	   *v;

		combo_grow_ent(cs);
		v = &cs->vid[(size_t) cs->nent * cs->ninputs];
		memset(v, 0, cs->ninputs * sizeof(uint64));
		v[0] = cs->nent;
		cs->null_input[cs->nent] = false;
		combo_make(ex, ci, v, &cs->ent[cs->nent]);
		cs->nent++;
	}
	return id;
}

/* entry e (1..) is the one of id k: the table grows to hold k */
static void
combo_dir_set(ComboSt *cs, uint64 k, uint32 e)
{
	if (k >= cs->dircap)
	{
		uint32		cap = Max(cs->dircap, 1024);

		while (cap <= k)
			cap *= 2;
		cs->dir = cs->dir ? repalloc(cs->dir, (size_t) cap * sizeof(uint32)) : MemoryContextAlloc(cs->cxt, (size_t) cap * sizeof(uint32));
		memset(cs->dir + cs->dircap, 0, (size_t) (cap - cs->dircap) * sizeof(uint32));
		cs->dircap = cap;
	}
	cs->dir[k] = e;
}

/* the entry of a tuple of leaf values, made on its first sight */
uint32
combo_ent(AggExec *ex, int ci, const uint64 *v)
{
	ComboSt    *cs = &ex->cs[ci];
	uint32		h,
				e;
	bool		direct;

	if (cs->by_id)
		return combo_ent_by_id(ex, ci, (uint32) v[0]);
	/* one input with a small id (a dictionary code, a small integer): the entry is an array index, no hash */
	direct = cs->ninputs == 1 && !combo_null_input(ex) && v[0] < COMBO_DIR_MAX;
	if (direct && v[0] < cs->dircap && cs->dir[v[0]])
		return cs->dir[v[0]] - 1;
	h = combo_hash(v, cs->ninputs, combo_null_input(ex)) & (cs->nht - 1);
	for (; (e = cs->ht[h]) != 0; h = (h + 1) & (cs->nht - 1))
		if (cs->null_input[e - 1] == combo_null_input(ex) &&
			combo_same(&cs->vid[(size_t) (e - 1) * cs->ninputs], v, cs->ninputs))
			return e - 1;
	combo_grow_ent(cs);
	memcpy(&cs->vid[(size_t) cs->nent * cs->ninputs], v, cs->ninputs * sizeof(uint64));
	cs->null_input[cs->nent] = combo_null_input(ex);
	combo_make(ex, ci, v, &cs->ent[cs->nent]);
	cs->ht[h] = ++cs->nent;
	if (direct)
		combo_dir_set(cs, v[0], cs->nent);
	if (cs->nent * 2 > cs->nht)
	{
		pfree(cs->ht);
		cs->nht *= 2;
		cs->ht = MemoryContextAllocZero(cs->cxt, cs->nht * sizeof(uint32));
		for (uint32 x = 0; x < cs->nent; x++)
		{
			h = combo_hash(&cs->vid[(size_t) x * cs->ninputs], cs->ninputs, cs->null_input[x]) & (cs->nht - 1);
			while (cs->ht[h])
				h = (h + 1) & (cs->nht - 1);
			cs->ht[h] = x + 1;
		}
	}
	return cs->nent - 1;
}

/* the output rows of the earlier lookups start again (table_reset): so do the ids of their values, which name those rows */
void
combo_forget_priors(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];

	for (int p = 0; p < cb->nleaves; p++)
	{
		if (cb->leaf[p] >= 0)
			continue;
		if (cs->pv[p].memo)
			pfree(cs->pv[p].memo);
		if (cs->pv[p].rep)
			pfree(cs->pv[p].rep);
		if (cs->id64[p].tab)
		{
			pfree(cs->id64[p].tab);
			pfree(cs->id64[p].val);
		}
		memset(&cs->pv[p], 0, sizeof(cs->pv[p]));
		memset(&cs->id64[p], 0, sizeof(cs->id64[p]));
	}
	if (cs->byrow)
		pfree(cs->byrow);
	cs->byrow = NULL;
	cs->nbyrow = 0;
}

/* the cache starts again: the entries, the outputs of the slots, the values and the ids of the leaves (nothing else holds them) */
void
combo_reset(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	SiftAggCombo *cb = &ex->spec->combo[ci];
	MemoryContext old;

	MemoryContextReset(cs->cxt);
	cs->nent = cs->entcap = cs->nout = cs->outcap = cs->ndat = cs->datcap = 0;
	cs->vid = NULL;
	cs->null_input = NULL;
	cs->ent = NULL;
	cs->val = NULL;
	cs->maprow = NULL;
	cs->dat = NULL;
	cs->dnum = NULL;
	cs->arena = NULL;
	cs->arena_left = 0;
	cs->nht = 64;
	cs->dir = NULL;
	cs->dircap = 0;
	cs->byrow = NULL;
	cs->nbyrow = 0;
	old = MemoryContextSwitchTo(cs->cxt);
	cs->ht = palloc0(cs->nht * sizeof(uint32));
	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0)
		{
			/* the output rows of the earlier lookup start again with it (combo_trim): so do the ids of its values */
			memset(&cs->pv[p], 0, sizeof(cs->pv[p]));
			memset(&cs->id64[p], 0, sizeof(cs->id64[p]));
		}
		else if (leaf_wide(&ex->lay->leaf[cb->leaf[p]]))
			id64_init(&cs->id64[p]);
	MemoryContextSwitchTo(old);
}

/*
 * The caches are a memo, not a table the answer needs: once one holds COMBO_MAX_ENTRIES entries, all of them start again before the next chunk.
 * A chunk runs through its joins to the aggregates before the next begins, so no output row of a combo outlives it, and the entries of a combo
 * keyed by the output rows of another go with it. A cache that was reset costs the evaluations it held, never an error.
 */
void
combo_trim(AggExec *ex)
{
	int64		max = sift_limit(combo_entries, COMBO_MAX_ENTRIES);
	bool		full = false;

	for (int ci = 0; ci < ex->spec->ncombo; ci++)
		full |= ex->cs[ci].nent >= max;
	if (!full)
		return;
	ex->n_combo_trim++;
	memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
		combo_reset(ex, ci);
}
