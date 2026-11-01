/*
 * col/preagg/cube.c - the mini cube of a unit: one dimension at a time
 * the groups, the counts, the sums and the minima and maxima
 * emitted as the cube blob
 */
#include "core/host.h"
#include "col/preagg/pagg_build_int.h"

/* Work areas of one unit's cube, sized by the group cap */
typedef struct Scratch
{
	uint32		hsize;
	uint32	   *gid;				/* [nrows] */
	uint32	   *perm;				/* [nrows] the rows grouped by gid, in the order of the rows within a group */
	uint32	   *gstart;				/* [maxg + 2] where a group starts in perm */
	int64	   *hk;					/* [hsize] */
	int32	   *hg;					/* [hsize] */
	int64	   *gkey;				/* [maxg + 1] */
	uint32	   *cnt;
	uint32	   *nn;
	GroupKey   *ord;
	double	   *sf;
	SiftCI128  *si;
	int64	   *mn;
	int64	   *mx;
	bool		minmax;
} Scratch;

/*
 * The count of values, the sum (128 bits), the minimum and the maximum of an integer measure by group. The rows are visited group by group (perm: the rows of a group
 * one after the other, gstart[g] where it starts), so that the four accumulators of a group stay in registers and are written once: the loop over the rows in their own order
 * loaded and stored them for each row. A measure without a NULL has no bit test and its count is the group's own (false is returned and nn is not written).
 */
static inline __attribute__((always_inline)) void
measure_int_pass(const int64 *restrict key, const uint64 *restrict nulls, const uint32 *restrict perm, const uint32 *restrict gstart, uint32 ng,
				 SiftCI128 *restrict si, uint32 *restrict nn, int64 *restrict mn, int64 *restrict mx, const bool has_nulls, const bool minmax, const bool narrow)
{
	for (uint32 g = 0; g < ng; g++)
	{
		SiftCI128	sum = 0;
		int64		nsum = 0,
					lo = INT64_MAX,
					hi = INT64_MIN;
		uint32		cnt = 0;

		for (uint32 j = gstart[g]; j < gstart[g + 1]; j++)
		{
			uint32		i = perm[j];
			int64		v;

			if (has_nulls && (nulls[i >> 6] >> (i & 63) & 1))
				continue;
			v = key[i];
			if (narrow)
				nsum += v;
			else
				sum += v;
			cnt++;
			if (minmax)
			{
				lo = Min(lo, v);
				hi = Max(hi, v);
			}
		}
		si[g] = narrow ? (SiftCI128) nsum : sum;
		if (has_nulls)
			nn[g] = cnt;
		mn[g] = lo;
		mx[g] = hi;
	}
}

/* The rows of each group together, in row order (perm), and where each group starts (gstart, ng + 1 entries): the integer measures are summed group by group. pos: ng entries of room */
void
cpagg_group_rows(const uint32 *gid, uint32 n, const uint32 *cnt, uint32 ng, uint32 *pos, uint32 *perm, uint32 *gstart)
{
	gstart[0] = 0;
	for (uint32 g = 0; g < ng; g++)
		gstart[g + 1] = gstart[g] + cnt[g];
	memcpy(pos, gstart, ng * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
		perm[pos[gid[i]]++] = i;
}

bool
cpagg_measure_int(const SiftCPaggCol *mc, const uint32 *perm, const uint32 *gstart, uint32 ng, uint32 nrows, SiftCI128 *si, uint32 *nn, int64 *mn, int64 *mx, bool minmax)
{
	const uint64 *nulls = mc->nulls;
	bool		has_nulls = false;

	for (uint32 w = 0; nulls != NULL && !has_nulls && w < (nrows + 63) / 64; w++)
		has_nulls = nulls[w] != 0;
#define PASS(hn, mm, nw) measure_int_pass(mc->key, nulls, perm, gstart, ng, si, nn, mn, mx, hn, mm, nw)
	if (mc->ranged && mc->lo >= -((int64) 1 << 31) && mc->hi < ((int64) 1 << 31))	/* a sum of up to 2^31 keys fits int64 */
	{
		if (has_nulls)
			minmax ? PASS(true, true, true) : PASS(true, false, true);
		else
			minmax ? PASS(false, true, true) : PASS(false, false, true);
	}
	else if (has_nulls)
		minmax ? PASS(true, true, false) : PASS(true, false, false);
	else
		minmax ? PASS(false, true, false) : PASS(false, false, false);
#undef PASS
	return has_nulls;
}

/* Builds the histogram of dim sd over the unit; false when it has more than maxg groups or the dim cannot be used (overflow values, inline text) */
static bool
build_dim(const SiftColSchema *s, const SiftCPaggDim *sd, const SiftCPaggCol *dc, const SiftCPaggCol *cols, uint32 nrows, uint32 maxg,
		  const Scratch *sc, DimBuild *b)
{
	StringInfo	out = &b->blob;
	uint32		ng = 0,
				nullg = UINT32_MAX,
				hsize = sc->hsize,
				mask = hsize - 1,
			   *gid = sc->gid,
			   *cnt = sc->cnt,
			   *nn = sc->nn;
	int64	   *hk = sc->hk,
			   *gkey = sc->gkey;
	int32	   *hg = sc->hg;
	bool		direct;
	int			mm[SIFT_PAGG_MAX_MEAS],
				nm = 0;
	GroupKey   *ord = sc->ord;

	if (!col_usable(dc, nrows) || nrows == 0)
		return false;
	direct = dc->ranged && (uint64) dc->hi - (uint64) dc->lo < hsize;	/* a zone narrower than the table: the key minus lo is the slot */
	memset(hg, 0xFF, hsize * sizeof(int32));
	for (uint32 i = 0; i < nrows; i++)
	{
		uint32		g;

		if (is_null(dc->nulls, i))
		{
			if (nullg == UINT32_MAX)
			{
				if (ng == maxg)
					return false;
				nullg = ng;
				gkey[ng] = 0;
				cnt[ng++] = 0;
			}
			g = nullg;
		}
		else
		{
			int64		k = dc->key[i];
			int32	   *gp;

			if (direct)
				gp = &hg[k - dc->lo];
			else
			{
				uint32		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;

				while (hg[h] >= 0 && hk[h] != k)
					h = (h + 1) & mask;
				gp = &hg[h];
			}
			if (*gp < 0)
			{
				if (ng == maxg)
					return false;
				hk[gp - hg] = k;
				*gp = ng;
				gkey[ng] = k;
				cnt[ng++] = 0;
			}
			g = *gp;
		}
		gid[i] = g;
		cnt[g]++;
	}
	for (uint32 g = 0; g < ng; g++)
		ord[g] = (GroupKey) {gkey[g], g, g == nullg};
	qsort(ord, ng, sizeof(GroupKey), cpagg_cmp_group);
	for (int i = 0; i < sd->nmeas; i++)
		if (sd->meas[i] != sd->leaf && col_usable(&cols[sd->meas[i]], nrows))
			mm[nm++] = sd->meas[i];
	b->ngroups = ng;
	appendStringInfoChar(out, (char) nm);
	sift_col_put_varint(out, ng);
	appendStringInfoChar(out, (nullg == UINT32_MAX ? 0 : PAGG_NULLGROUP) | (sc->minmax ? PAGG_MINMAX : 0));
	for (int i = 0; i < nm; i++)
		sift_col_put_varint(out, mm[i]);
	b->nm = nm;
	{
		int64		prev = 0;
		uint32		nk = ng - (nullg == UINT32_MAX ? 0 : 1);

		for (uint32 i = 0; i < nk; i++)
		{
			if (i == 0)
				put_zz(out, ord[i].key);
			else
				sift_col_put_varint(out, (uint64) ord[i].key - (uint64) prev);
			prev = ord[i].key;
		}
	}
	for (uint32 i = 0; i < ng; i++)
		sift_col_put_varint(out, cnt[ord[i].g]);
	cpagg_group_rows(gid, nrows, cnt, ng, sc->nn, sc->perm, sc->gstart);
	for (int m = 0; m < nm; m++)
	{
		const SiftCPaggCol *mc = &cols[mm[m]];
		bool		fl = s->cols[mm[m]].store == SIFT_COLS_F4 || s->cols[mm[m]].store == SIFT_COLS_F8;
		double	   *sf = sc->sf;
		SiftCI128  *si = sc->si;
		int64	   *mn = sc->mn,
				   *mx = sc->mx;

		memset(nn, 0, ng * sizeof(uint32));
		if (fl)
		{
			for (uint32 i = 0; i < nrows; i++)
			{
				double		v;

				if (is_null(mc->nulls, i))
					continue;
				v = sift_col_f8_from_key(mc->key[i]);
				/* the first value seeds the sum, as float8pl's transition does: a group of -0 sums to -0 */
				sf[gid[i]] = nn[gid[i]]++ == 0 ? v : sf[gid[i]] + v;
			}
			for (uint32 i = 0; i < ng; i++)
				sift_col_put_varint(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
			{
				double		v = nn[ord[i].g] ? sf[ord[i].g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
		}
		else
		{
			if (!cpagg_measure_int(mc, sc->perm, sc->gstart, ng, nrows, si, nn, mn, mx, sc->minmax))
				memcpy(nn, cnt, ng * sizeof(uint32));
			for (uint32 i = 0; i < ng; i++)
				sift_col_put_varint(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
				put_zz128(out, si[ord[i].g]);
			for (int pass = 0; sc->minmax && pass < 2; pass++)
				for (uint32 i = 0; i < ng; i++)
					if (nn[ord[i].g] > 0)
						put_zz(out, pass ? mx[ord[i].g] : mn[ord[i].g]);
		}
	}
	return true;
}

/* The kept dims as a cube blob: the directory of (leaf, bytes), then the bodies; nothing when none is kept */
void
cpagg_emit_cube(const SiftCPaggSpec *spec, const DimBuild *db, int nd, StringInfo out)
{
	int			kept = 0;

	for (int d = 0; d < nd; d++)
		kept += db[d].keep;
	if (kept == 0)
		return;
	appendStringInfoChar(out, (char) kept);
	for (int d = 0; d < nd; d++)
		if (db[d].keep)
		{
			sift_col_put_varint(out, spec->dim[db[d].spec].leaf + db[d].grain * SIFT_PAGG_CAL);
			sift_col_put_varint(out, db[d].blob.len);
		}
	for (int d = 0; d < nd; d++)
		if (db[d].keep)
			appendBinaryStringInfo(out, db[d].blob.data, db[d].blob.len);
}

/*
 * The calendar dim of a date or timestamp leaf: the cells of its finest grain whose groups fit, trying days, months, then years. A grain fits when
 * the unit has at most SIFT_PAGG_CAL_GROUPS cells of it, each standing for SIFT_PAGG_CAL_ROWS rows or more on average, and their bytes fit what is left
 * of the budget of the calendar dims (*left; 0: no budget). A unit with an infinity or a day no cell can start on has none.
 */
static void
build_cal(const SiftColSchema *s, const SiftCPaggDim *sd, const SiftCPaggCol *cols, uint32 nrows, const Scratch *sc, Size *left, bool budget, DimBuild *b)
{
	const SiftCPaggCol *dc = &cols[sd->leaf];
	SiftCPaggCol mc;
	int64	   *days,
			   *cells;

	b->ok = false;
	if (!col_usable(dc, nrows))
		return;
	days = palloc(nrows * sizeof(int64));
	cells = palloc(nrows * sizeof(int64));
	if (cpagg_cal_days(dc, s->cols[sd->leaf].type == SIFT_COLT_DATE, nrows, days))
		for (int g = sd->grain ? sd->grain : SIFT_CAL_DAY; g <= (sd->grain ? sd->grain : SIFT_CAL_YEAR) && !b->ok; g++)
		{
			cpagg_cal_cells(dc, nrows, g, days, cells);
			mc = (SiftCPaggCol) {cells, dc->nulls, NULL, nrows, true};
			resetStringInfo(&b->blob);
			b->grain = g;
			b->ok = build_dim(s, sd, &mc, cols, nrows, SIFT_PAGG_CAL_GROUPS, sc, b);
			if (!sd->grain)
				b->ok = b->ok && (uint64) b->ngroups * SIFT_PAGG_CAL_ROWS <= nrows && (!budget || b->blob.len + 1 <= *left);
		}
	if (b->ok)
	{
		b->keep = true;
		*left -= budget ? b->blob.len : 0;
	}
	pfree(days);
	pfree(cells);
}

/* The histogram of every dim of the spec; the calendar dims are chosen here, in spec order, from the budget they share (cal_budget) */
void
cpagg_build_dims(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, uint32 cubeg, Size cal_budget, DimBuild *db)
{
	uint32		hsize = 16,
				maxg = cubeg;
	Size		left = cal_budget;
	Scratch		sc;

	for (int d = 0; d < spec->ndims; d++)
		if (spec->dim[d].cal)
			maxg = Max(maxg, SIFT_PAGG_CAL_GROUPS);
	while (hsize < 2 * (maxg + 1))
		hsize *= 2;
	sc.hsize = hsize;
	sc.gid = palloc(nrows * sizeof(uint32));
	sc.perm = palloc(Max(nrows, 1) * sizeof(uint32));
	sc.gstart = palloc((maxg + 2) * sizeof(uint32));
	sc.hk = palloc(hsize * sizeof(int64));
	sc.hg = palloc(hsize * sizeof(int32));
	sc.gkey = palloc((maxg + 1) * sizeof(int64));
	sc.cnt = palloc((maxg + 1) * sizeof(uint32));
	sc.nn = palloc((maxg + 1) * sizeof(uint32));
	sc.ord = palloc((maxg + 1) * sizeof(GroupKey));
	sc.sf = palloc((maxg + 1) * sizeof(double));
	sc.si = palloc((maxg + 1) * sizeof(SiftCI128));
	sc.mn = palloc((maxg + 1) * sizeof(int64));
	sc.mx = palloc((maxg + 1) * sizeof(int64));
	for (int d = 0; d < spec->ndims; d++)
	{
		sc.minmax = spec->minmax && !spec->dim[d].no_minmax;
		db[d] = (DimBuild) {.spec = d};
		initStringInfo(&db[d].blob);
		if (spec->dim[d].cal)
		{
			build_cal(s, &spec->dim[d], cols, nrows, &sc, &left, cal_budget > 0, &db[d]);
			continue;
		}
		db[d].ok = build_dim(s, &spec->dim[d], &cols[spec->dim[d].leaf], cols, nrows, cubeg, &sc, &db[d]);
	}
	pfree(sc.gid);
	pfree(sc.perm);
	pfree(sc.gstart);
	pfree(sc.hk);
	pfree(sc.hg);
	pfree(sc.gkey);
	pfree(sc.cnt);
	pfree(sc.nn);
	pfree(sc.ord);
	pfree(sc.sf);
	pfree(sc.si);
	pfree(sc.mn);
	pfree(sc.mx);
}
