/*
 * agg/update/tuples.c - the tuple lists of a chunk
 * The post filter, the move of a tuple and the lists of the tuples of a grouping set.
 */
#include "postgres.h"

#include "agg/vnum.h"
#include "agg/update/int.h"

/* does conjunct i of the joined rows apply to set `set` (-1: the conjuncts of every set) */
static bool
post_applies(const SiftAggSpec *s, int i, int set)
{
	uint32		sets = s->postsets ? s->postsets[i] : 0;

	return set < 0 ? sets == 0 : (sets >> set & 1) != 0;
}

/* the tuple passes conjunct p over the joined rows: a boolean slot of its combo, or a row of the join */
static bool
post_holds(AggExec *ex, const SiftAggPred *p, const Tup *t, int j)
{
	const ComboSt *cs = &ex->cs[p->leaf];

	if (p->kind == SAP_ELEM_EXISTS)
		return elem_exists(ex, t, j, p);
	if (p->kind == SAP_MATCHED)
		return cs->maprow[t->cout[p->leaf][j]] != UINT32_MAX;
	/* a combo of the scanned table's columns only (a WHERE after a preserved join) has no join: its rows are resolved for the tuples */
	if (ex->spec->combo[p->leaf].join < 0)
		return combo_val(ex, p->leaf, combo_outs(ex, p->leaf)[j], p->op) != 0;
	return cs->val[(size_t) t->cout[p->leaf][j] * ex->spec->combo[p->leaf].nslots + p->op] != 0;
}

static void
tup_move(AggExec *ex, Tup *t, int to, int from, int stages)
{
	t->sel[to] = t->sel[from];
	t->tidx[to] = t->tidx[from];
	t->ridx[to] = t->ridx[from];
	for (int x = 0; x < stages; x++)
		t->cout[ex->jcombo[x]][to] = t->cout[ex->jcombo[x]][from];
}

/* the conjuncts over the joined rows [first, end) that apply to the set: the tuples that pass, moved to the front; the new count */
int
post_filter(AggExec *ex, Tup *t, int first, int end, int stages, int set)
{
	int			k = 0;

	for (int j = 0; j < t->n; j++)
	{
		bool		keep = true;

		for (int i = first; keep && i < end; i++)
			keep = !post_applies(ex->spec, i, set) || post_holds(ex, ex->spec->post[i], t, j);
		if (!keep)
			continue;
		tup_move(ex, t, k++, j, stages);
	}
	return k;
}

/* the conjuncts of the set's arm that the pass does not apply (the rows of its chunk, as an aggregate's FILTER): the tuples that pass, moved to the front; the new count */
static int
set_filter(AggExec *ex, Tup *t, const SiftAggPred *p)
{
	uint64		pass[VECW + PADW];
	int			k = 0;

	eval_pred(ex, (SiftAggPred *) p, ex->qlevel, ex->cstart, ex->cn, pass);
	for (int j = 0; j < t->n; j++)
		if (sift_colsrc_bit(pass, t->sel[j]))
			tup_move(ex, t, k++, j, ex->nj);
	return k;
}

/* the tuples of a set that has conjuncts of its own: a copy of the tuples (the other sets read them whole) that passes them */
static Tup *
set_tuples(AggExec *ex, const Tup *t, int set)
{
	Tup		   *own = ex->set_tup;

	if (own == NULL)
	{
		own = ex->set_tup = MemoryContextAllocZero(ex->cxt, sizeof(Tup));
		own->sel = MemoryContextAlloc(ex->cxt, (VEC + PADW + 16) * sizeof(uint32));
		own->tidx = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
		own->ridx = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
		own->cout = MemoryContextAllocZero(ex->cxt, ex->spec->ncombo * sizeof(uint32 *));
		for (int x = 0; x < ex->nj; x++)
			own->cout[ex->jcombo[x]] = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
	}
	own->n = t->n;
	memcpy(own->sel, t->sel, t->n * sizeof(uint32));
	memcpy(own->tidx, t->tidx, t->n * sizeof(uint32));
	memcpy(own->ridx, t->ridx, t->n * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
		memcpy(own->cout[ex->jcombo[x]], t->cout[ex->jcombo[x]], t->n * sizeof(uint32));
	own->n = post_filter(ex, own, ex->spec->nprepost, ex->spec->npost, ex->nj, set);
	if (own->n > 0 && ex->spec->setpred && ex->spec->setpred[set])
		own->n = set_filter(ex, own, ex->spec->setpred[set]);
	return own;
}

bool
set_has_post(const SiftAggSpec *s, int set)
{
	if (s->setpred && s->setpred[set])
		return true;
	for (int i = s->nprepost; i < s->npost; i++)
		if (post_applies(s, i, set))
			return true;
	return false;
}

/* the tuples that passed the joins and the filters over the joined rows: their groups and aggregates */
void
agg_tuples(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;
	bool		shared;

	ex->cur = t;
	ex->sel = t->sel;
	ex->tidx = t->tidx;
	ex->ridx = t->ridx;
	memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
	if (s->npost > s->nprepost)
	{
		t->n = post_filter(ex, t, s->nprepost, s->npost, ex->nj, -1);
		/* the free combos were resolved for the tuples as they came */
		memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
	}
	if (t->n == 0)
		return;
	if (s->narms > 0)
	{
		arm_chunk(ex, t);
		return;
	}
	ex->n_tuples += t->n;
	shared = false;
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		Tup		   *st;

		if (ex->fold && set != ex->fold - 1)	/* the other sets are summed from this one at the end (sets_fold) */
			continue;
		st = s->nsets && set_has_post(s, set) ? set_tuples(ex, t, set) : t;
		if (st->n == 0)
			continue;
		/* the free combos are resolved for the tuples they were asked for: the set's own are not the chunk's */
		if (st != t || ex->cur != t)
			memset(ex->cfree_ok, 0, ex->spec->ncombo * sizeof(bool));
		ex->cur = st;
		ex->sel = st->sel;
		ex->tidx = st->tidx;
		ex->ridx = st->ridx;
		ex->curset = set;
		ex->arm_mask = s->nsets ? s->setmask[set] : ~(uint32) 0;
		if (s->nsets > 1 && ex->hash && st == t && !ex->fold)
		{
			if (!shared)
			{
				ex->arm_mask = ~(uint32) 0;
				compute_kids(ex, t->n, ex->setkid);
				ex->arm_mask = s->setmask[set];
				shared = true;
			}
			compute_set_gids(ex, st->n, ex->setkid);
		}
		else
			compute_gids(ex, st->n);
		ex->ncnt[0] = ex->seen;
		ex->nncnt = 1;
		vnum_chunk_reset(ex);
		for (int i = 0; i < s->naggs; i++)
			if (!s->setaggs || (s->setaggs[i] >> set & 1))
				agg_update(ex, i, ex->cstart, ex->cn, st->n);
		if (s->nkeys == 0)
			for (int k = 0; k < ex->nncnt; k++)
				ex->ncnt[k][0] += st->n;
		else
			sift_aggk_count_gid_n(ex->ncnt, ex->nncnt, ex->gid, NULL, st->n);
	}
	ex->arm_mask = ~(uint32) 0;
}
