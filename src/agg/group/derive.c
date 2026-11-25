/*
 * agg/group/derive.c - the derived keys and arguments of a batch (SiftV6Spec.derive) for the groups of SiftAggV6. An argument that only aggregates with a FILTER (WHERE ..) read is computed for the
 * rows the clauses of those aggregates keep, as PostgreSQL computes it: a row the FILTER leaves out never raises the error of its argument (sum(id / g) FILTER (WHERE g <> 0)). The columns the
 * clauses, the keys or an aggregate with no FILTER read are computed first, for every active row, so that the clauses can be evaluated.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/group/int.h"
#include "agg/expr/num.h"

static void
mark_open(bool *open, const SiftV6Col *c)
{
	if (c->derive > 0)
		open[c->derive - 1] = true;
}

static void
mark_clause(SiftV6Group *g, const SiftV6Col *c, int fclause)
{
	if (c->derive > 0)
		g->dclause[c->derive - 1] |= UINT64CONST(1) << (fclause - 1);
}

/* dclause[k]: the clauses whose rows the derived column k is computed for, 0 when every active row needs it (a key, a column of a clause, a prefix of another derivation or an aggregate with no FILTER reads it) */
void
v6g_derive_init(SiftV6Group *g)
{
	const SiftV6Spec *spec = g->spec;
	bool		open[SIFT_V6_MAX_DERIVE] = {false};

	for (int i = 0; i < spec->nkeys; i++)
		mark_open(open, &spec->key[i]);
	for (int i = 0; i < spec->nfcols; i++)
		mark_open(open, &spec->fcol[i]);
	for (int k = 0; k < spec->nderive; k++)
		for (int j = 0; j < k; j++)
			open[j] |= sift_vn_prefix_of(&spec->derive[j], &spec->derive[k]);
	for (int j = 0; j < spec->naggs; j++)
	{
		const SiftV6Agg *a = &spec->agg[j];

		if (a->fclause == 0)
		{
			mark_open(open, &a->arg);
			mark_open(open, &a->arg2);
			for (int i = 0; i < a->nxcols; i++)
				mark_open(open, &a->xcol[i]);
			continue;
		}
		mark_clause(g, &a->arg, a->fclause);
		mark_clause(g, &a->arg2, a->fclause);
		for (int i = 0; i < a->nxcols; i++)
			mark_clause(g, &a->xcol[i], a->fclause);
	}
	for (int k = 0; k < spec->nderive; k++)
		if (open[k])
			g->dclause[k] = 0;
}

/* the active rows of bs that some clause of clauses keeps */
static void
rows_of_clauses(const SiftV6Group *g, uint64 clauses, const SiftBatchSel *bs, SiftBatchSel *out)
{
	const int	words = (bs->n_raw + 63) / 64;

	memset(out->mask, 0, sizeof(out->mask));
	for (uint64 c = clauses; c != 0; c &= c - 1)
	{
		const uint64 *bits = sift_fmasks_bits(g->fmasks, pg_rightmost_one_pos64(c));

		for (int w = 0; w < words; w++)
			out->mask[w] |= bits[w] & bs->mask[w];
	}
	out->n_active = 0;
	for (int w = 0; w < words; w++)
		out->n_active += pg_popcount64(out->mask[w]);
	out->n_raw = bs->n_raw;
	out->all_active = out->n_active == out->n_raw;
	out->has_sel = false;
}

void
sift_v6_derive_batch(SiftV6Group *g, int nderive, const SiftBatchSel *bs, SiftVec *const *argvec, SiftV6DeriveFn derive, void *arg)
{
	MemoryContext old;
	bool		later = false;

	for (int k = 0; k < nderive; k++)
		if (g != NULL && g->dclause[k] != 0)
			later = true;
		else
			derive(arg, k, bs);
	if (!later)
		return;
	old = MemoryContextSwitchTo(g->cx);
	v6g_filter_eval(g, bs, argvec);
	MemoryContextSwitchTo(old);
	for (int k = 0; k < nderive; k++)
		if (g->dclause[k] != 0)
		{
			SiftBatchSel rows;

			rows_of_clauses(g, g->dclause[k], bs, &rows);
			derive(arg, k, &rows);
		}
}
