/*
 * agg/group/table.c - the group table: slots, growth and the hash of a group
 * The accumulators of the groups, the growth of the table, the direct and the hash lookup
 * of a group by its key ids.
 */
#include "postgres.h"

#include "agg/acc_layout.h"
#include "agg/group/int.h"

AggAcc *
acc_alloc(AggExec *ex, uint32 cap)
{
	AggAcc	   *a = MemoryContextAllocZero(ex->tcxt, Max(ex->spec->naggs, 1) * sizeof(AggAcc));

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		unsigned	layout = sift_agg_acc_layout(&ex->spec->agg[i], ex->lane_serial);

		a[i].n = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));
		if (layout & ACCL_I)
			a[i].i = MemoryContextAllocZero(ex->tcxt, cap * sizeof(int64));	/* SAA_GEN: the group's number among the sorted values */
		if (layout & ACCL_W)
			a[i].w = MemoryContextAllocZero(ex->tcxt, cap * sizeof(sift_i128));
		if (layout & ACCL_D)
			a[i].d = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
		if (layout & ACCL_DT)
			a[i].dt = MemoryContextAllocZero(ex->tcxt, cap * sizeof(double));
		if (layout & ACCL_NF)
			a[i].nf = MemoryContextAllocZero(ex->tcxt, cap);
		if (layout & ACCL_V)
			a[i].v = MemoryContextAllocZero(ex->tcxt, cap * sizeof(Datum));
		if (layout & ACCL_R)
			a[i].r = MemoryContextAllocZero(ex->tcxt, cap * sizeof(Datum));
		if (layout & ACCL_LAST)
		{
			a[i].last = MemoryContextAlloc(ex->tcxt, cap * sizeof(uint64));
			memset(a[i].last, 0xff, cap * sizeof(uint64));
		}
		if (ex->lanes && sift_aggkind_float_sum(&ex->spec->agg[i]))
			a[i].lv = MemoryContextAllocZero(ex->tcxt, SIFT_AGG_LANES * sizeof(double *));
	}
	return a;
}

static void
acc_free(AggAcc *a, int naggs)
{
	for (int i = 0; i < naggs; i++)
	{
		void	   *col[] = {a[i].n, a[i].i, a[i].w, a[i].d, a[i].v, a[i].r, a[i].last, a[i].nf, a[i].dt, a[i].lv};

		for (int l = 0; a[i].lv && l < SIFT_AGG_LANES; l++)
			if (a[i].lv[l])
				pfree(a[i].lv[l]);
		for (int c = 0; c < lengthof(col); c++)
			if (col[c])
				pfree(col[c]);
	}
	pfree(a);
}

/* every per-group array to newcap groups; remap[old] = new index (UINT32_MAX: drop), NULL: identity */
void
groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	AggAcc	   *na = acc_alloc(ex, newcap);
	int64	   *nseen = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));

	/* the groups keep their numbers: copy the arrays whole, an unseen slot holds what a new one does */
	if (remap == NULL)
	{
		memcpy(nseen, ex->seen, oldcap * sizeof(int64));
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			AggAcc	   *o = &ex->acc[i],
					   *n = &na[i];

			memcpy(n->n, o->n, oldcap * sizeof(int64));
			if (o->i)
				memcpy(n->i, o->i, oldcap * sizeof(int64));
			if (o->w)
				memcpy(n->w, o->w, oldcap * sizeof(sift_i128));
			if (o->d)
				memcpy(n->d, o->d, oldcap * sizeof(double));
			if (o->dt)
				memcpy(n->dt, o->dt, oldcap * sizeof(double));
			if (o->last)
				memcpy(n->last, o->last, oldcap * sizeof(uint64));
			if (o->nf)
				memcpy(n->nf, o->nf, oldcap * sizeof(uint8));
			if (o->v)
				memcpy(n->v, o->v, oldcap * sizeof(Datum));
			if (o->r)
				memcpy(n->r, o->r, oldcap * sizeof(Datum));
			for (int l = 0; o->lv && l < SIFT_AGG_LANES; l++)
				if (o->lv[l])
				{
					n->lv[l] = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
					memcpy(n->lv[l], o->lv[l], oldcap * sizeof(double));
				}
		}
	}
	for (uint32 g = 0; remap && g < oldcap; g++)
	{
		uint32		t = remap ? remap[g] : g;

		if (t == UINT32_MAX || ex->seen[g] == 0)
			continue;
		nseen[t] = ex->seen[g];
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			AggAcc	   *o = &ex->acc[i],
					   *n = &na[i];

			n->n[t] = o->n[g];
			if (o->i)
				n->i[t] = o->i[g];
			if (o->w)
				n->w[t] = o->w[g];
			if (o->d)
				n->d[t] = o->d[g];
			if (ex->spec->agg[i].kind == SAA_GEN)
			{
				n->v[t] = o->v[g];
				n->r[t] = o->r[g];
			}
			if (o->dt)
				n->dt[t] = o->dt[g];
			for (int l = 0; o->lv && l < SIFT_AGG_LANES; l++)
				if (o->lv[l])
				{
					if (n->lv[l] == NULL)
						n->lv[l] = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
					n->lv[l][t] = o->lv[l][g];
				}
			if (o->last)
				n->last[t] = o->last[g];
			if (o->nf)
				n->nf[t] = o->nf[g];
			if (o->v)
				n->v[t] = o->v[g];
		}
	}
	if (remap)
		for (int i = 0; i < ex->spec->naggs; i++)
		{
			if (ex->dset[i].slot)
				dset_grow(ex, &ex->dset[i], remap);
			if (ex->dist && ex->dist[i])
				dist_regroup(ex, ex->dist[i], remap, oldcap, newcap);
		}
	arms_resize(ex, newcap, remap, oldcap);
	acc_free(ex->acc, ex->spec->naggs);
	pfree(ex->seen);
	ex->gen++;
	ex->seen = nseen;
	ex->acc = na;
	ex->gcap = newcap;
}

void
direct_layout(AggExec *ex)
{
	uint64		m = 1;

	for (int j = 0; j < ex->spec->nkeys; j++)
	{
		ex->ks[j].mult = (uint32) m;
		m *= ex->ks[j].cap;
	}
}

/* a new group of an id tuple in hash mode, in slot `pos` of its probe */
pg_noinline uint32
hash_add(AggExec *ex, const uint32 *ids, uint32 pos)
{
	int			nk = ex->spec->nkeys;
	uint32		g = ex->ngroups++;

	if (g >= ex->hkcap)
	{
		ex->hkcap = Max(ex->hkcap * 2, 1024);
		ex->hkey = ex->hkey ? repalloc(ex->hkey, (Size) ex->hkcap * nk * sizeof(uint32)) :
			MemoryContextAlloc(ex->tcxt, (Size) ex->hkcap * nk * sizeof(uint32));
	}
	memcpy(&ex->hkey[(Size) g * nk], ids, nk * sizeof(uint32));
	ex->hslot[pos] = g + 1;
	if ((uint64) ex->ngroups * 2 > ex->nhslot)
	{
		uint32		nn = ex->nhslot * 2;
		uint32	   *ns = MemoryContextAllocZero(ex->tcxt, nn * sizeof(uint32));

		for (uint32 x = 0; x < ex->ngroups; x++)
		{
			uint32		p = ids_hash(&ex->hkey[(Size) x * nk], nk, nn);

			while (ns[p])
				p = (p + 1) & (nn - 1);
			ns[p] = x + 1;
		}
		pfree(ex->hslot);
		ex->hslot = ns;
		ex->nhslot = nn;
	}
	return g;
}

/* one probe function per number of keys (`ex->hashg`): the hash and the comparison of the ids are unrolled, as the hot loop wants */
#define HASH_GROUP_FN(n) \
	static uint32 hash_group_h##n(AggExec *ex, const uint32 *ids, uint32 h) { return hash_probe(ex, ids, n, h); } \
	static uint32 hash_group_##n(AggExec *ex, const uint32 *ids) { return hash_group_h##n(ex, ids, ids_hash32(ids, n)); }
HASH_GROUP_FN(1)
HASH_GROUP_FN(2)
HASH_GROUP_FN(3)
HASH_GROUP_FN(4)
HASH_GROUP_FN(5)
HASH_GROUP_FN(6)
HASH_GROUP_FN(7)
HASH_GROUP_FN(8)
HASH_GROUP_FN(9)
HASH_GROUP_FN(10)
HASH_GROUP_FN(11)
HASH_GROUP_FN(12)
HASH_GROUP_FN(13)
HASH_GROUP_FN(14)
HASH_GROUP_FN(15)
HASH_GROUP_FN(16)

static uint32 (*const hash_group_fns[]) (AggExec *, const uint32 *) =
{hash_group_1, hash_group_2, hash_group_3, hash_group_4, hash_group_5, hash_group_6, hash_group_7, hash_group_8,
 hash_group_9, hash_group_10, hash_group_11, hash_group_12, hash_group_13, hash_group_14, hash_group_15, hash_group_16};

/* Wider tuples use the same hash and equality with the plan's actual width. */
static uint32
hash_group_generic_h(AggExec *ex, const uint32 *ids, uint32 h)
{
	return hash_probe(ex, ids, ex->spec->nkeys, h);
}

static uint32
hash_group_generic(AggExec *ex, const uint32 *ids)
{
	return hash_group_generic_h(ex, ids, ids_hash32(ids, ex->spec->nkeys));
}

void
hash_init(AggExec *ex, uint32 groups)
{
	int         nk = ex->spec->nkeys;

	ex->hash = true;
	ex->hashg = nk <= lengthof(hash_group_fns) ? hash_group_fns[nk - 1] : hash_group_generic;
	ex->nhslot = groups > 1024 ? pg_nextpower2_32(groups * 2) : 1024;
	ex->hslot = MemoryContextAllocZero(ex->tcxt, ex->nhslot * sizeof(uint32));
	ex->ngroups = 0;
	if (groups > 1024)
	{
		ex->hkcap = groups;
		ex->hkey = MemoryContextAlloc(ex->tcxt, (Size) groups * nk * sizeof(uint32));
	}
}

/* the arrays of `total` slots are far more than the groups the planner expects: the capacity of the keys multiplies, the groups do not */
bool
direct_oversized(AggExec *ex, uint64 total)
{
	return total > PRESIZE_MIN && total > ex->spec->est_groups * 4;
}

/* the bytes a slot of the direct arrays takes: the arrays acc_alloc makes for a group, and the count of its rows */
static Size
direct_slot_bytes(const AggExec *ex)
{
	Size		bytes = sizeof(int64);

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		unsigned	l = sift_agg_acc_layout(&ex->spec->agg[i], ex->lane_serial);

		bytes += sizeof(int64) + ((l & ACCL_I) ? sizeof(int64) : 0) + ((l & ACCL_W) ? sizeof(sift_i128) : 0) + ((l & ACCL_D) ? sizeof(double) : 0) +
			((l & ACCL_DT) ? sizeof(double) : 0) + ((l & ACCL_NF) ? 1 : 0) + ((l & ACCL_V) ? sizeof(Datum) : 0) + ((l & ACCL_R) ? sizeof(Datum) : 0) +
			((l & ACCL_LAST) ? sizeof(uint64) : 0);
	}
	return bytes;
}

/*
 * The arrays of `total` slots hold more than three quarters of hash memory (the keys and the sets of the groups take the rest) of a scan that spills: they would be over the memory before a row is read, and so would a pass of the
 * scan for a share of the groups. The groups go to the hash table, which holds what they use.
 */
static bool
direct_past_memory(const AggExec *ex, uint64 total)
{
	return ex->spill != NULL && total * direct_slot_bytes(ex) > get_hash_memory_limit() / 4 * 3;
}

/* a key outgrew its capacity: more room per key, or the hash table when the arrays would be too big */
void
direct_grow(AggExec *ex)
{
	int			nk = ex->spec->nkeys;
	uint32		oldcap = ex->gcap,
				oldcaps[SIFT_AGG_MAX_KEYS],
				oldmult[SIFT_AGG_MAX_KEYS];
	uint64		total = 1;
	uint32	   *remap;

	for (int j = 0; j < nk; j++)
	{
		KeyState   *k = &ex->ks[j];

		oldcaps[j] = k->cap;
		oldmult[j] = k->mult;
		while (k->cap < key_used(k))
			k->cap *= 2;
		total = Min((uint64) ex->direct_max + 1, total * k->cap);
	}
	remap = palloc(oldcap * sizeof(uint32));
	if (total <= ex->direct_max && direct_oversized(ex, total))
	{
		uint32		used = 0;

		for (uint32 g = 0; g < oldcap; g++)
			used += ex->seen[g] != 0;
		if ((uint64) used * DIRECT_SPARSE < total)
			total = (uint64) ex->direct_max + 1;
	}
	if (total > ex->direct_max || direct_past_memory(ex, total))
	{
		/* the existing groups keep their ids in the hash table */
		hash_init(ex, 1024);
		for (uint32 g = 0; g < oldcap; g++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			if (ex->seen[g] == 0)
			{
				remap[g] = UINT32_MAX;
				continue;
			}
			for (int j = 0; j < nk; j++)
				ids[j] = (g / oldmult[j]) % oldcaps[j];
			remap[g] = hash_group(ex, ids);
		}
		groups_resize(ex, Max(ex->ngroups * 2, 1024), remap, oldcap);
	}
	else
	{
		bool		same = true;

		direct_layout(ex);
		/* only the last key grew: every group keeps its number */
		for (int j = 0; j < nk - 1; j++)
			same &= ex->ks[j].cap == oldcaps[j];
		for (uint32 g = 0; !same && g < oldcap; g++)
		{
			uint64		t = 0;

			for (int j = 0; j < nk; j++)
				t += (uint64) ((g / oldmult[j]) % oldcaps[j]) * ex->ks[j].mult;
			remap[g] = (uint32) t;
		}
		groups_resize(ex, (uint32) total, same ? NULL : remap, oldcap);
	}
	pfree(remap);
}

