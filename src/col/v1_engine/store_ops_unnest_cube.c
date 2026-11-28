/*
 * col/v1_engine/store_ops_unnest_cube.c - the aggregate scan of a LATERAL unnest of a Format 1 index (store_ops_unnest.c) answering units from the rollup of the elements of its array
 * (col/v1/preagg/preagg.h) instead of reading them.
 *
 * The source offers each unit it is about to read. A unit is answered when its rollup of the array has every element column the statement groups, tests or filters by among its dimensions
 * (agg/unnest/cells.h), the counts, sums, least and greatest of the aggregates among its measures, and the unit stands for exactly the rows the snapshot sees (SiftV1ReaderOps.unit_rows): the
 * tuples go through the derivations, the quals of the element, the FILTER clauses and the sets of keys as the pairs of the run do, when no column of the elements the statement declares holds a
 * value it cannot. A condition on the row is one the zones of a unit decide: a unit they say every row passes is answered. Any other unit is read as it was.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement or the tuples of one unit, not the rows of the table */
#include "col/v1_engine/store_ops_unnest.h"

#include "agg/group/int.h"
#include "agg/unnest/cells.h"
#include "col/v1_engine/store_ops_preagg_ext.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

#define ROWS_ONLY	(-1)
#define DIM_COUNT(d)	(-2 - (d))	/* the count of the elements whose dimension d is not NULL */

typedef struct UnCube
{
	SiftV1UnitSink sink;
	UnRun	   *u;
	int			list;			/* the leaf of the array */
	SiftV1ZoneTests *rows;		/* the conditions on the row, NULL: none */
	uint64		dims;			/* bit c: element column c is a dimension the statement reads */
	MemoryContext cx;			/* the strings of a unit */
	/* the unit in hand */
	int			cdim[SIFT_V6_XELEM_COLS];	/* of each element column of dims the dimension of the rollup */
	int		   *meas;			/* of each aggregate the measure of the rollup it reads, ROWS_ONLY or DIM_COUNT */
	V6Acc	   *acc;
	SiftVec		ev[SIFT_V6_XELEM_COLS];	/* the element columns of a batch of tuples */
	uint64		keep[SIFT_VEC_WORDS + 1];
} UnCube;

static bool
fn_ok(SiftV6ElemFn fn)
{
	return fn == SV6E_RECORDSET || fn == SV6E_ELEMS_TEXT || fn == SV6E_MEMBERS;
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

/* Where each dimension the statement reads and each aggregate are in r: a column of text a dimension of strings, one of integers a dimension of integers */
static bool
bind_rollup(UnCube *c, const SiftSrcRollup *r)
{
	const SiftV6Spec *spec = c->u->spec;

	for (int e = 0; e < c->u->nel; e++)
		if ((c->dims >> e) & 1)
		{
			const SiftVecType t = c->ev[e].type;

			if ((c->cdim[e] = leaf_dim(r, c->u->x->elem.col_leaf[e])) < 0 || (t != SVEC_STRVIEW && t != SVEC_I64) || (r->dict[c->cdim[e]] != NULL) != (t == SVEC_STRVIEW))
				return false;
		}
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

/* The values of the dimensions of tuple t as row n of the element columns */
static void
tuple_dims(UnCube *c, const SiftSrcRollup *r, uint32 t, uint32 n)
{
	for (int e = 0; e < c->u->nel; e++)
		if ((c->dims >> e) & 1)
		{
			const int	d = c->cdim[e];

			if (null_at(r, d, t))
				continue;
			if (r->dict[d] != NULL)
				sift_v1_strview(c->cx, &((SiftStrView *) c->ev[e].data)[n], r->dict[d], (uint32) r->key[d][t]);
			else
				((int64 *) c->ev[e].data)[n] = r->key[d][t];
			c->ev[e].valid[n >> 6] |= UINT64CONST(1) << (n & 63);
		}
}

static void
batch_reset(UnCube *c)
{
	for (int e = 0; e < c->u->nel; e++)
		memset(c->ev[e].valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
}

/* The n tuples in the element columns and the states, through the quals of the element, on to the groups */
static void
emit_batch(UnCube *c, uint32 n)
{
	memset(c->keep, 0, sizeof(c->keep));
	if (c->u->eev != NULL)
		sift_v6_elem_eval(c->u->eev, c->ev, 0, n, NULL, c->keep);
	else
		for (uint32 i = 0; i < n; i++)
			c->keep[i >> 6] |= UINT64CONST(1) << (i & 63);
	for (int e = 0; e < c->u->nel; e++)
		c->ev[e].len = (uint16) n;
	sift_v1_unnest_emit_cells(c->u, c->ev, c->keep, c->acc, n);
}

/* The tuples of the rollup that hold elements, a batch at a time */
static void
fold(UnCube *c, const SiftSrcRollup *r)
{
	const int	naggs = c->u->spec->naggs;
	uint32		n = 0;

	batch_reset(c);
	for (uint32 t = 0; t < r->ntuples; t++)
	{
		if (r->count[t] == 0)
			continue;
		tuple_dims(c, r, t, n);
		for (int j = 0; j < naggs; j++)
			tuple_acc(c, r, t, j, &c->acc[n * naggs + j]);
		if (++n == SIFT_VEC_LEN)
		{
			emit_batch(c, n);
			batch_reset(c);
			n = 0;
		}
	}
	if (n > 0)
		emit_batch(c, n);
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
	fold(c, &r);
	MemoryContextSwitchTo(old);
	c->u->stats->grans_preagg++;
	return true;
}

SiftV1UnitSink *
sift_v1_unnest_cube_begin(UnRun *u)
{
	UnCube	   *c;
	uint64		dims;

	if (!u->has_elems || !fn_ok(u->x->elem.fn) || !sift_v6_elem_cells_ok(u->spec, u->eev, &dims))
		return NULL;
	c = palloc0(sizeof(UnCube));
	*c = (UnCube) {.sink = {.offer = offer,.arg = c},.u = u,.list = u->espec.group_leaf,.dims = dims};
	if (u->x->heap_where != NIL && (c->rows = sift_v1_zone_tests(u->index, sift_v1_reader_of(false)->layout(u->index), u->x->heap_where)) == NULL)
		return NULL;
	c->meas = palloc(u->spec->naggs * sizeof(int));
	c->acc = palloc(SIFT_VEC_LEN * u->spec->naggs * sizeof(V6Acc));
	for (int e = 0; e < u->nel; e++)
		c->ev[e] = (SiftVec) {.data = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * Max(sizeof(SiftStrView), sizeof(int64)) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0),
		.valid = palloc0((SIFT_VEC_WORDS + 1) * sizeof(uint64)),.dense = true,.type = u->etype[e]};
	c->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 unnest rollup");
	return &c->sink;
}
