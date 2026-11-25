/*
 * col/f2/preagg/test.c - test-only SQL functions sift._test_f2_*: the pre-aggregates of an index, the compaction, the time strides, the digest of a part, the order of the rows and the plan (col/f2/core_test.c has the shared helpers)
 */
#include "postgres.h"

#include "core/horizon.h"
#include "core/memtrack.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "col/f2/test_int.h"

PG_FUNCTION_INFO_V1(sift_test_f2_preagg);
PG_FUNCTION_INFO_V1(sift_test_f2_preagg_index);
PG_FUNCTION_INFO_V1(sift_test_f2_preagg_excl);
PG_FUNCTION_INFO_V1(sift_test_f2_candidates);
PG_FUNCTION_INFO_V1(sift_test_f2_compact);
PG_FUNCTION_INFO_V1(sift_test_f2_compact_racing);
PG_FUNCTION_INFO_V1(sift_test_f2_plan);
PG_FUNCTION_INFO_V1(sift_test_f2_tstride);
PG_FUNCTION_INFO_V1(sift_test_f2_part_bytes);
PG_FUNCTION_INFO_V1(sift_test_f2_part_digest);
PG_FUNCTION_INFO_V1(sift_test_f2_order);
PG_FUNCTION_INFO_V1(sift_test_f2_qset);

static const SiftPreaggReads all_reads = SIFT_PREAGG_READS_ALL;

/* ---- the oracle: the cells of a cube from the values of the granule, against the cells it holds ---- */

#define ORACLE_MAX_GROUPS	8192

typedef struct OMeas
{
	uint64		cnt;
	uint64		sum_lo;
	int64		sum_hi;
	int64		mn,
				mx;
	double		fsum;
	double		fmin,
				fmax;
	bool		nan,
				pinf,
				ninf,
				nonnan;
} OMeas;

typedef struct OGrp
{
	uint8		mask;
	int64		key[SIFT_PREAGG_MAX_DIMS];
	uint64		rows;
	OMeas		m[SIFT_PREAGG_MAX_MEAS];
	int64	   *dv[SIFT_PREAGG_MAX_DIST];
	int			nd[SIFT_PREAGG_MAX_DIST];
} OGrp;

typedef struct OracleCols
{
	int			nr;
	int64	   *dim[SIFT_PREAGG_MAX_DIMS];
	bool	   *dnull[SIFT_PREAGG_MAX_DIMS];
	int64	   *ival[SIFT_PREAGG_MAX_MEAS];
	double	   *fval[SIFT_PREAGG_MAX_MEAS];
	bool	   *mnull[SIFT_PREAGG_MAX_MEAS];
	int64	   *dist[SIFT_PREAGG_MAX_DIST];
	bool	   *distnull[SIFT_PREAGG_MAX_DIST];
} OracleCols;

/* The code a string has in the dictionary of the part (-1: none) */
static int64
dict_code(const SiftPartDict *pd, const char *p, uint32 len)
{
	for (uint32 c = 0; c < pd->n; c++)
		if (pd->off[c + 1] - pd->off[c] == len && memcmp(pd->bytes + pd->off[c], p, len) == 0)
			return c;
	return -1;
}

/* A column as integers, or as the codes of the dictionary of the part for a string */
static void
oracle_ints(const SiftGranuleView *v, int leaf, bool is_dict, int nr, int64 **out, bool **isnull)
{
	*out = palloc(nr * sizeof(int64));
	*isnull = palloc(nr);
	if (!is_dict)
		sift_granule_ints(v, leaf, *out, *isnull);
	else
	{
		SiftStrCell *sc = palloc(nr * sizeof(SiftStrCell));
		const SiftPartDict *pd = sift_part_dict_find(v->dicts, leaf, SIFT_DICT_KIND_STR);

		sift_granule_strs(v, leaf, sc);
		for (int r = 0; r < nr; r++)
		{
			(*isnull)[r] = sc[r].isnull;
			(*out)[r] = sc[r].isnull ? 0 : dict_code(pd, sc[r].p, sc[r].len);
		}
	}
}

static void
oracle_read(const SiftPreaggCubeDesc *cd, const SiftGranuleView *v, OracleCols *oc)
{
	const int	nr = oc->nr;

	for (int d = 0; d < cd->ndims; d++)
	{
		oracle_ints(v, cd->dim_leaf[d], cd->dim_kind[d] == SIFT_PDIM_DICT32, nr, &oc->dim[d], &oc->dnull[d]);
		for (int r = 0; cd->dim_kind[d] == SIFT_PDIM_NULLFLAG && r < nr; r++)
		{
			oc->dim[d][r] = !oc->dnull[d][r];
			oc->dnull[d][r] = false;
		}
	}
	for (int m = 0; m < cd->nmeasures; m++)
	{
		oc->mnull[m] = palloc(nr);
		if (cd->meas_kind[m] >= SIFT_PMEAS_F4)
		{
			oc->fval[m] = palloc(nr * sizeof(double));
			sift_granule_f8(v, cd->meas_leaf[m], oc->fval[m], oc->mnull[m]);
		}
		else
		{
			oc->ival[m] = palloc(nr * sizeof(int64));
			sift_granule_ints(v, cd->meas_leaf[m], oc->ival[m], oc->mnull[m]);
		}
	}
	for (int d = 0; d < cd->ndistinct; d++)
		oracle_ints(v, cd->dist_leaf[d], v->dicts != NULL && sift_part_dict_find(v->dicts, cd->dist_leaf[d], SIFT_DICT_KIND_STR) != NULL, nr, &oc->dist[d], &oc->distnull[d]);
}

static void
omeas_add(OMeas *a, const SiftPreaggCubeDesc *cd, int m, const OracleCols *oc, int r)
{
	if (oc->mnull[m][r])
		return;
	a->cnt++;
	if (cd->meas_kind[m] < SIFT_PMEAS_F4)
	{
		const int64 x = oc->ival[m][r];

		a->mn = a->cnt == 1 ? x : Min(a->mn, x);
		a->mx = a->cnt == 1 ? x : Max(a->mx, x);
		sift_pag_sum_add(&a->sum_lo, &a->sum_hi, x);
		return;
	}
	{
		const double x = oc->fval[m][r];

		if (isnan(x))
		{
			a->nan = true;
			return;
		}
		a->pinf |= isinf(x) && x > 0;
		a->ninf |= isinf(x) && x < 0;
		if (!isinf(x))
			a->fsum += x;
		a->fmin = !a->nonnan || x < a->fmin ? x : a->fmin;
		a->fmax = !a->nonnan || x > a->fmax ? x : a->fmax;
		a->nonnan = true;
	}
}

static void
odist_add(OGrp *g, int d, int64 x)
{
	for (int i = 0; i < g->nd[d]; i++)
		if (g->dv[d][i] == x)
			return;
	g->dv[d] = g->nd[d] == 0 ? palloc(SIFT_GRANULE_ROWS * sizeof(int64)) : g->dv[d];
	g->dv[d][g->nd[d]++] = x;
}

static OGrp *
oracle_groups(const SiftPreaggCubeDesc *cd, const OracleCols *oc, const bool *excluded, int *ng)
{
	OGrp	   *grp = palloc0(ORACLE_MAX_GROUPS * sizeof(OGrp));
	int			n = 0;

	for (int r = 0; r < oc->nr; r++)
	{
		uint8		mask = 0;
		int64		key[SIFT_PREAGG_MAX_DIMS] = {0};
		OGrp	   *g = NULL;

		if (excluded != NULL && excluded[r])
			continue;
		for (int d = 0; d < cd->ndims; d++)
			if (oc->dnull[d][r])
				mask |= (uint8) (1 << d);
			else
				key[d] = oc->dim[d][r];
		for (int i = 0; i < n && g == NULL; i++)
			if (grp[i].mask == mask && memcmp(grp[i].key, key, sizeof(key)) == 0)
				g = &grp[i];
		if (g == NULL)
		{
			g = &grp[n++];
			g->mask = mask;
			memcpy(g->key, key, sizeof(key));
		}
		g->rows++;
		for (int m = 0; m < cd->nmeasures; m++)
			omeas_add(&g->m[m], cd, m, oc, r);
		for (int d = 0; d < cd->ndistinct; d++)
			if (!oc->distnull[d][r])
				odist_add(g, d, oc->dist[d][r]);
	}
	*ng = n;
	return grp;
}

static int
cmp_i64(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static bool
meas_matches(const SiftPreaggMeasure *pm, const OMeas *o, bool is_float)
{
	if (pm->count_nonnull != o->cnt)
		return false;
	if (!is_float)
	{
		return pm->u.i.sum_hi == o->sum_hi && (uint64) pm->u.i.sum_lo == o->sum_lo && (o->cnt == 0 || (pm->u.i.min_i == o->mn && pm->u.i.max_i == o->mx));
	}
	{
		const double sum = pm->u.f.sum_hi + pm->u.f.sum_lo;
		const bool	nonnan = (pm->flags & SIFT_PM_HAS_NONNAN) != 0;

		return !!(pm->flags & SIFT_PM_HAS_NAN) == o->nan && !!(pm->flags & SIFT_PM_HAS_PINF) == o->pinf && !!(pm->flags & SIFT_PM_HAS_NINF) == o->ninf && nonnan == o->nonnan &&
			fabs(sum - o->fsum) <= 1e-12 * (fabs(o->fsum) + 1) && (!nonnan || (pm->u.f.min_f == o->fmin && pm->u.f.max_f == o->fmax));
	}
}

/* The distinct values the cell holds against the oracle's: an overflowed set holds more than the budget */
static bool
dist_matches(Relation rel, const SiftPreaggPart *pp, const SiftPreaggDistRef *ref, const OGrp *o, int d)
{
	int64		vals[SIFT_GRANULE_ROWS];
	uint32		n;
	int64	   *want;

	if (ref->mode == SIFT_PDIST_OVERFLOW)
		return o->nd[d] > SIFT_PAG_DIST_LIST_MAX;
	n = sift_preagg_dist_values(ref, sift_preagg_dist_read(rel, pp, ref), vals, SIFT_GRANULE_ROWS);
	if (n != (uint32) o->nd[d])
		return false;
	want = o->nd[d] > 0 ? memcpy(palloc(o->nd[d] * sizeof(int64)), o->dv[d], o->nd[d] * sizeof(int64)) : NULL;
	if (n > 0)
		qsort(want, n, sizeof(int64), cmp_i64);
	return n == 0 || memcmp(want, vals, n * sizeof(int64)) == 0;
}

/* The mismatches between the cells of a cube of a granule (those with a row left) and the cube of the rows that are not excluded: a cell of its own for each group, and its fields (the sets of distinct values only for the whole granule: a subtraction leaves them) */
static int
cube_diff(Relation rel, const SiftPreaggPart *pp, int k, const SiftPreaggGran *gran, const SiftGranuleView *v, const bool *excluded)
{
	const SiftPreaggCubeDesc *cd = &pp->cube[k];
	OracleCols	oc = {.nr = v->hdr->nrows};
	int			ng,
				bad = 0,
				cells = 0;
	OGrp	   *grp;

	oracle_read(cd, v, &oc);
	grp = oracle_groups(cd, &oc, excluded, &ng);
	for (uint32 c = 0; c < gran->ncells; c++)
	{
		const SiftPreaggGroupHdr *gh = sift_pag_group(cd, gran->cells, c);
		OGrp	   *o = NULL;

		if (gh->row_count == 0)
			continue;
		cells++;
		for (int i = 0; i < ng && o == NULL; i++)
			if (grp[i].mask == gh->dim_null_mask && memcmp(grp[i].key, gh->keys, sizeof(gh->keys)) == 0)
				o = &grp[i];
		if (o == NULL || o->rows != gh->row_count)
		{
			bad++;
			continue;
		}
		for (int m = 0; m < cd->nmeasures; m++)
			bad += !meas_matches(sift_pag_measure(cd, gh, m), &o->m[m], cd->meas_kind[m] >= SIFT_PMEAS_F4);
		for (int d = 0; excluded == NULL && d < cd->ndistinct; d++)
			bad += !dist_matches(rel, pp, sift_pag_dist(cd, gh, d), o, d);
	}
	return bad + abs(cells - ng);
}

/* ---- sift._test_preagg_v2_check: every cube of every granule of every part against the values ---- */

PG_FUNCTION_INFO_V1(sift_test_preagg_v2_check);

typedef struct CheckTally
{
	int			parts;
	int			with_preagg;
	int			cubes;
	int			granules;
	int			skipped;
	int64		cells;
	int			mismatched;
} CheckTally;

static void
check_part(Relation rel, const SiftPart *p, bool with_flag, CheckTally *t)
{
	SiftPreaggPart *pp = sift_preagg_part_open(rel, p);

	t->parts++;
	if (pp == NULL)
		return;
	t->with_preagg++;
	t->mismatched += !with_flag;
	t->cubes += pp->hdr.ncubes;
	for (uint32 g = 0; g < p->hdr.ngranules; g++)
	{
		SiftGranuleDesc gd;
		char	   *rec = sift_part_read_granule(rel, p, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);

		t->granules++;
		for (int k = 0; k < pp->hdr.ncubes; k++)
		{
			SiftPreaggGran gran;

			sift_preagg_gran_read(rel, pp, k, g, &gran);
			t->cells += gran.ncells;
			if (gran.ncells == 0)
				t->skipped++;
			else
				t->mismatched += cube_diff(rel, pp, k, &gran, &v, NULL) > 0;
		}
	}
}

/* (index): the parts, those with SEC_PREAGG (all of them must carry SIFT_PART_EXT_PREAGG), the cubes, the granules, the cubes a granule is left out of, the cells and the cells that differ from the values */
Datum
sift_test_preagg_v2_check(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	CheckTally	t = {0};
	Datum		v[7];
	bool		n[7] = {0};

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
			check_part(rel, sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id), (pd[k].ext_flags & SIFT_PART_EXT_PREAGG) != 0, &t);
	}
	v[0] = Int32GetDatum(t.parts);
	v[1] = Int32GetDatum(t.with_preagg);
	v[2] = Int32GetDatum(t.cubes);
	v[3] = Int32GetDatum(t.granules);
	v[4] = Int32GetDatum(t.skipped);
	v[5] = Int64GetDatum(t.cells);
	v[6] = Int32GetDatum(t.mismatched);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* ---- the correction of a cube for dead rows ---- */

typedef struct PreaggTally
{
	int			granules;
	int			with_dead;
	int			corrected;
	int			rescanned;
	int			mismatched;
	int64		dead;
} PreaggTally;

/* The cube of the part that has dimension dim alone: its number, -1 when there is none */
static int
dim_cube(const SiftPreaggPart *pp, int dim)
{
	for (int k = 0; pp != NULL && k < pp->hdr.ncubes; k++)
		if (pp->cube[k].ndims == 1 && pp->cube[k].dim_leaf[0] == dim)
			return k;
	return -1;
}

/* The cube of a granule less the rows its deletion vector marks against the cube of the rows it leaves, one cell at a time */
static void
preagg_granule(Relation rel, const SiftPart *p, const SiftPreaggPart *pp, uint32 g, const SiftDv *dv, int dim, PreaggTally *t)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	const int	k = dim_cube(pp, dim);
	bool	   *dead = palloc0(gd.nrows);
	SiftPreaggGran gran;

	t->granules++;
	if (dg == NULL || dg->ndead == 0 || k < 0)
		return;
	t->with_dead++;
	t->dead += dg->ndead;
	sift_preagg_gran_read(rel, pp, k, g, &gran);
	if (!sift_preagg_delta_ok(gd.nrows, dg->ndead) || !sift_preagg_v2_subtract(&pp->cube[k], &gran, &v, dg->rows, dg->ndead, &all_reads))
	{
		t->rescanned++;
		return;
	}
	t->corrected++;
	for (int i = 0; i < dg->ndead; i++)
		dead[dg->rows[i]] = true;
	t->mismatched += cube_diff(rel, pp, k, &gran, &v, dead) > 0;
}

/* (index, dimension column, measure column): the cube of every granule of every part less its deleted rows, against the cube of the rows that are left, and how many granules it could correct */
Datum
sift_test_f2_preagg_index(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	PreaggTally t = {0};
	int			nparts = 0;
	Datum		v[7];
	bool		n[7] = {0};

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			SiftDv	   *dv = pd[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &pd[k], (uint32) mr.entries[i].part_id, f2t_dir_rows(rel, p)) : NULL;
			SiftPreaggPart *pp = sift_preagg_part_open(rel, p);

			for (uint32 g = 0; pp != NULL && g < p->hdr.ngranules; g++)
				preagg_granule(rel, p, pp, g, dv, PG_GETARG_INT32(1), &t);
			nparts++;
		}
	}
	v[0] = Int32GetDatum(nparts);
	v[1] = Int32GetDatum(t.granules);
	v[2] = Int32GetDatum(t.with_dead);
	v[3] = Int32GetDatum(t.corrected);
	v[4] = Int32GetDatum(t.rescanned);
	v[5] = Int32GetDatum(t.mismatched);
	v[6] = Int64GetDatum(t.dead);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

typedef struct ExclTally
{
	int			granules;
	int			with_excl;
	int			corrected;
	int			rescanned;
	int			mismatched;
	int			naive_mismatched;
	int64		dead;
	int64		not_null;
	int64		overlap;
} ExclTally;

/*
 * The cube of a granule less the rows that WHERE nullcol IS NULL leaves out of the answer (the rows nullcol has a value on) and the rows its deletion vector marks, as the one list of their union
 * (a row in both is taken out once), against the cube of the rows that are left; and the same taking the two lists out one after the other, which counts a row in both twice
 */
static void
excl_granule(Relation rel, const SiftPart *p, const SiftPreaggPart *pp, const SiftGranuleDesc *dir, uint32 g, const SiftDv *dv, int dim, int nullcol, ExclTally *t)
{
	const uint16 cap = SIFT_PREAGG_SPARSE_PRED_MAX;
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	const int	k = dim_cube(pp, dim);
	int			nr = gd.nrows,
				nd = dg ? dg->ndead : 0;
	uint16	   *all = palloc(nr * sizeof(uint16)),
			   *un = palloc((nr + 1) * sizeof(uint16));
	uint16		nall = sift_granule_nonnull_rows(&v, nullcol, all, nr),
				nlim = sift_part_read_nonnull_rows(rel, p, &dir[g], nullcol, un, cap),
				nun;
	bool	   *excluded = palloc0(nr);
	SiftPreaggGran gran,
				gran2;

	t->granules++;
	t->mismatched += nlim != Min(nall, cap + 1);
	if ((nd == 0 && nall == 0) || k < 0)
		return;
	t->with_excl++;
	t->dead += nd;
	t->not_null += nall;
	for (int i = 0; i < nd; i++)
		excluded[dg->rows[i]] = true;
	for (int i = 0; i < nall; i++)
	{
		t->overlap += excluded[all[i]];
		excluded[all[i]] = true;
	}
	sift_preagg_gran_read(rel, pp, k, g, &gran);
	gran2 = (SiftPreaggGran) {.ncells = gran.ncells,.cells = memcpy(palloc(Max(gran.ncells, 1) * (uint64) sift_pag_cell_size(&pp->cube[k])), gran.cells, gran.ncells * (uint64) sift_pag_cell_size(&pp->cube[k]))};
	t->naive_mismatched += !((nd == 0 || sift_preagg_v2_subtract(&pp->cube[k], &gran2, &v, dg->rows, nd, &all_reads)) && sift_preagg_v2_subtract(&pp->cube[k], &gran2, &v, all, nall, &all_reads) &&
							 cube_diff(rel, pp, k, &gran2, &v, excluded) == 0);
	nun = sift_excl_union_u16(dg ? dg->rows : NULL, nd, all, nall, un, cap);
	if (nun > cap || !sift_preagg_excl_ok(nr, nun, nd, 1) || !sift_preagg_v2_subtract(&pp->cube[k], &gran, &v, un, nun, &all_reads))
	{
		t->rescanned++;
		return;
	}
	t->corrected++;
	t->mismatched += cube_diff(rel, pp, k, &gran, &v, excluded) > 0;
}

/* (index, dimension column, measure column, column the predicate IS NULL is on): per granule of every part, the cube less the union of its dead rows and the rows the column has a value on against the cube of the rows that are left */
Datum
sift_test_f2_preagg_excl(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	ExclTally	t = {0};
	Datum		v[9];
	bool		n[9] = {0};

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			SiftGranuleDesc *dir = sift_part_granule_dir(rel, p);
			SiftDv	   *dv = pd[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &pd[k], (uint32) mr.entries[i].part_id, f2t_dir_rows(rel, p)) : NULL;
			SiftPreaggPart *pp = sift_preagg_part_open(rel, p);

			for (uint32 g = 0; pp != NULL && g < p->hdr.ngranules; g++)
				excl_granule(rel, p, pp, dir, g, dv, PG_GETARG_INT32(1), PG_GETARG_INT32(3), &t);
		}
	}
	v[0] = Int32GetDatum(t.granules);
	v[1] = Int32GetDatum(t.with_excl);
	v[2] = Int32GetDatum(t.corrected);
	v[3] = Int32GetDatum(t.rescanned);
	v[4] = Int32GetDatum(t.mismatched);
	v[5] = Int32GetDatum(t.naive_mismatched);
	v[6] = Int64GetDatum(t.dead);
	v[7] = Int64GetDatum(t.not_null);
	v[8] = Int64GetDatum(t.overlap);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* (index, part_id, partition lower key, generation, granule): the stored cube of the first dimension less the dead rows against the cube of the live rows */
Datum
sift_test_f2_preagg(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n,
				live = 0,
				cells = 0;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	uint32		g = PG_GETARG_INT32(4);
	SiftPart   *p = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) entry.part_id);
	SiftPreaggPart *pp = sift_preagg_part_open(rel, p);
	SiftDv	   *dv = d->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, d, (uint32) entry.part_id, f2t_dir_rows(rel, p)) : NULL;
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	bool	   *dead = palloc0(gd.nrows);
	SiftPreaggGran gran;
	int			k = -1;
	char	   *res;

	for (int c = 0; pp != NULL && k < 0 && c < pp->hdr.ncubes; c++)
		k = pp->cube[c].ndims == 1 ? c : -1;
	if (k < 0 || dg == NULL)
		elog(ERROR, "the part has no pre-aggregate or no dead rows in granule %u", g);
	sift_preagg_gran_read(rel, pp, k, g, &gran);
	if (!sift_preagg_v2_subtract(&pp->cube[k], &gran, &v, dg->rows, dg->ndead, &all_reads))
		res = psprintf("fallback ndead=%d rows=%d delta_ok=%d", dg->ndead, gd.nrows, sift_preagg_delta_ok(gd.nrows, dg->ndead));
	else
	{
		for (int i = 0; i < dg->ndead; i++)
			dead[dg->rows[i]] = true;
		for (uint32 c = 0; c < gran.ncells; c++)
		{
			const uint64 rows = sift_pag_group(&pp->cube[k], gran.cells, c)->row_count;

			cells += rows > 0 && (sift_pag_group(&pp->cube[k], gran.cells, c)->dim_null_mask == 0);
			live += (int) rows;
		}
		res = psprintf("delta ok=%s cells=%d live=%d", cube_diff(rel, pp, k, &gran, &v, dead) == 0 ? "true" : "false", cells, live);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

Datum
sift_test_f2_candidates(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n,
				nc;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	uint64	   *c = sift_compact_candidates(parts, n, &nc);
	Datum	   *elems = palloc(Max(nc, 1) * sizeof(Datum));

	for (int i = 0; i < nc; i++)
		elems[i] = Int64GetDatum(c[i]);
	index_close(rel, NoLock);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, nc, INT8OID));
}

/* (index, part_id, partition lower key, generations, preagg, dictionary remap) */
Datum
sift_test_f2_compact(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(3);
	int			ng = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	uint64	   *gens = palloc(Max(ng, 1) * sizeof(uint64));
	SiftPartSpec spec = (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_rel(rel)->spec : f2t_spec(PG_GETARG_BOOL(4));
	SiftCompactResult res;
	bool		done;
	Datum		v[13];
	bool		nl[13] = {0};
	int64	   *src = (int64 *) ARR_DATA_PTR(a);

	for (int i = 0; i < ng; i++)
		gens[i] = src[i];
	sift_compact_dict_remap = PG_NARGS() < 6 || PG_GETARG_BOOL(5);
	PG_TRY();
	{
		done = sift_partition_compact_online(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), gens, ng, &spec, (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_hot_cut(sift_f2_rel(rel)) : PG_INT64_MIN, sift_horizon(rel), &res);
	}
	PG_FINALLY();
	{
		sift_compact_dict_remap = true;
	}
	PG_END_TRY();
	v[0] = BoolGetDatum(done);
	v[1] = Int64GetDatum(res.new_gen);
	v[2] = Int64GetDatum(res.rows_in);
	v[3] = Int64GetDatum(res.rows_out);
	v[4] = Int64GetDatum(res.dead_dropped);
	v[5] = Int64GetDatum(res.aborted_dropped);
	v[6] = Int64GetDatum(res.frozen);
	v[7] = Int64GetDatum(res.granules_copied);
	v[8] = Int64GetDatum(res.granules_written);
	v[9] = Int64GetDatum(res.bytes_out);
	v[10] = Int64GetDatum(res.dict_cols_remapped);
	v[11] = Int64GetDatum(res.dict_codes_remapped);
	v[12] = Int64GetDatum(res.writer_bytes);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/* (index, part_id, partition lower key, generations, rows a deleter takes from the first part between the steps): whether the compaction was done */
Datum
sift_test_f2_compact_racing(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(3);
	int			ng = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	uint64	   *gens = palloc(Max(ng, 1) * sizeof(uint64));
	SiftCompactResult res;
	int64	   *src = (int64 *) ARR_DATA_PTR(a);
	bool		done;

	for (int i = 0; i < ng; i++)
		gens[i] = src[i];
	sift_test_compact_delete = PG_GETARG_INT32(4);
	done = sift_partition_compact_online(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), gens, ng, &sift_f2_rel(rel)->spec, sift_f2_hot_cut(sift_f2_rel(rel)), sift_store_horizon(rel), &res);
	sift_test_compact_delete = 0;
	index_close(rel, NoLock);
	PG_RETURN_BOOL(done);
}

/* A part of n rows with a time index (the key is the time), written and read back stride by stride; its blocks are released at the end */
Datum
sift_test_f2_tstride(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	int			n = PG_GETARG_INT32(1);
	SiftPartSpec spec = f2t_spec(false);
	SiftRowBuf *rb;
	SiftColIn	cols[F2_NCOLS];
	SiftRows	r;
	SiftPartDesc d;
	SiftPart   *part;
	SiftExtent *blocks;
	int			nb,
				good = 0,
				strides = 0;
	uint32		gen;

	spec.time_col = 1;
	rb = sift_rowbuf_new(&spec);
	for (int i = 0; i < n; i++)
	{
		SiftCell	c[F2_NCOLS] = {{0}};
		uint8		key[SIFT_BOUND_BYTES];

		c[0].i = i;
		c[1] = (SiftCell) {.s = "",.slen = 0};
		f2t_key(i, key);
		sift_rowbuf_add(rb, f2t_tid(0, i), FrozenTransactionId, 0, 0, key, c, NULL);
	}
	sift_rowbuf_sort(rb, &spec);
	r = sift_rowbuf_rows(rb, cols, &spec);
	gen = (uint32) sift_manifest_new_gen(rel, 1);
	d = sift_part_write_rows(rel, 1, gen, 1, SIFT_TIER_WARM_L1, &r);
	part = sift_part_open(rel, d.header_blk, gen, 1);
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		int			cnt = Min(n - (int) g * SIFT_GRANULE_ROWS, SIFT_GRANULE_ROWS),
					m;
		SiftTimeStride *st = sift_part_read_tstride(rel, part, g, &m);
		bool		ok = m == (cnt + SIFT_TSTRIDE_ROWS - 1) / SIFT_TSTRIDE_ROWS;

		for (int k = 0; ok && k < m; k++)
		{
			int64		first = (int64) g * SIFT_GRANULE_ROWS + (int64) k * SIFT_TSTRIDE_ROWS;

			ok = st[k].time_min == first && st[k].time_max == Min(first + SIFT_TSTRIDE_ROWS, (int64) g * SIFT_GRANULE_ROWS + cnt) - 1;
		}
		good += ok;
		strides += m;
	}
	blocks = sift_part_blocks(part, &nb);
	sift_fsm_release(rel, blocks, nb, InvalidFullTransactionId);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("granules=%u verified=%d strides=%d", part->hdr.ngranules, good, strides)));
}

/* The bytes of the parts of a partition, by level: element l + 1 of an array of 8 is the level l */
Datum
sift_test_f2_part_bytes(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	Datum		by_level[8] = {0};

	for (int i = 0; i < n; i++)
		by_level[Min(parts[i].level, 7)] = Int64GetDatum(DatumGetInt64(by_level[Min(parts[i].level, 7)]) + (int64) parts[i].nbytes);
	index_close(rel, NoLock);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(by_level, 8, INT8OID));
}

/* The rows of every part of a partition as bytes: the checksum of the dictionaries and the granule records of each part, in the order of the parts, as "granules:crc" for each */
Datum
sift_test_f2_part_digest(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	StringInfoData out;

	initStringInfo(&out);
	for (int i = 0; i < n; i++)
	{
		SiftPart   *part = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) PG_GETARG_INT64(1));
		StringInfoData bytes;
		uint64		len;
		void	   *dicts = sift_part_read_section(rel, part, SEC_DICTS, &len);

		initStringInfo(&bytes);
		if (dicts != NULL)
			appendBinaryStringInfo(&bytes, dicts, len);
		for (uint32 g = 0; g < part->hdr.ngranules; g++)
		{
			SiftGranuleDesc gd;
			char	   *rec = sift_part_read_granule(rel, part, g, &gd);

			appendBinaryStringInfo(&bytes, rec, gd.data_len);
			pfree(rec);
		}
		appendStringInfo(&out, "%s%u:%08x", i > 0 ? "," : "", part->hdr.ngranules, sift_crc32c(bytes.data, bytes.len));
		pfree(bytes.data);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * (index, part_id, partition lower key): what the lead key column looks like in the granules of the parts of a partition, read back with the decoders and not through the keys: the
 * rows in the order they are stored, how many steps of the integers go up and down, how many of the parts have their NULLs first, last, or broken by an integer
 */
Datum
sift_test_f2_order(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftF2Rel  *f = sift_f2_rel(rel);
	SiftPartitionEntry entry;
	int			n,
				first = 0,
				last = 0,
				broken = 0;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	int64		rows = 0,
				nulls = 0,
				ups = 0,
				downs = 0;

	for (int i = 0; i < n; i++)
	{
		SiftPart   *p = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) entry.part_id);
		bool		started = false,
					prev_null = false,
					have_val = false,
					null_run_done = false,
					part_first = false,
					part_broken = false;
		int64		prev = 0;

		for (uint32 g = 0; g < p->hdr.ngranules; g++)
		{
			SiftGranuleDesc gd;
			char	   *rec = sift_part_read_granule(rel, p, g, &gd);
			SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
			/* check-memory: allow a test function: the rows of one granule */
			int64	   *val = palloc(gd.nrows * sizeof(int64));
			bool	   *isnull = palloc(gd.nrows);

			sift_granule_ints(&v, f->key_cols[0], val, isnull);
			for (int r = 0; r < gd.nrows; r++)
			{
				rows++;
				nulls += isnull[r];
				part_first |= !started && isnull[r];
				if (started && prev_null != isnull[r])
				{
					part_broken |= null_run_done;
					null_run_done = true;
				}
				if (!isnull[r])
				{
					ups += have_val && val[r] > prev;
					downs += have_val && val[r] < prev;
					prev = val[r];
					have_val = true;
				}
				prev_null = isnull[r];
				started = true;
			}
			pfree(rec);
		}
		first += part_first;
		last += !part_first && null_run_done;
		broken += part_broken;
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("rows=%lld nulls=%lld ups=%lld downs=%lld parts_nulls_first=%d parts_nulls_last=%d parts_nulls_broken=%d", (long long) rows, (long long) nulls,
											  (long long) ups, (long long) downs, first, last, broken)));
}

/* What a vacuum would compact: the groups, as 'generations / generations' */
Datum
sift_test_f2_plan(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n,
				ng;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftCompactGroup *groups = sift_compact_plan(parts, n, (uint64) sift_mem_maint_kb() * 1024 / 4, (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_hot_cut(sift_f2_rel(rel)) : PG_INT64_MIN, &ng);
	StringInfoData out;

	initStringInfo(&out);
	for (int k = 0; k < ng; k++)
	{
		appendStringInfoString(&out, k > 0 ? " / " : "");
		for (int i = 0; i < groups[k].n; i++)
			appendStringInfo(&out, i > 0 ? ",%llu" : "%llu", (unsigned long long) groups[k].gens[i]);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* op: add, remove, has, count, generation */
Datum
sift_test_f2_qset(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int64		r = 0;

	if (strcmp(op, "add") == 0)
		sift_qset_add(&rel->rd_locator);
	else if (strcmp(op, "remove") == 0)
		sift_qset_remove(&rel->rd_locator);
	else if (strcmp(op, "has") == 0)
		r = sift_qset_has(&rel->rd_locator);
	else if (strcmp(op, "count") == 0)
		r = sift_qset_count();
	else
		r = sift_qset_generation();
	index_close(rel, NoLock);
	PG_RETURN_INT64(r);
}

#endif							/* SIFT_TESTING */
