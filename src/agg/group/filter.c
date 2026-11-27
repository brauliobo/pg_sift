/*
 * agg/group/filter.c - FILTER (WHERE ..) in the groups of SiftAggV6: the distinct clauses of a statement are evaluated once for a batch (v6_filter_eval.h), as bitmaps (v6_filter_pivot.h), and
 * every aggregate that carries one takes the rows of it. A set with no keys folds an aggregate over the bitmap; a set with keys reads the 64-bit mask of each row (bit f: the row passes clause f), the
 * transposition of the bitmaps of each 64 rows, so that the one load of a row tells every aggregate of the statement whether the row is its own. A statement of up to 64 pivot columns reads the table once.
 */
#include "postgres.h"

#include "agg/group/int.h"
#include "agg/filter/pg.h"
#include "core/popcount.h"

void
v6g_filter_init(SiftV6Group *g)
{
	const SiftV6Spec *spec = g->spec;

	g->nfilters = spec->nfilters;
	if (g->nfilters == 0)
		return;
	g->fmasks = sift_fmasks_create(g->cx, g->nfilters);
	g->clause = MemoryContextAlloc(g->cx, sizeof(SiftClause) * g->nfilters);
	for (int f = 0; f < g->nfilters; f++)
		if (!sift_v6_clause_bind(g->cx, list_nth(spec->filter, f), spec, &g->clause[f]))
			SIFT_INVARIANT(false, "the FILTER clause %d of the aggregate scan of relation %u is not compiled", f, spec->relid);
	v6g_derive_init(g);
}

typedef struct EvalCtx
{
	const SiftV6Group *g;
	SiftVec *const *vecs;
	uint16		n_raw;
} EvalCtx;

static void
eval_clause(void *arg, int f, uint64 *out)
{
	const EvalCtx *e = arg;

	sift_clause_eval(&e->g->clause[f], e->vecs, e->n_raw, out);
}

void
v6g_filter_eval(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *argvec)
{
	EvalCtx		e = {.g = g,.vecs = argvec + sift_v6_spec_fvec(g->spec),.n_raw = bs->n_raw};

	if (g->nfilters > 0)
		sift_fmasks_eval(g->fmasks, bs, eval_clause, &e);
}

const uint64 *
v6g_filter_take(const SiftV6Group *g, const SiftV6Agg *a, const SiftBatchSel *bs, uint16 *n)
{
	const uint64 *bits;
	uint32		count = 0;

	if (a->fclause == 0)
	{
		*n = bs->n_active;
		return bs->mask;
	}
	bits = sift_fmasks_bits(g->fmasks, a->fclause - 1);
	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		count += (uint32) sift_pc64(bits[w]);
	*n = (uint16) count;
	return bits;
}

static bool
agg_reads(const SiftV6Spec *s, int j, const Bitmapset *vecs)
{
	bool		reads = bms_is_member(j, vecs) || bms_is_member(s->naggs + j, vecs);

	for (int x = 0; x < s->agg[j].nxcols; x++)
		reads |= bms_is_member(sift_v6_agg_xvec(s, j) + x, vecs);
	return reads;
}

static bool
takes_any(const SiftV6Group *g, const SiftV6Agg *a, const SiftBatchSel *bs)
{
	uint16		n;
	const uint64 *bits = v6g_filter_take(g, a, bs, &n);

	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
		if ((bits[w] & bs->mask[w]) != 0)
			return true;
	return false;
}

bool
sift_v6_group_reads_any(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *argvec, const Bitmapset *vecs)
{
	const SiftV6Spec *s = g->spec;

	for (int v = sift_v6_spec_fvec(s); v < sift_v6_spec_nvec(s); v++)
		if (bms_is_member(v, vecs))
			return true;
	v6g_filter_eval(g, bs, argvec);
	for (int j = 0; j < s->naggs; j++)
		if (agg_reads(s, j, vecs) && takes_any(g, &s->agg[j], bs))
			return true;
	return false;
}

void
v6g_filter_free(SiftV6Group *g)
{
	if (g->fmasks != NULL)
		sift_fmasks_free(g->fmasks);
	g->fmasks = NULL;
}
