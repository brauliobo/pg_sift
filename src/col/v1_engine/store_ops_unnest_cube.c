/*
 * col/v1_engine/store_ops_unnest_cube.c - the aggregate scan of a LATERAL unnest of a Format 1 index (store_ops_unnest.c) answering units from the rollup of the elements of its array
 * (col/v1/preagg/preagg.h) instead of reading them.
 *
 * The source offers each unit it is about to read. A unit is answered when its rollup of the array has every key of the statement and the column its tests of the element read among its
 * dimensions, the counts, sums, least and greatest of the aggregates among its measures, and the unit stands for exactly the rows the snapshot sees (SiftV1ReaderOps.unit_rows): the tuples that pass
 * the tests are folded into the groups, when no column of the elements the statement declares holds a value it cannot. A condition on the row is one the zones of a unit decide: a unit they say every row passes is answered. Any other unit is read as it was.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement or the tuples of one unit, not the rows of the table */
#include "col/v1_engine/store_ops_unnest.h"

#include "agg/group/int.h"
#include "col/v1_engine/store_ops_preagg_ext.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

#define ROWS_ONLY	(-1)
#define DIM_COUNT(d)	(-2 - (d))	/* the count of the elements whose dimension d is not NULL */

typedef struct UnCube
{
	SiftV1UnitSink sink;
	const UnRun *u;
	int			list;			/* the leaf of the array */
	SiftV1ZoneTests *rows;		/* the conditions on the row, NULL: none */
	int			tslot;			/* the column of the element the tests read, -1: none */
	SiftClause	ewhere;
	int			ecol[SIFT_V6_MAX_FCOLS];
	int			nevec;
	MemoryContext cx;			/* the strings of a unit */
	/* the unit in hand */
	int			kdim[SIFT_V6_MAX_KEYS];
	int			tdim;
	int		   *meas;			/* of each aggregate the measure of the rollup it reads, ROWS_ONLY or DIM_COUNT */
	bool	   *pass;
	V6Acc	   *acc;
	void	   *kdata[SIFT_V6_MAX_KEYS];
	uint64		kvalid[SIFT_V6_MAX_KEYS][SIFT_VEC_WORDS];
	SiftVec		kvec[SIFT_V6_MAX_KEYS];
	SiftVec    *kvp[SIFT_V6_MAX_KEYS];
	SiftStrView *tbuf;
	uint64		tvalid[SIFT_VEC_WORDS];
} UnCube;

static bool
agg_ok(const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);

	if (a->fclause != 0 || a->nxcols != 0 || a->arg.derive > 0)
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

static bool
key_ok(const SiftV6Col *k)
{
	const SiftV6Class cls = sift_v6_class(k->typid);

	return k->src == SV6SRC_ELEM && k->derive == 0 && (cls == SV6C_TEXT || cls == SV6C_I32 || cls == SV6C_I64);
}

/* The statement groups and aggregates the elements of an array of objects or strings the leaves hold */
static bool
spec_ok(const UnRun *u)
{
	const SiftV6Spec *spec = u->spec;
	const SiftV6Ext *x = u->x;

	if (spec->nsets != 0 || spec->nbins != 0 || spec->nderive != 0 || spec->nfilters != 0 || spec->nfuncs != 0 || spec->text_queries != NIL || spec->jsonqual != NIL ||
		spec->vqual != NIL || spec->pgexpr != NIL || spec->genref != NIL || spec->constant_groups || spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1 ||
		spec->is_table || spec->tid_range || spec->indexqual != NIL || !u->has_elems)
		return false;
	if (x->ndims != 0 || x->nrcol != 0 || x->ntests != 0 || x->resid != NIL || x->elem.left || x->elem.semi ||
		(x->elem.fn != SV6E_RECORDSET && x->elem.fn != SV6E_ELEMS_TEXT && x->elem.fn != SV6E_MEMBERS))
		return false;
	for (int c = 0; c < x->elem.ncols; c++)
		if (x->elem.col_kind[c])
			return false;
	for (int i = 0; i < spec->nkeys; i++)
		if (!key_ok(&spec->key[i]))
			return false;
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_ok(&spec->agg[j]))
			return false;
	return true;
}

/* ---- the rollup of a unit ---- */

static int
leaf_dim(const SiftSrcRollup *r, int leaf)
{
	for (int d = 0; d < r->ndims; d++)
		if (r->dim[d] == leaf)
			return d;
	return -1;
}

static int
leaf_meas(const SiftSrcRollup *r, int leaf)
{
	for (int m = 0; m < r->nmeas; m++)
		if (r->meas[m] == leaf)
			return m;
	return -1;
}

/* The measure of aggregate a in r; false when r does not keep what it reads */
static bool
bind_agg(UnCube *c, const SiftSrcRollup *r, const SiftV6Agg *a, int *meas)
{
	const int	leaf = a->kind == SV6_COUNT_STAR ? -1 : c->u->x->elem.col_leaf[a->arg.slot];
	const bool	flt = sift_v6_class(a->arg.typid) == SV6C_F64;
	const int	d = leaf >= 0 ? leaf_dim(r, leaf) : -1;
	const int	m = leaf >= 0 ? leaf_meas(r, leaf) : -1;

	*meas = a->kind == SV6_COUNT_STAR ? ROWS_ONLY : a->kind == SV6_COUNT && d >= 0 ? DIM_COUNT(d) : m;
	if (*meas < 0 || a->kind == SV6_COUNT)
		return *meas != -1 || a->kind == SV6_COUNT_STAR;
	if (a->kind == SV6_MIN || a->kind == SV6_MAX)
		return flt ? r->minf[m] != NULL && r->maxf[m] != NULL : r->min[m] != NULL && r->max[m] != NULL;
	return flt ? r->sumf[m] != NULL : r->sum[m] != NULL;
}

/* Where each key, the tested column and each aggregate are in r: a key of text a dimension of strings, one of integers a dimension of integers */
static bool
bind_rollup(UnCube *c, const SiftSrcRollup *r)
{
	const SiftV6Spec *spec = c->u->spec;
	const SiftV6XElem *e = &c->u->x->elem;

	for (int i = 0; i < spec->nkeys; i++)
		if ((c->kdim[i] = leaf_dim(r, e->col_leaf[spec->key[i].slot])) < 0 ||
			(r->dict[c->kdim[i]] != NULL) != (sift_v6_class(spec->key[i].typid) == SV6C_TEXT))
			return false;
	c->tdim = c->tslot >= 0 ? leaf_dim(r, e->col_leaf[c->tslot]) : -1;
	if (c->tslot >= 0 && (c->tdim < 0 || r->dict[c->tdim] == NULL))
		return false;
	for (int j = 0; j < spec->naggs; j++)
		if (!bind_agg(c, r, &spec->agg[j], &c->meas[j]))
			return false;
	return true;
}

static bool
null_at(const SiftSrcRollup *r, int d, uint32 t)
{
	return r->isnull[d] != NULL && r->isnull[d][t];
}

/* Whether each tuple passes the tests of the statement on the column of the element they read */
static void
tuples_pass(UnCube *c, const SiftSrcRollup *r)
{
	for (uint32 at = 0; at < r->ntuples; at += SIFT_VEC_LEN)
	{
		const uint32 cnt = Min(SIFT_VEC_LEN, r->ntuples - at);
		uint64		keep[SIFT_VEC_WORDS];
		SiftVec		in[SIFT_V6_MAX_FCOLS];
		SiftVec    *inp[SIFT_V6_MAX_FCOLS];

		if (c->tslot < 0)
		{
			memset(c->pass + at, true, cnt);
			continue;
		}
		memset(c->tvalid, 0, sizeof(c->tvalid));
		for (uint32 i = 0; i < cnt; i++)
		{
			const bool	null = null_at(r, c->tdim, at + i);

			c->tbuf[i] = (SiftStrView) {0};
			if (!null)
				sift_v1_strview(c->cx, &c->tbuf[i], r->dict[c->tdim], (uint32) r->key[c->tdim][at + i]);
			c->tvalid[i >> 6] |= (uint64) !null << (i & 63);
		}
		for (int v = 0; v < c->nevec; v++)
		{
			in[v] = (SiftVec) {.data = c->tbuf,.valid = c->tvalid,.len = (uint16) cnt,.dense = true,.type = SVEC_STRVIEW};
			inp[v] = &in[v];
		}
		sift_clause_eval(&c->ewhere, inp, cnt, keep);
		for (uint32 i = 0; i < cnt; i++)
			c->pass[at + i] = (keep[i >> 6] >> (i & 63)) & 1;
	}
}

static void
tuple_acc(const UnCube *c, const SiftSrcRollup *r, uint32 t, int j, V6Acc *out)
{
	const SiftV6Agg *a = &c->u->spec->agg[j];
	const int	m = c->meas[j];
	const bool	flt = sift_v6_class(a->arg.typid) == SV6C_F64;

	*out = (V6Acc) {0};
	if (m == ROWS_ONLY)
		out->n = r->count[t];
	else if (m < ROWS_ONLY)
		out->n = null_at(r, DIM_COUNT(0) - m, t) ? 0 : r->count[t];
	else if (a->kind == SV6_COUNT)
		out->n = r->nn[m][t];
	else if (a->kind == SV6_MIN || a->kind == SV6_MAX)
		sift_v1_extreme_acc(flt, a->kind == SV6_MIN, r->nn[m][t], flt ? 0 : r->min[m][t], flt ? 0 : r->max[m][t], flt ? r->minf[m][t] : 0, flt ? r->maxf[m][t] : 0, out);
	else
		sift_v1_sum_acc(flt, r->nn[m][t], flt ? 0 : r->sum[m][t], flt ? r->sumf[m][t] : 0, out);
}

static void
keys_reset(UnCube *c, const SiftSrcRollup *r)
{
	for (int i = 0; i < c->u->spec->nkeys; i++)
	{
		const bool	text = r->dict[c->kdim[i]] != NULL;

		memset(c->kvalid[i], 0, sizeof(c->kvalid[i]));
		c->kvec[i] = (SiftVec) {.data = c->kdata[i],.valid = c->kvalid[i],.dense = true,.type = text ? SVEC_STRVIEW : SVEC_I64};
		c->kvp[i] = &c->kvec[i];
	}
}

static void
keys_put(UnCube *c, const SiftSrcRollup *r, uint32 t, uint16 n)
{
	for (int i = 0; i < c->u->spec->nkeys; i++)
	{
		const int	d = c->kdim[i];
		const bool	null = null_at(r, d, t);

		if (r->dict[d] != NULL)
		{
			((SiftStrView *) c->kdata[i])[n] = (SiftStrView) {0};
			if (!null)
				sift_v1_strview(c->cx, &((SiftStrView *) c->kdata[i])[n], r->dict[d], (uint32) r->key[d][t]);
		}
		else
			((int64 *) c->kdata[i])[n] = null ? 0 : r->key[d][t];
		c->kvalid[i][n >> 6] |= (uint64) !null << (n & 63);
	}
}

static void
merge(UnCube *c, const SiftSrcRollup *r, uint16 n)
{
	for (int i = 0; i < c->u->spec->nkeys; i++)
		c->kvec[i].len = n;
	sift_v6_group_merge_cells(c->u->pipe->group, c->u->spec->nkeys > 0 ? c->kvp : NULL, c->acc, n);
	keys_reset(c, r);
}

/* The tuples that pass folded into the groups, a batch of keys at a time */
static void
fold(UnCube *c, const SiftSrcRollup *r)
{
	const int	naggs = c->u->spec->naggs;
	uint16		n = 0;

	keys_reset(c, r);
	for (uint32 t = 0; t < r->ntuples; t++)
	{
		if (r->count[t] == 0 || !c->pass[t])
			continue;
		for (int j = 0; j < naggs; j++)
			tuple_acc(c, r, t, j, &c->acc[n * naggs + j]);
		keys_put(c, r, t, n);
		if (++n == SIFT_VEC_LEN)
		{
			merge(c, r, n);
			n = 0;
		}
	}
	if (n > 0)
		merge(c, r, n);
}

/* Every column of the elements the statement declares holds its values in the unit as it reads them: one it cannot hold (a string where it is a float8) raises in PostgreSQL's function */
static bool
columns_exact(const UnCube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	const SiftV6XElem *e = &c->u->x->elem;
	SiftSrcUnitAgg z;

	for (int col = 0; col < e->ncols; col++)
		if (!rd->unit_agg(src, unit, e->col_leaf[col], &z))
			return false;
	return true;
}

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	UnCube	   *c = arg;
	SiftSrcRollup r;
	MemoryContext old;

	if (unit->nsu != 0 || rd->unit_rollup == NULL || !sift_v6_group_takes_cells(c->u->pipe->group) || rd->unit_rows(src, unit) < 0 || !columns_exact(c, rd, src, unit) ||
		(c->rows != NULL && !sift_v1_zone_tests_all(c->rows, rd, src, unit)) || !rd->unit_rollup(src, unit, c->list, NULL, &r) || !bind_rollup(c, &r))
		return false;
	MemoryContextReset(c->cx);
	old = MemoryContextSwitchTo(c->cx);
	c->pass = palloc(Max(r.ntuples, 1));
	tuples_pass(c, &r);
	fold(c, &r);
	MemoryContextSwitchTo(old);
	c->u->stats->grans_preagg++;
	return true;
}

SiftV1UnitSink *
sift_v1_unnest_cube_begin(const UnRun *u)
{
	UnCube	   *c;

	if (!spec_ok(u))
		return NULL;
	c = palloc0(sizeof(UnCube));
	*c = (UnCube) {.sink = {.offer = offer,.arg = c},.u = u,.list = u->espec.group_leaf,.tslot = -1};
	if (u->x->heap_where != NIL && (c->rows = sift_v1_zone_tests(u->index, sift_v1_reader_of(false)->layout(u->index), u->x->heap_where)) == NULL)
		return NULL;
	if (u->x->elem_where != NIL)
	{
		if (!sift_v6_unnest_elem_clause(CurrentMemoryContext, u->x->elem_where, &c->ewhere, c->ecol, &c->nevec))
			return NULL;
		for (int v = 0; v < c->nevec; v++)
		{
			if (c->tslot >= 0 && c->tslot != c->ecol[v])
				return NULL;
			c->tslot = c->ecol[v];
		}
	}
	c->meas = palloc(u->spec->naggs * sizeof(int));
	c->acc = palloc(SIFT_VEC_LEN * u->spec->naggs * sizeof(V6Acc));
	c->tbuf = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	for (int i = 0; i < u->spec->nkeys; i++)
		c->kdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * Max(sizeof(SiftStrView), sizeof(int64)) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	c->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 unnest rollup");
	return &c->sink;
}
