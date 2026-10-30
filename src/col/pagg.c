/*
 * col/pagg.c - the pre-aggregates of a unit through the column source (col/pagg.h, docs/COL-PREAGG.md).
 */
#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/ktab.h"
#include "col/pagg.h"

PG_FUNCTION_INFO_V1(sift_col_unit_aggs_sql);
PG_FUNCTION_INFO_V1(sift_col_unit_hists_sql);

bool
sift_col_unit_preagg_ok(const SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	return sift_col_unit_super(unit, &seg, &u) || (sift_col_unit_stored(unit, &seg, &u) && (sift_col_unit_exact(unit) || sift_col_unit_dead_on(unit)));
}

/* the leaf's counts and sums net of the dead rows; min and max only bound what is left */
static bool
agg_dead(const SiftColUnit *unit, int col, SiftCAgg *a)
{
	SiftCLeafAgg la;
	uint32		ndead,
				nnull;

	if (sift_col_unit_dead_bounds(unit))
		return true;
	if (!sift_col_unit_dead_leaf(unit, col, &ndead, &nnull, &la) || ndead > a->nvals || nnull > a->nnull)
		return false;
	a->nvals -= ndead;
	a->nnull -= nnull;
	a->sum -= la.sum;
	if (la.flags & SIFT_CAGG_SUMSQ_OVF)
		a->flags |= SIFT_CAGG_SUMSQ_OVF;
	else
		a->sumsq -= la.sumsq;
	a->flags |= SIFT_CAGG_LOOSE;
	return true;
}

bool
sift_col_unit_agg(const SiftColUnit *unit, int col, SiftCAgg *a)
{
	SiftColSegment *seg;
	uint32		u;

	if (sift_col_unit_super(unit, &seg, &u))
		return sift_cseg_super_agg(seg, col, u, a);
	if (unit->except >= 0 || !sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	sift_cseg_agg(seg, col, u, a);
	if (a->zflags & SIFT_COLZ_OVERFLOW)
		return false;
	return !sift_col_unit_dead_on(unit) || agg_dead(unit, col, a);
}

bool
sift_col_unit_presence(const SiftColUnit *unit, int col, SiftCPresence *p)
{
	SiftColSegment *seg;
	uint32		u;

	if (unit->except >= 0 || sift_col_unit_dead_on(unit) || sift_col_unit_super(unit, &seg, &u) || !sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	return sift_cseg_presence(seg, col, u, p);
}

bool
sift_col_unit_hll(const SiftColUnit *unit, int col, const uint8 **blob)
{
	SiftColSegment *seg;
	uint32		u,
				len;

	if (sift_col_unit_dead_on(unit) || sift_col_unit_super(unit, &seg, &u) || !sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	return (*blob = sift_cseg_hll(seg, col, u, &len)) != NULL;
}

static void
corrupt_cube(SiftColSegment *seg, uint32 u)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift index \"%s\" segment %llu: the cube of unit %u does not decode", RelationGetRelationName(seg->index),
					(unsigned long long) seg->entry.gen, u)));
}

/* ---------- exceptions (docs/COL-PREAGG.md 11) ---------- */

/* The exception leaf and cube bytes of unit u; NULL when it has none */
static const uint8 *
x_cube(SiftColSegment *seg, uint32 u, int *leaf, uint32 *len)
{
	uint32		n,
				i = 0;
	const uint8 *b;
	uint64		v = 0;
	int			shift = 0;

	if (seg->entry.format < SIFT_COL_FORMAT_EXCEPT || (b = sift_cseg_xcube(seg, u, &n)) == NULL)
		return NULL;
	do
	{
		if (i >= n || shift > 28)
			corrupt_cube(seg, u);
		v |= (uint64) (b[i] & 0x7f) << shift;
		shift += 7;
	} while (b[i++] & 0x80);
	if (v >= (uint64) seg->schema->ncols)
		corrupt_cube(seg, u);
	*leaf = (int) v;
	*len = n - i;
	return b + i;
}

bool
sift_col_unit_except(SiftColUnit *unit, int leaf)
{
	SiftColSegment *seg;
	uint32		u,
				len;
	int			xl;

	unit->except = -1;
	if (leaf < 0)
		return true;
	if (unit->nsu > 0 || !sift_col_unit_preagg_ok(unit) || !sift_col_unit_stored(unit, &seg, &u) || x_cube(seg, u, &xl, &len) == NULL || xl != leaf)
		return false;
	unit->except = leaf;
	return true;
}

/*
 * total - e for a float sum, when the cancellation costs at most 6 bits: the sums of a unit are in row order and the exceptions' are added on their own, so a
 * result far below the total (or a non-finite one) would carry the rounding of the total, not of the rows that remain
 */
static bool
float_net(double total, double e, uint32 left, double *out)
{
	double		d;

	if (left == 0)
	{
		*out = 0;
		return true;
	}
	if (!isfinite(total) || !isfinite(e))
		return false;
	d = total - e;
	if (d == 0 || fabs(d) * 64 < fabs(total))
		return false;
	*out = d;
	return true;
}

/*
 * h minus the groups of x (the rows where the exception leaf is not NULL), found by key; min and max are gone, and so is a measure x lacks or whose float sum would
 * cancel (a query that reads it scans the unit, the others do not mind). false: x has a group h lacks
 */
static bool
hist_subtract(SiftCHistDim *h, const SiftCHistDim *x)
{
	int			slot[SIFT_PAGG_MAX_MEAS];
	bool		bad[SIFT_PAGG_MAX_MEAS] = {false};
	uint32		nk = h->ngroups - h->has_null,
				xk = x->ngroups - x->has_null,
				g = 0;

	for (int m = 0; m < h->nmeas; m++)
		bad[m] = true;
	for (int m = 0; m < x->nmeas; m++)
	{
		slot[m] = 0;
		while (slot[m] < h->nmeas && h->meas[slot[m]] != x->meas[m])
			slot[m]++;
		if (slot[m] == h->nmeas)
			return false;
		bad[slot[m]] = false;
	}
	for (uint32 i = 0; i < x->ngroups; i++)
	{
		uint32		at;

		if (i < xk)
		{
			while (g < nk && h->key[g] < x->key[i])
				g++;
			if (g >= nk || h->key[g] != x->key[i])
				return false;
			at = g;
		}
		else if (h->has_null)
			at = h->ngroups - 1;
		else
			return false;
		if (x->count[i] > h->count[at])
			return false;
		h->count[at] -= x->count[i];
		for (int m = 0; m < x->nmeas; m++)
		{
			int			hm = slot[m];

			if (bad[hm])
				continue;
			if (x->nn[m][i] > h->nn[hm][at])
				return false;
			h->nn[hm][at] -= x->nn[m][i];
			if (h->sum[hm] && x->sum[m])
				h->sum[hm][at] -= x->sum[m][i];
			else if (h->sumf[hm] && x->sumf[m])
				bad[hm] = !float_net(h->sumf[hm][at], x->sumf[m][i], h->nn[hm][at], &h->sumf[hm][at]);
			else
				bad[hm] = true;
		}
	}
	h->minmax = false;
	{
		int			to = 0;

		for (int m = 0; m < h->nmeas; m++)
			if (!bad[m])
			{
				h->meas[to] = h->meas[m];
				h->nn[to] = h->nn[m];
				h->sum[to] = h->sum[m];
				h->sumf[to] = h->sumf[m];
				h->min[to] = h->max[to] = NULL;
				to++;
			}
		h->nmeas = to;
	}
	return true;
}

/* the order of tuple i of x against tuple t of r: the dims first to last, a NULL after the values */
static int
tuple_cmp(const SiftCRollup *r, uint32 t, const SiftCRollup *x, uint32 i)
{
	for (int j = 0; j < r->ndims; j++)
	{
		bool		rn = r->isnull[j] && r->isnull[j][t],
					xn = x->isnull[j] && x->isnull[j][i];

		if (rn != xn)
			return rn ? 1 : -1;
		if (!rn && r->key[j][t] != x->key[j][i])
			return r->key[j][t] < x->key[j][i] ? -1 : 1;
	}
	return 0;
}

/* the same for the tuples of a rollup, which both hold with all their dims */
static bool
rollup_subtract(SiftCRollup *r, const SiftCRollup *x)
{
	int			slot[SIFT_PAGG_MAX_MEAS];
	bool		bad[SIFT_PAGG_MAX_MEAS] = {false};
	uint32		t = 0;

	if (r->ndims != x->ndims)
		return false;
	for (int j = 0; j < r->ndims; j++)
		if (r->dim[j] != x->dim[j] || r->key[j] == NULL || x->key[j] == NULL)
			return false;
	for (int m = 0; m < r->nmeas; m++)
		bad[m] = true;
	for (int m = 0; m < x->nmeas; m++)
	{
		slot[m] = 0;
		while (slot[m] < r->nmeas && r->meas[slot[m]] != x->meas[m])
			slot[m]++;
		if (slot[m] == r->nmeas)
			return false;
		bad[slot[m]] = false;
	}
	for (uint32 i = 0; i < x->ntuples; i++)
	{
		while (t < r->ntuples && tuple_cmp(r, t, x, i) < 0)
			t++;
		if (t >= r->ntuples || tuple_cmp(r, t, x, i) != 0 || x->count[i] > r->count[t])
			return false;
		r->count[t] -= x->count[i];
		for (int m = 0; m < x->nmeas; m++)
		{
			int			rm = slot[m];

			if (bad[rm])
				continue;
			if (x->nn[m][i] > r->nn[rm][t])
				return false;
			r->nn[rm][t] -= x->nn[m][i];
			if (r->sum[rm] && x->sum[m])
				r->sum[rm][t] -= x->sum[m][i];
			else if (r->sumf[rm] && x->sumf[m])
				bad[rm] = !float_net(r->sumf[rm][t], x->sumf[m][i], r->nn[rm][t], &r->sumf[rm][t]);
			else
				bad[rm] = true;
		}
	}
	{
		int			to = 0;

		for (int m = 0; m < r->nmeas; m++)
			if (!bad[m])
			{
				r->meas[to] = r->meas[m];
				r->nn[to] = r->nn[m];
				r->sum[to] = r->sum[m];
				r->sumf[to] = r->sumf[m];
				r->min[to] = r->max[to] = NULL;
				r->minf[to] = r->maxf[to] = NULL;
				to++;
			}
		r->nmeas = to;
	}
	return true;
}

/* The exceptions' rollup of list in unit u, all dims and measures; false: the unit has none of that list */
static bool
x_rollup(SiftColSegment *seg, uint32 u, int list, MemoryContext mcxt, SiftCRollup *x)
{
	uint32		elen,
				len;
	const uint8 *e = sift_cseg_xecube(seg, u, &elen),
			   *blob;

	if (e == NULL)
		return false;
	blob = sift_cpagg_elem_cube(seg->schema, e, elen, list, &len);
	if (len == UINT32_MAX)
		corrupt_cube(seg, u);
	if (blob == NULL)
		return false;
	if (!sift_cpagg_rollup_decode(seg->schema, blob, len, mcxt, x))
		corrupt_cube(seg, u);
	return true;
}

/* The histogram of dim without the rows of the unit's exceptions */
static bool
hist_except(SiftColSegment *seg, uint32 u, int dim, MemoryContext mcxt, SiftCHistDim *h)
{
	SiftCHistDim x;
	SiftCRollup r;
	int			xl,
				found;
	uint32		len;
	const uint8 *blob;

	if (dim >= SIFT_PAGG_CAL || seg->schema->cols[dim].parent < 0)
	{
		if ((blob = x_cube(seg, u, &xl, &len)) == NULL || len == 0)
			return false;
		if ((found = sift_cpagg_decode_dim(seg->schema, blob, len, mcxt, dim, &x)) < 0)
			corrupt_cube(seg, u);
		return found > 0 && hist_subtract(h, &x);
	}
	return x_rollup(seg, u, seg->schema->cols[dim].parent, mcxt, &r) && sift_cpagg_rollup_dim(seg->schema, &r, dim, mcxt, &x) && hist_subtract(h, &x);
}

/* The histogram of dim of a stored unit: from the cube (SIFT_PAGG_CAL + a leaf: the calendar cells of a date or timestamp leaf), or the rollup of its list added up by dim. false: the unit has none */
static bool
seg_hist(SiftColSegment *seg, uint32 u, int dim, MemoryContext mcxt, SiftCHistDim *out)
{
	uint32		len;
	const uint8 *blob = dim >= SIFT_PAGG_CAL ? sift_cseg_cube(seg, u, &len) : sift_cseg_dim_cube(seg, u, dim, &len);
	SiftCRollup r;
	int			found;

	if (blob == NULL)
		return false;
	if (dim >= SIFT_PAGG_CAL || seg->schema->cols[dim].parent < 0)
	{
		if ((found = sift_cpagg_decode_dim(seg->schema, blob, len, mcxt, dim, out)) < 0)
			corrupt_cube(seg, u);
		return found > 0;
	}
	if (!sift_cpagg_rollup_decode(seg->schema, blob, len, mcxt, &r))
		corrupt_cube(seg, u);
	return sift_cpagg_rollup_dim(seg->schema, &r, dim, mcxt, out);
}

/* The histogram of dim of a superunit: from its merged cube (the dims of lists and the calendar cells are not merged) */
static bool
super_hist(SiftColSegment *seg, uint32 k, int dim, MemoryContext mcxt, SiftCHistDim *out)
{
	uint32		len;
	const uint8 *blob;
	int			found;

	if (dim >= SIFT_PAGG_CAL || seg->schema->cols[dim].parent >= 0 || (blob = sift_cseg_super_cube(seg, k, &len)) == NULL)
		return false;
	if ((found = sift_cpagg_decode_dim(seg->schema, blob, len, mcxt, dim, out)) < 0)
		corrupt_cube(seg, k);
	return found > 0;
}

bool
sift_col_unit_hist(const SiftColUnit *unit, int dim, MemoryContext mcxt, SiftCHistDim *out, SiftColDict *dict)
{
	SiftColSegment *seg;
	uint32		u;

	if (sift_col_unit_whole(unit, &seg))
	{
		const SiftCHistDim *d = dim < SIFT_PAGG_CAL ? sift_cseg_ktab(seg, dim) : NULL;

		if (d == NULL)
			return false;
		*out = *d;
	}
	else if (sift_col_unit_super(unit, &seg, &u))
	{
		if (!super_hist(seg, u, dim, mcxt, out))
			return false;
	}
	else
	{
		if (!sift_col_unit_preagg_ok(unit))
			return false;
		sift_col_unit_stored(unit, &seg, &u);
		if (!seg_hist(seg, u, dim, mcxt, out) || (unit->except >= 0 && !hist_except(seg, u, dim, mcxt, out)) ||
			(sift_col_unit_dead_on(unit) && !sift_col_unit_dead_hist(unit, out)))
			return false;
	}
	if (dict && dim < SIFT_PAGG_CAL && seg->schema->cols[dim].store == SIFT_COLS_STR)
		sift_cdict_view(sift_cseg_dict(seg, dim), seg->entry.gen << 8 | dim, dict);
	return true;
}

bool
sift_col_unit_rollup(const SiftColUnit *unit, int list, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r, SiftColDict *dict)
{
	SiftColSegment *seg;
	uint32		u,
				len;
	const uint8 *blob;

	if (!sift_col_unit_preagg_ok(unit) || sift_col_unit_super(unit, &seg, &u))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	if ((blob = sift_cseg_rollup(seg, u, list, &len)) == NULL)
		return false;
	if (!sift_cpagg_rollup_decode_need(seg->schema, blob, len, mcxt, unit->except >= 0 || sift_col_unit_dead_on(unit) ? NULL : need, r) || r->list != list)
		corrupt_cube(seg, u);
	if (list < 0)
	{
		/* the tuples of the rollup of the rows hold every row of the unit once */
		uint64		rows = 0;

		for (uint32 t = 0; t < r->ntuples; t++)
			rows += r->count[t];
		if (rows != unit->nrows)
			corrupt_cube(seg, u);
	}
	if (unit->except >= 0)
	{
		SiftCRollup x;

		if (!x_rollup(seg, u, list, mcxt, &x) || !rollup_subtract(r, &x))
			return false;
	}
	if (sift_col_unit_dead_on(unit) && !sift_col_unit_dead_rollup(unit, r))
		return false;
	memset(dict, 0, SIFT_PAGG_MAX_RDIMS * sizeof(*dict));
	for (int j = 0; j < r->ndims; j++)
		if (seg->schema->cols[r->dim[j]].store == SIFT_COLS_STR)
			sift_cdict_view(sift_cseg_dict(seg, r->dim[j]), seg->entry.gen << 8 | r->dim[j], &dict[j]);
	return true;
}

uint32
sift_col_unit_rollup_tuples(const SiftColUnit *unit, int list)
{
	SiftColSegment *seg;
	uint32		u,
				len;
	const uint8 *blob;

	if (!sift_col_unit_preagg_ok(unit) || sift_col_unit_super(unit, &seg, &u))
		return 0;
	sift_col_unit_stored(unit, &seg, &u);
	if ((blob = sift_cseg_rollup(seg, u, list, &len)) == NULL)
		return 0;
	return sift_cpagg_rollup_tuples(blob, len);
}

bool
sift_col_unit_rollup_miss(const SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	/* a segment of minor 3 stays as written, readable by pg_sift 3.0: its rollups come with the merge that rewrites it */
	return sift_col_unit_stored(unit, &seg, &u) && seg->entry.format >= SIFT_COL_FORMAT_TAGS && sift_col_rollup_request(seg->index, seg->entry.gen);
}

void
sift_col_pagg_spec(Relation index, SiftCPaggSpec *spec)
{
	*spec = sift_col_rel(index)->pagg;
	sift_ktab_spec(sift_col_rel(index)->schema, spec);
}

/* The leaf with this attribute name or shredder path; a list of scalars or a map and its element leaf share a path: scalar takes the element leaf */
static int
find_leaf(Relation index, const char *name, bool scalar)
{
	const SiftColSchema *sc = sift_col_rel(index)->schema;
	TupleDesc	desc = RelationGetDescr(index);

	for (int c = 0; c < sc->ncols; c++)
		if ((!scalar || sc->cols[c].kind == SIFT_COLK_SCALAR) &&
			strcmp(sc->cols[c].path[0] ? sc->cols[c].path : NameStr(TupleDescAttr(desc, sc->cols[c].attno - 1)->attname), name) == 0)
			return c;
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("index \"%s\" has no leaf \"%s\"", RelationGetRelationName(index), name)));
	return -1;
}

/* A 128-bit integer scaled by 10^scale as a numeric */
/* The text of a key-domain value of a non-text leaf, as its SQL type prints it */
static Datum
key_text(Relation index, const SiftColDef *def, int64 key)
{
	Oid			typid = sift_col_def_typid(RelationGetDescr(index), def),
				outfn;
	bool		isvar;
	Datum		d;

	if (def->lane > 0 || def->lanes > 1)	/* a lane of a wide value is a plain integer */
		return CStringGetTextDatum(psprintf(INT64_FORMAT, key));
	switch (def->store == SIFT_COLS_INT ? def->type : def->store == SIFT_COLS_F4 ? SIFT_COLT_FLOAT4 : SIFT_COLT_FLOAT8)
	{
		case SIFT_COLT_BOOL:
			d = BoolGetDatum(key != 0);
			break;
		case SIFT_COLT_INT2:
			d = Int16GetDatum((int16) key);
			break;
		case SIFT_COLT_INT4:
			d = Int32GetDatum((int32) key);
			break;
		case SIFT_COLT_DATE:
			d = DateADTGetDatum((DateADT) key);
			break;
		case SIFT_COLT_NUMERIC:
			d = sift_col_key_numeric(def->scale, key);
			break;
		case SIFT_COLT_FLOAT4:
			d = Float4GetDatum((float4) sift_col_f8_from_key(key));
			break;
		case SIFT_COLT_FLOAT8:
			d = Float8GetDatum(sift_col_f8_from_key(key));
			break;
		default:
			d = Int64GetDatum(key);
	}
	getTypeOutputInfo(typid, &outfn, &isvar);
	return CStringGetTextDatum(OidOutputFunctionCall(outfn, d));
}

static bool
is_float(const SiftColDef *def)
{
	return def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8;
}

typedef struct PaggScan
{
	Relation	index;
	Relation	heap;
	SiftColSource *src;
} PaggScan;

static void
pagg_open(PaggScan *ps, Oid indexoid)
{
	ps->index = index_open(indexoid, AccessShareLock);
	if (!sift_col_is_index(ps->index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(ps->index))));
	ps->heap = table_open(ps->index->rd_index->indrelid, AccessShareLock);
	ps->src = sift_col_open(ps->heap, ps->index, GetActiveSnapshot(), CurrentMemoryContext, 0);
	sift_col_begin(ps->src, NULL, 0, 0, PG_UINT32_MAX);
}

static void
pagg_close(PaggScan *ps)
{
	sift_col_close(ps->src);
	table_close(ps->heap, AccessShareLock);
	index_close(ps->index, AccessShareLock);
}

/* One row per stored unit: the aggregate of the leaf and whether the unit may answer from it (the visibility rule) */
Datum
sift_col_unit_aggs_sql(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	PaggScan	ps;
	int			leaf;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	pagg_open(&ps, PG_GETARG_OID(0));
	leaf = find_leaf(ps.index, text_to_cstring(PG_GETARG_TEXT_PP(1)), false);
	while (sift_col_next(ps.src, &unit))
	{
		SiftColSegment *seg;
		uint32		u;
		const SiftColDef *def = &sift_col_rel(ps.index)->schema->cols[leaf];
		SiftCAgg	a;
		Datum		v[11];
		bool		nulls[11] = {false};
		bool		minmax;

		if (!sift_col_unit_stored(&unit, &seg, &u))
		{
			/* a unit of the write buffer: no pre-aggregates, never exact */
			memset(nulls, true, sizeof(nulls));
			nulls[1] = nulls[2] = nulls[3] = false;
			v[1] = Int32GetDatum(unit.sb);
			v[2] = Int32GetDatum(unit.nrows);
			v[3] = BoolGetDatum(false);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
			continue;
		}
		sift_cseg_agg(seg, leaf, u, &a);
		minmax = def->store != SIFT_COLS_STR && a.nnull < a.nvals;
		v[0] = Int64GetDatum(seg->entry.gen);
		v[1] = Int32GetDatum(unit.sb);
		v[2] = Int32GetDatum(unit.nrows);
		v[3] = BoolGetDatum(sift_col_unit_preagg_ok(&unit));
		v[4] = Int64GetDatum(a.nvals);
		v[5] = Int64GetDatum(a.nnull);
		nulls[6] = nulls[7] = !minmax;
		if (minmax)
		{
			v[6] = key_text(ps.index, def, a.min);
			v[7] = key_text(ps.index, def, a.max);
		}
		nulls[8] = nulls[9] = !a.has_sums;
		if (a.has_sums)
		{
			int			scale = def->lane > 0 || def->lanes > 1 ? 0 : def->scale;

			v[8] = sift_col_w128_numeric(a.sum, scale);
			v[9] = sift_col_w128_numeric(a.sumsq, 2 * scale);
			nulls[9] = (a.flags & SIFT_CAGG_SUMSQ_OVF) != 0;
		}
		nulls[10] = !is_float(def);
		v[10] = Float8GetDatum(a.sum_f);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	pagg_close(&ps);
	return (Datum) 0;
}

/* One row per group of the dim's histogram in every stored unit that has one; the measure's count and sum when one is given and stored */
Datum
sift_col_unit_hists_sql(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	PaggScan	ps;
	int			dim,
				meas = -1;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	pagg_open(&ps, PG_GETARG_OID(0));
	dim = find_leaf(ps.index, text_to_cstring(PG_GETARG_TEXT_PP(1)), true);
	if (!PG_ARGISNULL(2))
		meas = find_leaf(ps.index, text_to_cstring(PG_GETARG_TEXT_PP(2)), true);
	while (sift_col_next(ps.src, &unit))
	{
		SiftColSegment *seg;
		uint32		u;
		SiftCHistDim hd;
		const SiftCHistDim *d = &hd;
		const SiftColDef *dd = &sift_col_rel(ps.index)->schema->cols[dim];
		bool		exact;
		int			slot;

		if (!sift_col_unit_stored(&unit, &seg, &u) || !seg_hist(seg, u, dim, CurrentMemoryContext, &hd))
			continue;
		slot = meas >= 0 ? sift_cpagg_meas_slot(d, meas) : -1;
		exact = sift_col_unit_preagg_ok(&unit);
		for (uint32 g = 0; g < d->ngroups; g++)
		{
			Datum		v[8];
			bool		nulls[8] = {false};
			bool		is_null_group = d->has_null && g == d->ngroups - 1;

			v[0] = Int64GetDatum(seg->entry.gen);
			v[1] = Int32GetDatum(unit.sb);
			v[2] = BoolGetDatum(exact);
			nulls[3] = is_null_group;
			if (!is_null_group)
			{
				if (dd->store == SIFT_COLS_STR)
				{
					uint32		slen;
					const char *str = sift_cdict_str(sift_cseg_dict(seg, dim), (uint32) d->key[g], &slen);

					v[3] = PointerGetDatum(cstring_to_text_with_len(str, slen));
				}
				else
					v[3] = key_text(ps.index, dd, d->key[g]);
			}
			v[4] = Int64GetDatum(d->count[g]);
			nulls[5] = nulls[6] = nulls[7] = slot < 0;
			if (slot >= 0)
			{
				const SiftColDef *md = &sift_col_rel(ps.index)->schema->cols[meas];

				v[5] = Int64GetDatum(d->nn[slot][g]);
				nulls[6] = d->sum[slot] == NULL;
				nulls[7] = d->sumf[slot] == NULL;
				if (d->sum[slot])
					v[6] = sift_col_w128_numeric(d->sum[slot][g], md->scale);
				if (d->sumf[slot])
					v[7] = Float8GetDatum(d->sumf[slot][g]);
			}
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
	}
	pagg_close(&ps);
	return (Datum) 0;
}
