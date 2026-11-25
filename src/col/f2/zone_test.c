/*
 * col/f2/zone_test.c - test-only SQL functions of the zones of format major 2: the verdict of a zone on a comparison, and the zones the granules of an index keep for a leaf.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/f2/test.h"

PG_FUNCTION_INFO_V1(sift_test_zone_verdict_f8);
PG_FUNCTION_INFO_V1(sift_test_zone_verdict_i8);
PG_FUNCTION_INFO_V1(sift_test_f2_zone_dump);
PG_FUNCTION_INFO_V1(sift_test_f2_zone_census);

static const char *
verdict_name(ZoneVerdict v)
{
	return v == Z_NONE ? "none" : v == Z_ALL ? "all" : "maybe";
}

/* (op, constants, min, max, nnull, nrows, flags): what a zone of a float column says of op over the constants */
Datum
sift_test_zone_verdict_f8(PG_FUNCTION_ARGS)
{
	ArrayType  *consts = PG_GETARG_ARRAYTYPE_P(1);
	const double *c = (const double *) ARR_DATA_PTR(consts);
	SiftColValue vals[16];
	SiftColPred p = {.col = 0,.op = PG_GETARG_INT32(0),.nvals = Min(ArrayGetNItems(ARR_NDIM(consts), ARR_DIMS(consts)), 16),.vals = vals};
	SiftZone	z = {.fmin = PG_GETARG_FLOAT8(2),.fmax = PG_GETARG_FLOAT8(3),.nnull = PG_GETARG_INT32(4),.flags = PG_GETARG_INT32(6)};
	SiftColDef	d = {.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_F8,.type = SIFT_COLT_FLOAT8};

	for (int j = 0; j < p.nvals; j++)
		vals[j] = (SiftColValue) {.v.f = c[j]};
	PG_RETURN_TEXT_P(cstring_to_text(verdict_name(sift_f2_zone_verdict(&d, &p, &z, PG_GETARG_INT32(5), false))));
}

/* (op, constants, min, max, nnull, nrows, flags): the same for an integer column */
Datum
sift_test_zone_verdict_i8(PG_FUNCTION_ARGS)
{
	ArrayType  *consts = PG_GETARG_ARRAYTYPE_P(1);
	const int64 *c = (const int64 *) ARR_DATA_PTR(consts);
	SiftColValue vals[16];
	SiftColPred p = {.col = 0,.op = PG_GETARG_INT32(0),.nvals = Min(ArrayGetNItems(ARR_NDIM(consts), ARR_DIMS(consts)), 16),.vals = vals};
	SiftZone	z = {.min = PG_GETARG_INT64(2),.max = PG_GETARG_INT64(3),.nnull = PG_GETARG_INT32(4),.flags = PG_GETARG_INT32(6)};
	SiftColDef	d = {.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_INT,.type = SIFT_COLT_INT8};

	for (int j = 0; j < p.nvals; j++)
		vals[j] = (SiftColValue) {.v.i = c[j]};
	PG_RETURN_TEXT_P(cstring_to_text(verdict_name(sift_f2_zone_verdict(&d, &p, &z, PG_GETARG_INT32(5), false))));
}

static int
leaf_of(const SiftColSchema *schema, const char *label)
{
	for (int i = 0; i < schema->ncols; i++)
	{
		const char *l = schema->cols[i].path != NULL && schema->cols[i].path[0] != '\0' ? schema->cols[i].path : schema->cols[i].name;

		if (l != NULL && strcmp(l, label) == 0)
			return i;
	}
	ereport(ERROR, (errmsg("the index has no leaf \"%s\"", label)));
}

/* Calls fn(zone, part, granule) for the zone of the leaf of every granule of every part of the index */
static void
each_zone(Relation rel, const char *label, void (*fn) (const SiftColDef *, const SiftZone *, uint32, void *), void *arg)
{
	const SiftColSchema *schema = sift_col_rel(rel)->schema;
	const int	leaf = leaf_of(schema, label);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *part = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);

			for (uint32 g = 0; zs != NULL && g < part->hdr.ngranules; g++)
			{
				SiftZone	z;

				sift_part_read_section_bytes(rel, part, zs, ((uint64) leaf * part->hdr.ngranules + g) * sizeof(z), sizeof(z), &z);
				fn(&schema->cols[leaf], &z, g, arg);
			}
		}
	}
}

static void
dump_zone(const SiftColDef *d, const SiftZone *z, uint32 g, void *arg)
{
	StringInfo	out = arg;
	const bool	flt = z->flags & SIFT_ZONE_FLOAT;

	if (flt)
		appendStringInfo(out, "%s%u:%x:%g:%g:%u", out->len ? " " : "", g, z->flags & 0x7F, z->fmin, z->fmax, z->nnull);
	else
		appendStringInfo(out, "%s%u:%x:%lld:%lld:%u", out->len ? " " : "", g, z->flags & 0x7F, (long long) z->min, (long long) z->max, z->nnull);
}

/* (index, leaf): "granule:flags:min:max:nnull" of the zones of the leaf of every granule */
Datum
sift_test_f2_zone_dump(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	StringInfoData out;

	initStringInfo(&out);
	each_zone(rel, text_to_cstring(PG_GETARG_TEXT_PP(1)), dump_zone, &out);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

typedef struct Census
{
	SiftColPred pred;
	int			n[3];
} Census;

static void
count_zone(const SiftColDef *d, const SiftZone *z, uint32 g, void *arg)
{
	Census	   *c = arg;
	ZoneVerdict v = sift_f2_zone_verdict(d, &c->pred, z, SIFT_GRANULE_ROWS, false);

	c->n[v == Z_NONE ? 0 : v == Z_ALL ? 2 : 1]++;
}

/* (index, leaf, op, float constants): how many granules the zones of the leaf rule out, cannot decide and pass whole: "none/maybe/all" */
Datum
sift_test_f2_zone_census(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ArrayType  *consts = PG_GETARG_ARRAYTYPE_P(3);
	const double *c = (const double *) ARR_DATA_PTR(consts);
	SiftColValue vals[16];
	Census		census = {.pred = {.col = 0,.op = PG_GETARG_INT32(2),.nvals = Min(ArrayGetNItems(ARR_NDIM(consts), ARR_DIMS(consts)), 16),.vals = vals}};

	for (int j = 0; j < census.pred.nvals; j++)
		vals[j] = (SiftColValue) {.v.f = c[j]};
	each_zone(rel, text_to_cstring(PG_GETARG_TEXT_PP(1)), count_zone, &census);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d/%d/%d", census.n[0], census.n[1], census.n[2])));
}

#endif							/* SIFT_TESTING */
