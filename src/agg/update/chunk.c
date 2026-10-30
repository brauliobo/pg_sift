/*
 * agg/update/chunk.c - the update of the aggregates of a chunk: staging and joins
 * The tuples a chunk stages for the semi and the lookup joins, the join stage and the entry
 * of a dense chunk (agg_chunk).
 */
#include "postgres.h"

#include "agg/dense_int.h"
#include "col/am.h"
#include "agg/update/int.h"

/* the stage's tuples: the outputs of the earlier joins are copied only by a stage that repeats or drops tuples, so a chain of one-row lookups holds one array per stage */
Tup *
stage_tuples(AggExec *ex, int s)
{
	Tup		   *own = &ex->stage[s];

	if (s > 0 && own->cout[ex->jcombo[0]] == NULL)
		for (int y = 0; y < s; y++)
			own->cout[ex->jcombo[y]] = MemoryContextAlloc(ex->cxt, (VEC + PADW) * sizeof(uint32));
	return own;
}

static void
semi_stage(AggExec *ex, int s, Tup *in)
{
	int			ci = ex->jcombo[s];
	ComboSt    *cs = &ex->cs[ci];
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const bool	want = ex->spec->join[cb->join].kind != SJ_ANTI;
	const bool	pass = cs->spass;
	const SiftSrcVec *v = pass ? NULL : get_vec(ex, cb->leaf[0]);
	const uint32 *idx = leaf_lvl(ex, cb->leaf[0]) == ex->qlevel ? in->tidx : in->ridx;
	const int32 *x = pass ? NULL : (const int32 *) v->data;
	Tup		   *own = stage_tuples(ex, s);
	int			k = 0;
	int			i;

#define SEMI_HIT(i) (pass || (!(v->isnull && v->isnull[idx[i]]) && (uint64) ((int64) x[idx[i]] - cs->slo) < cs->snlut && cs->slut[(int64) x[idx[i]] - cs->slo]))
	/* a request that names every key of the table (a wide request) keeps every tuple: they go on as they are, nothing is copied */
	for (i = 0; i < in->n && SEMI_HIT(i) == want; i++)
		;
	if (i == in->n)
	{
		Tup			al = *in;

		memset(own->cout[ci], 0, in->n * sizeof(uint32));
		al.cout = memcpy(ex->alcout[s], in->cout, ex->spec->ncombo * sizeof(uint32 *));
		al.cout[ci] = own->cout[ci];
		join_stage(ex, s + 1, &al);
		return;
	}
	for (i = 0; i < in->n; i++)
	{
		if (SEMI_HIT(i) != want)
			continue;
		own->sel[k] = in->sel[i];
		own->tidx[k] = in->tidx[i];
		own->ridx[k] = in->ridx[i];
		for (int y = 0; y < s; y++)
			own->cout[ex->jcombo[y]][k] = in->cout[ex->jcombo[y]][i];
		own->cout[ci][k] = 0;
		k++;
	}
	own->n = k;
#undef SEMI_HIT
	if (k > 0)
		join_stage(ex, s + 1, own);
}

/* applies the lookup joins one at a time: a tuple is repeated per match of its key, and dropped (INNER) or kept once (LEFT) without one */
void
join_stage(AggExec *ex, int s, Tup *in)
{
	int			ci,
				n = in->n;
	ComboSt    *cs;
	Tup		   *own;
	uint32	   *eid;
	bool		plain = true;

	if (s == ex->nj)
	{
		agg_tuples(ex, in);
		return;
	}
	ci = ex->jcombo[s];
	cs = &ex->cs[ci];
	if (cs->map->keep && ex->spec->nprepost > 0)
	{
		in->n = n = post_filter(ex, in, 0, ex->spec->nprepost, s, -1);
		if (n == 0)
			return;
	}
	if (cs->slut || cs->spass)
	{
		semi_stage(ex, s, in);
		return;
	}
	own = &ex->stage[s];
	eid = ex->peid;
	combo_resolve(ex, ci, ex->qlevel, n, in->tidx, in->ridx, eid, NULL, in);
	for (int i = 0; i < n; i++)
		plain &= cs->ent[eid[i]].nout == 1;
	if (plain)
	{
		Tup			al = *in;

		for (int i = 0; i < n; i++)
			own->cout[ci][i] = cs->ent[eid[i]].base;
		al.cout = memcpy(ex->alcout[s], in->cout, ex->spec->ncombo * sizeof(uint32 *));
		al.cout[ci] = own->cout[ci];
		join_stage(ex, s + 1, &al);
		return;
	}
	/* the entries of this stage's tuples stay in eid only until the next stage resolves its own: keep a copy */
	own = stage_tuples(ex, s);
	eid = memcpy(ex->seid[s], eid, n * sizeof(uint32));
	for (int j = 0, e = 0; j < n;)
	{
		int			k = 0;

		for (; j < n && k < VEC; k++)
		{
			const ComboEnt *en = &cs->ent[eid[j]];

			if (e >= en->nout)
			{
				j++;
				e = 0;
				k--;
				continue;
			}
			own->sel[k] = in->sel[j];
			own->tidx[k] = in->tidx[j];
			own->ridx[k] = in->ridx[j];
			for (int x = 0; x < s; x++)
				own->cout[ex->jcombo[x]][k] = in->cout[ex->jcombo[x]][j];
			own->cout[ci][k] = en->base + e;
			if (++e >= en->nout)
			{
				j++;
				e = 0;
			}
		}
		own->n = k;
		if (k > 0)
			join_stage(ex, s + 1, own);
	}
}

/*
 * The row of every tuple of t (ridx), the tuples and the rows both ascending: one walk over the offsets, from the row the last chunk ended in. A chunk that
 * keeps most of its elements takes the owners of the unit, which one pass makes for all of them; a few kept ones do not pay for those of the others.
 */
static void
tuple_rows(AggExec *ex, Tup *t, int n)
{
	if (ex->parent != NULL || (uint64) t->n * 3 >= n)
	{
		const uint32 *par = unit_parent(ex);

		for (int j = 0; j < t->n; j++)
			t->ridx[j] = par[t->tidx[j]];
	}
	else
	{
		const uint32 *end = ex->offs + 1;
		int			r = ex->prow;

		if (t->tidx[0] < ex->offs[r])
			r = 0;
		for (int j = 0; j < t->n; j++)
		{
			while (end[r] <= t->tidx[j])
				r++;
			t->ridx[j] = r;
		}
		ex->prow = r;
	}
}

/* one chunk of tuples [start, start + n) selected by mask */
void
agg_chunk(AggExec *ex, int start, int n, const uint64 *mask)
{
	Tup		   *t = &ex->t0;
	int			nw = (n + 63) / 64;

	combo_trim(ex);
	if (ex->dense && !ex->hash && ex->rsel == NULL && !ex->nullpass && dense_chunk(ex, start, n, mask))
		return;
	t->n = sift_colk->bits_to_idx(mask, nw, 0, t->sel);
	if (t->n == 0)
		return;
	ex->cstart = start;
	ex->cn = n;
	for (int j = 0; j < t->n; j++)
		t->tidx[j] = start + t->sel[j];
	if (ex->spec->group >= 0 && !ex->nullpass)
		tuple_rows(ex, t, n);
	else
		memcpy(t->ridx, t->tidx, t->n * sizeof(uint32));
	join_stage(ex, 0, t);
}
