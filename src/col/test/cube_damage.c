/*
 * col/test/cube_damage.c - damage the tests do to the cube of a segment
 * a unit, a census, a row
 * to see the verification refuse it
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "col/am.h"
#include "col/pagg.h"
#include "core/sql.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_cube_damage);

/* does a decoded cube keep what the decoder promises: min <= max in every group that has a value, and the groups of a calendar dim are the first days of cells of its grain, in ascending order */
static bool
cube_sound(const SiftColSchema *sc, const SiftCHist *h)
{
	for (int d = 0; d < h->ndims; d++)
	{
		const SiftCHistDim *hd = &h->dim[d];

		for (uint32 g = 0; hd->leaf >= SIFT_PAGG_CAL && g < hd->ngroups - (hd->has_null ? 1 : 0); g++)
			if (!sift_cpagg_cal_key_ok(sc->cols[hd->leaf % SIFT_PAGG_CAL].type, hd->leaf / SIFT_PAGG_CAL, hd->key[g]) || (g > 0 && hd->key[g] <= hd->key[g - 1]))
				return false;
		for (int m = 0; m < h->dim[d].nmeas; m++)
			for (uint32 g = 0; h->dim[d].min[m] != NULL && g < h->dim[d].ngroups; g++)
				if (h->dim[d].nn[m][g] > 0 && h->dim[d].min[m][g] > h->dim[d].max[m][g])
					return false;
	}
	return true;
}

/* does the census of the sidecar (sift_col_refresh) call a unit with these bytes odd (it must never read outside them either) */
static int
census_odd_units(const SiftColSchema *sc, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen)
{
	SiftCPaggCensus c;

	memset(&c, 0, sizeof(c));
	sift_cpagg_census(sc, spec, spec->minmax, blob, len, eblob, elen, &c);
	return c.odd;
}

/*
 * One cube blob decoded as stored, cut at every length, and with every byte set to some other values (one at a time). A decode must never read
 * outside the blob (meaningful under SANITIZE=1) and must refuse what it cannot trust: a cut blob is always refused, and a damaged one that is
 * accepted still obeys min <= max and the cells of a calendar dim. The census that sift.col_refresh takes of the same bytes (spec given: a sift_col index) reads headers only: it
 * must stay inside them, and call every cut blob odd (census_cut_odd; the empty cut is no blob).
 */
void
sift_test_cube_damage(const SiftColSchema *sc, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
					  MemoryContext cx, SiftTestDamage *r)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint8	   *copy = palloc(len);
	SiftCHist	h;

	r->blobs++;
	MemoryContextReset(cx);
	if (sift_cpagg_decode(sc, blob, len, cx, &h))
	{
		r->intact++;
		r->minmax += h.dim[0].minmax;
		for (int d = 0; d < h.ndims; d++)
			if (h.dim[d].leaf >= SIFT_PAGG_CAL)
			{
				r->calendar++;
				break;
			}
	}
	for (uint32 l = 0; l < len; l++)
	{
		MemoryContextReset(cx);
		memcpy(copy, blob, l);
		r->cut++;
		r->cut_accepted += sift_cpagg_decode(sc, copy, l, cx, &h);
		if (spec)
			r->census_cut_odd += census_odd_units(sc, spec, copy, l, eblob, eblob ? elen : 0);
	}
	for (uint32 i = 0; i < len; i++)
		for (int k = 0; k < lengthof(vals); k++)
		{
			memcpy(copy, blob, len);
			if (copy[i] == vals[k])
				continue;
			copy[i] = vals[k];
			MemoryContextReset(cx);
			r->damaged++;
			if (spec)
				r->census_odd += census_odd_units(sc, spec, copy, len, eblob, eblob ? elen : 0);
			if (sift_cpagg_decode(sc, copy, len, cx, &h))
			{
				r->accepted++;
				r->unsound += !cube_sound(sc, &h);
			}
		}
	pfree(copy);
}

/* The rollup of the rows of a unit decoded as stored, cut at every length and with every byte set to other values: a cut is always refused, and the decoder never reads outside the blob */
static void
rows_damage(const SiftColSchema *sc, const uint8 *blob, uint32 len, MemoryContext cx, SiftTestDamage *r)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint8	   *copy = palloc(len);
	SiftCRollup ro;

	r->blobs++;
	MemoryContextReset(cx);
	r->intact += sift_cpagg_rollup_decode(sc, blob, len, cx, &ro);
	for (uint32 l = 0; l < len; l++)
	{
		MemoryContextReset(cx);
		memcpy(copy, blob, l);
		r->cut++;
		r->cut_accepted += sift_cpagg_rollup_decode(sc, copy, l, cx, &ro);
	}
	for (uint32 i = 0; i < len; i++)
		for (int k = 0; k < lengthof(vals); k++)
		{
			memcpy(copy, blob, len);
			if (copy[i] == vals[k])
				continue;
			copy[i] = vals[k];
			MemoryContextReset(cx);
			r->damaged++;
			r->accepted += sift_cpagg_rollup_decode(sc, copy, len, cx, &ro);
		}
	pfree(copy);
}

/*
 * sift._test_col_cube_damage(index, units, rows): sift_test_cube_damage of the cube blob of the first units of the index (rows: rows_damage of their rollup of the rows)
 */
Datum
sift_test_col_cube_damage(PG_FUNCTION_ARGS)
{
	int			maxunits = PG_GETARG_INT32(1);
	bool		rows = PG_GETARG_BOOL(2);
	TestScan	ts;
	SiftColUnit unit;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "cube damage", ALLOCSET_DEFAULT_SIZES);
	SiftTestDamage r = {0};
	Datum		v[10];
	bool		nulls[10] = {false};

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (r.blobs < maxunits && ctest_test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					len,
					elen;
		const uint8 *blob;

		if (!sift_col_unit_stored(&unit, &seg, &u) || (blob = rows ? sift_cseg_rollup(seg, u, -1, &len) : sift_cseg_cube(seg, u, &len)) == NULL)
			continue;
		if (rows)
			rows_damage(sift_col_rel(ts.index)->schema, blob, len, cx, &r);
		else
			sift_test_cube_damage(sift_col_rel(ts.index)->schema, &sift_col_rel(ts.index)->pagg, blob, len, sift_cseg_ecube(seg, u, &elen), elen, cx, &r);
	}
	ctest_scan_close(&ts);
	v[0] = Int64GetDatum(r.blobs);
	v[1] = Int64GetDatum(r.intact);
	v[2] = Int64GetDatum(r.minmax);
	v[3] = Int64GetDatum(r.cut);
	v[4] = Int64GetDatum(r.cut_accepted);
	v[5] = Int64GetDatum(r.damaged);
	v[6] = Int64GetDatum(r.accepted);
	v[7] = Int64GetDatum(r.unsound);
	v[8] = Int64GetDatum(r.census_cut_odd);
	v[9] = Int64GetDatum(r.census_odd);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}
#endif							/* SIFT_TESTING */
