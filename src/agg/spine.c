/*
 * agg/spine.c - the spine of a two level aggregate: its rows, and the rows each cell of the arms joins
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "utils/memutils.h"

#include "agg/agg_int.h"

/* the group of the spine rows whose join values are ids, or PG_UINT32_MAX */
static uint32
spine_group(const SpineSt *sp, int npair, const uint32 *ids)
{
	for (uint32 h = ids_hash(ids, npair, sp->nslot); sp->gslot[h]; h = (h + 1) & (sp->nslot - 1))
		if (memcmp(&sp->jid[(Size) sp->grep[sp->gslot[h] - 1] * npair], ids, npair * sizeof(uint32)) == 0)
			return sp->gslot[h] - 1;
	return PG_UINT32_MAX;
}

/* Groups the rows by their join values, so that the cells of an arm find the rows they join by a lookup, not by a pass over the spine. */
static void
spine_index(SpineSt *sp, int npair)
{
	uint32	   *rgrp = palloc(Max(sp->nrow, 1) * sizeof(uint32)),
			   *pos;

	sp->nslot = 16;
	while (sp->nslot < sp->nrow * 2)
		sp->nslot <<= 1;
	sp->gslot = palloc0(sp->nslot * sizeof(uint32));
	sp->grep = palloc(Max(sp->nrow, 1) * sizeof(uint32));
	sp->gfirst = palloc0((sp->nrow + 2) * sizeof(uint32));
	sp->grow = palloc(Max(sp->nrow, 1) * sizeof(uint32));
	for (uint32 r = 0; r < sp->nrow; r++)
	{
		const uint32 *ids = &sp->jid[(Size) r * npair];
		uint32		g = spine_group(sp, npair, ids),
					h;

		if (g == PG_UINT32_MAX)
		{
			g = sp->ngrp++;
			sp->grep[g] = r;
			for (h = ids_hash(ids, npair, sp->nslot); sp->gslot[h]; h = (h + 1) & (sp->nslot - 1))
				;
			sp->gslot[h] = g + 1;
		}
		rgrp[r] = g;
		sp->gfirst[g + 1]++;
	}
	for (uint32 g = 0; g < sp->ngrp; g++)
		sp->gfirst[g + 1] += sp->gfirst[g];
	pos = palloc(Max(sp->ngrp, 1) * sizeof(uint32));
	memcpy(pos, sp->gfirst, sp->ngrp * sizeof(uint32));
	for (uint32 r = 0; r < sp->nrow; r++)
		sp->grow[pos[rgrp[r]]++] = r;
	pfree(pos);
	pfree(rgrp);
}

/* The rows of the spine's plan, once when the scan starts: their join values and the ids of the columns the query groups by. */
void
spine_load(AggExec *ex)
{
	const SiftAggSpine *sa = ex->spec->spine;
	PlanState  *child = list_nth(ex->css.custom_ps, sa->child);
	MemoryContext caller = MemoryContextSwitchTo(ex->cxt);
	SpineSt    *sp = palloc0(sizeof(SpineSt));
	uint32		cap = 64,
				kid[SIFT_AGG_MAX_JKEYS];

	ex->spine = sp;
	for (int p = 0; p < sa->npair; p++)
	{
		const Const *k = (const Const *) sa->pair[p].konst;

		ot_init(&sp->jot[p], sa->pair[p].eqop, sa->pair[p].typid, sa->pair[p].collid);
		kid[p] = k == NULL || k->constisnull ? 0 : ot_id(&sp->jot[p], k->constvalue);
	}
	sp->jid = palloc(cap * sa->npair * sizeof(uint32));
	sp->oid = palloc(cap * Max(sa->nout, 1) * sizeof(uint32));
	for (;;)
	{
		TupleTableSlot *slot;

		MemoryContextSwitchTo(caller);
		slot = ExecProcNode(child);
		MemoryContextSwitchTo(ex->cxt);
		if (TupIsNull(slot))
			break;
		slot_getallattrs(slot);
		if (sp->nrow == cap)
		{
			cap *= 2;
			sp->jid = repalloc(sp->jid, cap * sa->npair * sizeof(uint32));
			sp->oid = repalloc(sp->oid, cap * Max(sa->nout, 1) * sizeof(uint32));
		}
		for (int p = 0; p < sa->npair; p++)
		{
			int			col = sa->pair[p].col;

			sp->jid[(Size) sp->nrow * sa->npair + p] = col < 0 ? kid[p] : slot->tts_isnull[col] ? 0 : ot_id(&sp->jot[p], slot->tts_values[col]);
		}
		for (int o = 0; o < sa->nout; o++)
		{
			int			col = sa->outcol[o];
			uint32		id = 0;

			if (!slot->tts_isnull[col])
			{
				MemoryContextSwitchTo(ex->tcxt);
				id = ot_id(&ex->ks[sa->outkey[o]].ot, slot->tts_values[col]);
				MemoryContextSwitchTo(ex->cxt);
			}
			sp->oid[(Size) sp->nrow * sa->nout + o] = id;
		}
		sp->nrow++;
	}
	sp->hit = palloc0(Max(sp->nrow, 1));
	spine_index(sp, sa->npair);
	MemoryContextSwitchTo(caller);
	/* the ids of the spine's columns are all known: the groups of the direct table have room for them */
	for (int o = 0; !ex->hash && o < sa->nout; o++)
		if (key_used(&ex->ks[sa->outkey[o]]) > ex->ks[sa->outkey[o]].cap)
		{
			direct_grow(ex);
			break;
		}
}

static void
memo_grow(SpineMemo *m, int npair)
{
	m->nslot = m->nslot ? m->nslot * 2 : 64;
	if (m->slot)
		pfree(m->slot);
	m->slot = MemoryContextAllocZero(CurrentMemoryContext, m->nslot * sizeof(uint32));
	for (uint32 e = 0; e < m->n; e++)
	{
		uint32		h = ids_hash(&m->ids[(Size) e * npair], npair, m->nslot);

		while (m->slot[h])
			h = (h + 1) & (m->nslot - 1);
		m->slot[h] = e + 1;
	}
}

/*
 * The rows of the spine that the cells of arm k whose join keys have the ids `ids` (one per pair of the join) are joined to; a key that came
 * before is in the memo of the arm. A NULL matches NULL in a null-safe pair and nothing in the others.
 */
uint32
spine_rows(AggExec *ex, int k, const uint32 *ids, const uint32 **rows)
{
	const SiftAggSpine *sa = ex->spec->spine;
	SpineSt    *sp = ex->spine;
	SpineMemo  *m = &sp->memo[k];
	MemoryContext caller;
	uint32		h,
				e,
				g,
				want[SIFT_AGG_MAX_JKEYS];
	bool		dead = false;

	if (m->nslot == 0)
	{
		caller = MemoryContextSwitchTo(ex->cxt);
		memo_grow(m, sa->npair);
		MemoryContextSwitchTo(caller);
	}
	for (h = ids_hash(ids, sa->npair, m->nslot); m->slot[h]; h = (h + 1) & (m->nslot - 1))
		if (memcmp(&m->ids[(Size) (m->slot[h] - 1) * sa->npair], ids, sa->npair * sizeof(uint32)) == 0)
		{
			e = m->slot[h] - 1;
			*rows = sp->grow + m->first[e];
			return m->count[e];
		}
	caller = MemoryContextSwitchTo(ex->cxt);
	for (int p = 0; p < sa->npair; p++)
	{
		bool		isnull;
		Datum		v = key_id_datum(ex, sa->pair[p].key[k], ids[p], &isnull);

		dead |= isnull && !sa->pair[p].nullsafe;
		want[p] = isnull ? 0 : ot_id(&sp->jot[p], v);
	}
	if (m->n == m->cap)
	{
		m->cap = m->cap ? m->cap * 2 : 16;
		m->ids = m->ids ? repalloc(m->ids, (Size) m->cap * sa->npair * sizeof(uint32)) : palloc((Size) m->cap * sa->npair * sizeof(uint32));
		m->first = m->first ? repalloc(m->first, m->cap * sizeof(uint32)) : palloc(m->cap * sizeof(uint32));
		m->count = m->count ? repalloc(m->count, m->cap * sizeof(uint32)) : palloc(m->cap * sizeof(uint32));
	}
	e = m->n++;
	memcpy(&m->ids[(Size) e * sa->npair], ids, sa->npair * sizeof(uint32));
	g = dead ? PG_UINT32_MAX : spine_group(sp, sa->npair, want);
	m->first[e] = g == PG_UINT32_MAX ? 0 : sp->gfirst[g];
	m->count[e] = g == PG_UINT32_MAX ? 0 : sp->gfirst[g + 1] - sp->gfirst[g];
	if (m->n * 2 > m->nslot)
		memo_grow(m, sa->npair);
	else
	{
		for (h = ids_hash(ids, sa->npair, m->nslot); m->slot[h]; h = (h + 1) & (m->nslot - 1))
			;
		m->slot[h] = e + 1;
	}
	MemoryContextSwitchTo(caller);
	*rows = sp->grow + m->first[e];
	return m->count[e];
}
