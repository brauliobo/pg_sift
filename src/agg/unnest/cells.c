/*
 * agg/unnest/cells.c - the statements of a LATERAL unnest the cells of a cube of the elements answer (agg/unnest/cells.h).
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/expr/pg.h"
#include "agg/filter/pg.h"
#include "agg/safety.h"
#include "agg/settings.h"
#include "agg/unnest/cells.h"

/* The aggregate counts the elements, or counts or reads a column of them as it is */
static bool
agg_ok(const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);

	if (a->nxcols != 0 || a->arg.derive > 0)
		return false;
	if (a->kind == SV6_COUNT_STAR)
		return true;
	if (a->arg.src != SV6SRC_ELEM)
		return false;
	if (a->kind == SV6_COUNT)
		return true;
	if (sift_agg_exact_float && cls == SV6C_F64 && (a->kind == SV6_SUM || a->kind == SV6_AVG))
		return false;
	return (a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX) && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64);
}

static bool derive_dims(const SiftV6Spec *spec, int k, uint64 *dims);

/* The column is an element column, or a derivation of element columns: those are dimensions */
static bool
col_dims(const SiftV6Spec *spec, const SiftV6Col *c, uint64 *dims)
{
	if (c->derive > 0)
		return derive_dims(spec, c->derive - 1, dims);
	if (c->src != SV6SRC_ELEM)
		return false;
	*dims |= UINT64CONST(1) << c->slot;
	return true;
}

static bool
derive_dims(const SiftV6Spec *spec, int k, uint64 *dims)
{
	const SiftV6Derive *d = &spec->derive[k];
	SiftV6PgItem it;

	if (d->kind != SVD_PG_EXPR)
	{
		for (int c = 0; c < d->ncond; c++)
			if (!col_dims(spec, &d->cond[c], dims))
				return false;
		return col_dims(spec, &d->src, dims);
	}
	sift_v6_pgexpr_item(spec, d, &it);
	for (int c = 0; c < it.nin; c++)
		if (!col_dims(spec, &it.col[c], dims))
			return false;
	return true;
}

/* Bit i: the clause of some FILTER (WHERE ..) reads fcol[i]; the others are columns of the WHERE of the rows */
static uint64
filter_cols(const SiftV6Spec *spec)
{
	uint64		cols = 0;
	SiftClause	cl;

	for (int f = 0; f < spec->nfilters; f++)
	{
		const bool	bound = sift_v6_clause_bind(CurrentMemoryContext, list_nth(spec->filter, f), spec, &cl);

		SIFT_INVARIANT(bound, "the FILTER clause %d of the aggregate scan of relation %u is not compiled", f, spec->relid);
		for (int p = 0; p < cl.npred; p++)
			cols |= UINT64CONST(1) << cl.pred[p].vec;
	}
	return cols;
}

static bool
spec_ok(const SiftV6Spec *spec)
{
	const SiftV6Ext *x = spec->ext;

	if (spec->nbins != 0 || spec->nfuncs != 0 || spec->text_queries != NIL || spec->jsonqual != NIL || spec->vqual != NIL || spec->genref != NIL || spec->constant_groups ||
		spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1 || spec->is_table || spec->tid_range || spec->indexqual != NIL)
		return false;
	if (x == NULL || !x->unnest || x->ndims != 0 || x->nrcol != 0 || x->ntests != 0 || x->resid != NIL || x->jexpr != NIL || x->elem.left || x->elem.semi)
		return false;
	for (int c = 0; c < x->elem.ncols; c++)
		if (x->elem.col_kind[c])
			return false;
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_ok(&spec->agg[j]))
			return false;
	return true;
}

/* A qual of the element or a derivation PostgreSQL evaluates may raise for some value: on the cells it would meet the values in another order than on the elements, and raise another error */
static bool
may_raise(const SiftV6Spec *spec)
{
	foreach_ptr(Node, q, spec->ext->elem_where)
		if (agg_node_may_raise(IsA(q, List) ? linitial((List *) q) : q))
			return true;
	for (int k = 0; k < spec->nderive; k++)
	{
		SiftV6PgItem it;

		if (spec->derive[k].kind != SVD_PG_EXPR)
			continue;
		sift_v6_pgexpr_item(spec, &spec->derive[k], &it);
		if (agg_node_may_raise((Node *) it.expr))
			return true;
	}
	return false;
}

bool
sift_v6_elem_cells_ok(const SiftV6Spec *spec, const SiftV6ElemEval *ee, uint64 *dims)
{
	*dims = ee != NULL ? sift_v6_elem_eval_cols(ee) : 0;
	if (!spec_ok(spec) || may_raise(spec))
		return false;
	for (int i = 0; i < spec->nkeys; i++)
		if (!col_dims(spec, &spec->key[i], dims))
			return false;
	for (uint64 f = filter_cols(spec); f != 0; f &= f - 1)
		if (!col_dims(spec, &spec->fcol[pg_rightmost_one_pos64(f)], dims))
			return false;
	for (int k = 0; k < spec->nderive; k++)
		if (!derive_dims(spec, k, dims))
			return false;
	return true;
}
