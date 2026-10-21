/*
 * col/pagg.c - the pre-aggregates of a unit through the column source (col/pagg.h, docs/COL-PREAGG.md).
 */
#include "postgres.h"

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
#include "col/pagg.h"

PG_FUNCTION_INFO_V1(sift_col_unit_aggs_sql);
PG_FUNCTION_INFO_V1(sift_col_unit_hists_sql);

bool
sift_col_unit_preagg_ok(const SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	return sift_col_unit_stored(unit, &seg, &u) && sift_col_unit_exact(unit);
}

bool
sift_col_unit_agg(const SiftColUnit *unit, int col, SiftCAgg *a)
{
	SiftColSegment *seg;
	uint32		u;

	if (!sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	sift_cseg_agg(seg, col, u, a);
	return !(a->zflags & SIFT_COLZ_OVERFLOW);
}

static void
corrupt_cube(SiftColSegment *seg, uint32 u)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift index \"%s\" segment %llu: the cube of unit %u does not decode", RelationGetRelationName(seg->index),
					(unsigned long long) seg->entry.gen, u)));
}

/* The histogram of dim of a stored unit: from the cube, or the rollup of its list added up by dim. false: the unit has none */
static bool
seg_hist(SiftColSegment *seg, uint32 u, int dim, MemoryContext mcxt, SiftCHistDim *out)
{
	uint32		len;
	const uint8 *blob = sift_cseg_dim_cube(seg, u, dim, &len);
	SiftCRollup r;
	int			found;

	if (blob == NULL)
		return false;
	if (seg->schema->cols[dim].parent < 0)
	{
		if ((found = sift_cpagg_decode_dim(seg->schema, blob, len, mcxt, dim, out)) < 0)
			corrupt_cube(seg, u);
		return found > 0;
	}
	if (!sift_cpagg_rollup_decode(seg->schema, blob, len, mcxt, &r))
		corrupt_cube(seg, u);
	return sift_cpagg_rollup_dim(seg->schema, &r, dim, mcxt, out);
}

bool
sift_col_unit_hist(const SiftColUnit *unit, int dim, MemoryContext mcxt, SiftCHistDim *out, SiftColDict *dict)
{
	SiftColSegment *seg;
	uint32		u;

	if (!sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	if (!seg_hist(seg, u, dim, mcxt, out))
		return false;
	if (dict && seg->schema->cols[dim].store == SIFT_COLS_STR)
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

	if (!sift_col_unit_preagg_ok(unit))
		return false;
	sift_col_unit_stored(unit, &seg, &u);
	if ((blob = sift_cseg_rollup(seg, u, list, &len)) == NULL)
		return false;
	if (!sift_cpagg_rollup_decode_need(seg->schema, blob, len, mcxt, need, r))
		corrupt_cube(seg, u);
	memset(dict, 0, SIFT_PAGG_MAX_RDIMS * sizeof(*dict));
	for (int j = 0; j < r->ndims; j++)
		if (seg->schema->cols[r->dim[j]].store == SIFT_COLS_STR)
			sift_cdict_view(sift_cseg_dict(seg, r->dim[j]), seg->entry.gen << 8 | r->dim[j], &dict[j]);
	return true;
}

bool
sift_col_unit_rollup_miss(const SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	/* a segment of minor 3 stays as written, readable by pg_sift 3.0: its rollups come with the merge that rewrites it */
	return sift_col_unit_stored(unit, &seg, &u) && seg->entry.format >= 4 && sift_col_rollup_request(seg->index, seg->entry.gen);
}

void
sift_col_pagg_spec(Relation index, SiftCPaggSpec *spec)
{
	*spec = sift_col_rel(index)->pagg;
}

/* ---------- SQL: sift.col_unit_agg, sift.col_unit_hist (debug and test hooks) ---------- */

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
static Datum
i128_numeric(SiftCI128 v, int scale)
{
	char		digits[48],
				out[64];
	unsigned __int128 u = v < 0 ? -(unsigned __int128) v : (unsigned __int128) v;
	int			n = 0,
				o = 0;

	do
	{
		digits[n++] = '0' + (int) (u % 10);
		u /= 10;
	} while (u);
	while (n <= scale)
		digits[n++] = '0';
	if (v < 0)
		out[o++] = '-';
	for (int i = n - 1; i >= 0; i--)
	{
		out[o++] = digits[i];
		if (i == scale && scale > 0)
			out[o++] = '.';
	}
	out[o] = '\0';
	return DirectFunctionCall3(numeric_in, CStringGetDatum(out), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

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

			v[8] = i128_numeric(a.sum, scale);
			v[9] = i128_numeric(a.sumsq, 2 * scale);
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
					v[6] = i128_numeric(d->sum[slot][g], md->scale);
				if (d->sumf[slot])
					v[7] = Float8GetDatum(d->sumf[slot][g]);
			}
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
	}
	pagg_close(&ps);
	return (Datum) 0;
}
