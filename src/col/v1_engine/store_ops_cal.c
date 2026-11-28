/*
 * col/v1_engine/store_ops_cal.c - a key of SiftAggV6 computed from a date or timestamp leaf, from the groups of a Format 1 unit (col/v1_engine/store_ops_cal.h).
 *
 * The key is the derivation the scan evaluates for its rows (agg/expr/pg.h), or the bin of a date_trunc('month') or date_bin key (agg/win/datebin.h), computed here for one row of each
 * group: its value as the leaf stores it (days or microseconds since 2000-01-01), or the first day of its cell (the UTC midnight of a timestamp or timestamptz leaf). A bin keeps its value over
 * the cells it is a union of: a month over days and months, a date_bin of whole days from a midnight over days.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/expr/pg.h"
#include "agg/qual/grain.h"
#include "agg/win/datebin_pg.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/store_ops.h"
#include "col/v1_engine/store_ops_cal.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

struct SiftV1CalKey
{
	SiftV6PgExpr *x;			/* a derivation; NULL: a bin */
	SiftDateBin bin;
	Oid			typid;			/* of the leaf */
	int			cells;			/* the cells the key keeps its value over (SIFT_V6_CELL_*) */
	int64	   *in;				/* the leaf's value of each row */
	uint64		valid[SIFT_VEC_WORDS];
	SiftVxOut	out;
};

struct SiftV1CalTest
{
	SiftV6PgExpr *x;
	Oid			typid;			/* of the leaf */
	int			cells;
};

/* The cells of grain (SIFT_CAL_*) are among cells (SIFT_V6_CELL_*) */
static bool
grain_in(int cells, int grain)
{
	switch (grain)
	{
		case SIFT_CAL_DAY:
			return (cells & SIFT_V6_CELL_DAY) != 0;
		case SIFT_CAL_MONTH:
			return (cells & SIFT_V6_CELL_MONTH) != 0;
		default:
			return (cells & SIFT_V6_CELL_YEAR) != 0;
	}
}

/* The value of the leaf of type typid for group r of h: its value, or the first instant of its cell */
static int64
group_value(Oid typid, const SiftSrcHist *h, uint32 r)
{
	return h->grain == 0 || typid == DATEOID ? h->key[r] : h->key[r] * USECS_PER_DAY;
}

static bool
null_group(const SiftSrcHist *h, uint32 r)
{
	return h->has_null && r == h->ngroups - 1;
}

static const SiftV6Bin *
bin_of(const SiftV6Spec *spec, int key)
{
	for (int b = 0; b < spec->nbins; b++)
		if (spec->bin[b].key == key)
			return &spec->bin[b];
	return NULL;
}

/* The cells a bin is a union of; 0 for a bin that raises for every row */
static int
bin_cells(const SiftV6Bin *b)
{
	if (b->status != SDB_OK)
		return 0;
	if (b->stride_us == SIFT_DATEBIN_MONTH)
		return SIFT_V6_CELL_DAY | SIFT_V6_CELL_MONTH;
	return b->stride_us % USECS_PER_DAY == 0 && b->origin % USECS_PER_DAY == 0 ? SIFT_V6_CELL_DAY : 0;
}

/* The leaf a key reads and the cells it keeps its value over, with the evaluator of a derivation in *x; NULL for any other key */
static const SiftSrcLeaf *
key_leaf(MemoryContext cx, const SiftV6Spec *spec, const SiftSrcLayout *lay, int *cells, SiftV6PgExpr **x)
{
	const SiftV6Col *key = &spec->key[0];
	const SiftV6Bin *bin = bin_of(spec, 0);
	const SiftV6Derive *d = key->derive > 0 ? &spec->derive[key->derive - 1] : NULL;
	const SiftSrcLeaf *l;
	SiftV6PgItem it;

	if (key->src != SV6SRC_ATTR || key->jleaf > 0)
		return NULL;
	if (bin != NULL)
	{
		*cells = bin_cells(bin);
		return d == NULL ? sift_v1_att_leaf(lay, key->attno) : NULL;
	}
	if (d == NULL || d->kind != SVD_PG_EXPR)
		return NULL;
	sift_v6_pgexpr_item(spec, d, &it);
	l = it.nin == 1 && it.col[0].src == SV6SRC_ATTR ? sift_v1_att_leaf(lay, it.col[0].attno) : NULL;
	if (l == NULL || (*cells = sift_v6_calendar_grains((const Node *) it.expr, SIFT_V6_PGX_RELID)) == 0)
		return NULL;
	*x = sift_v6_pgexpr_of(cx, spec, d, &it, &(SiftV6PgIn) {.typid = it.col[0].typid,.scale = it.col[0].scale});
	return l;
}

SiftV1CalKey *
sift_v1_cal_key_open(MemoryContext cx, const SiftV6Spec *spec, const SiftSrcLayout *lay, int *leaf)
{
	const SiftV6Bin *bin = bin_of(spec, 0);
	SiftV6PgExpr *x = NULL;
	int			cells = 0;
	const SiftSrcLeaf *l = key_leaf(cx, spec, lay, &cells, &x);
	SiftV1CalKey *k;

	if (!sift_v1_leaf_int(l) || cells == 0)
		return NULL;
	k = MemoryContextAllocZero(cx, sizeof(SiftV1CalKey));
	k->x = x;
	if (bin != NULL)
		k->bin = (SiftDateBin) {.stride_us = bin->stride_us,.origin = bin->origin,.stride_status = bin->status};
	k->typid = l->typid;
	k->cells = cells;
	k->in = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	sift_vx_out_init(&k->out, cx);
	*leaf = l->id;
	return k;
}

bool
sift_v1_cal_key_fits(const SiftV1CalKey *k, const SiftSrcHist *h)
{
	return h->grain == 0 || grain_in(k->cells, h->grain);
}

void
sift_v1_cal_key_put(SiftV1CalKey *k, const SiftSrcHist *h, uint32 r, int row)
{
	if (null_group(h, r))
		return;
	k->valid[row >> 6] |= UINT64CONST(1) << (row & 63);
	k->in[row] = group_value(k->typid, h, r);
}

/* The bins of the rows of bs, the error PostgreSQL raises for one of them raised */
static void
bin_vec(SiftV1CalKey *k, const SiftBatchSel *bs, const SiftVec *in)
{
	uint32		err_row;

	if (k->bin.stride_us == SIFT_DATEBIN_MONTH)
		sift_datemonth_batch(bs, in, k->out.data);
	else
		sift_datebin_raise(sift_datebin_batch(&k->bin, bs, in, k->out.data, &err_row));
	memcpy(k->out.valid, k->valid, sizeof(k->valid));
	k->out.vec = (SiftVec) {.data = k->out.data,.valid = k->out.valid,.len = bs->n_raw,.dense = true,.type = SVEC_I64};
}

SiftVec *
sift_v1_cal_key_vec(SiftV1CalKey *k, int n)
{
	SiftBatchSel bs = {.n_raw = (uint16) n,.n_active = (uint16) n,.all_active = true};
	const SiftVec in = {.data = k->in,.valid = k->valid,.len = (uint16) n,.dense = true,.type = SVEC_I64};
	const SiftVec *inp[1] = {&in};

	for (int i = 0; i < n; i++)
		bs.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	if (k->x != NULL)
		sift_v6_pgexpr_derive(k->x, inp, &bs, &k->out);
	else
		bin_vec(k, &bs, &in);
	memset(k->valid, 0, sizeof(k->valid));
	return &k->out.vec;
}

/* ---- tests ---- */

static bool
param_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan))
		return true;
	return expression_tree_walker(n, param_walker, context);
}

SiftV1CalTest *
sift_v1_cal_test_of(const Node *clause, const List *vars, Oid typid)
{
	Expr	   *e;
	SiftV1CalTest *t;
	int			cells;

	if (exprType(clause) != BOOLOID || contain_volatile_functions((Node *) clause) || param_walker((Node *) clause, NULL))
		return NULL;
	e = sift_v6_pgexpr_renumber((const Expr *) clause, 1, 0, vars);
	if ((cells = sift_v6_calendar_grains((const Node *) e, SIFT_V6_PGX_RELID)) == 0)
		return NULL;
	t = palloc(sizeof(SiftV1CalTest));
	*t = (SiftV1CalTest) {.x = sift_v6_pgexpr_open(CurrentMemoryContext, NULL, e, 1, &(SiftV6PgIn) {.typid = typid}),.typid = typid,.cells = cells};
	return t;
}

/* The test of the value v of the leaf (NULL: isnull) */
static bool
test_at(SiftV1CalTest *t, int64 v, bool isnull)
{
	SiftBatchSel bs = {.mask = {1},.n_raw = 1,.n_active = 1,.all_active = true};
	uint64		valid = isnull ? 0 : 1;
	const SiftVec in = {.data = &v,.valid = &valid,.len = 1,.dense = true,.type = SVEC_I64};
	const SiftVec *inp[1] = {&in};

	sift_v6_pgexpr_eval(t->x, inp, &bs);
	return !t->x->isnull[0] && DatumGetBool(t->x->vals[0]);
}

bool
sift_v1_cal_test_passes(SiftV1CalTest *t, const SiftSrcHist *h, uint32 r)
{
	SIFT_INVARIANT(h->grain == 0 || null_group(h, r), "a calendar test is made on a cell by its verdict, not as a group of values");
	return test_at(t, null_group(h, r) ? 0 : h->key[r], null_group(h, r));
}

SiftV1Verdict
sift_v1_cal_test_cell(SiftV1CalTest *t, const SiftSrcHist *h, uint32 r)
{
	if (!grain_in(t->cells, h->grain))
		return SV1V_SOME;
	return test_at(t, group_value(t->typid, h, r), false) ? SV1V_ALL : SV1V_NONE;
}
