/*
 * col/preagg.c - pre-aggregates of a unit: exact integer sums, the mini cube (GROUP BY dim -> count, per measure non-NULL count and sum),
 * the rollups of the lists, their byte formats, their option and their verification. Design and the rules of use: docs/COL-PREAGG.md.
 * Portable (core/host.h only).
 *
 * Cube blob of a unit (all integers LEB128 varints unless said): u8 ndims, a directory of ndims x (leaf, bytes of its body) so that one dim is
 * found without reading the others, then the bodies:
 *     u8 nmeas, ngroups, u8 flags (bit 0: the last group is the NULL group; bit 1, column format minor 7: min and max of the INT measures),
 *     nmeas x measure leaf, keys of the groups with a value in ascending order (first: zigzag, then the positive differences),
 *     counts[ngroups],
 *     per measure: nn[ngroups], then sum[ngroups] (INT measure: zigzag 128-bit; float measure: 8 bytes, the double's bits) and, with the
 *     flag, for an INT measure the smallest value of each group that has one (nn > 0) and then the largest, as zigzag keys
 *
 * Element blob of a unit (column format minor 4): u8 nlists, nlists x (leaf of the list, bytes of its rollup) as varints, then the rollups in that
 * order; the rollup of the rows of the unit (minor 8) is listed as the leaf ncols and has top-level leaves for dims and measures.
 * Rollup of a list (its rows are the elements): u8 ndims, u8 nmeas, ntuples, ndims x dim leaf, nmeas x measure leaf, then
 *     per dim: u8 anynull, (anynull: a bitmap of the tuples that have a NULL there, bit i of byte i / 8), then for the tuples with a value
 *     the key as the zigzag difference to the previous one (the tuples are in ascending order of their keys, dim 0 first, a NULL after the values),
 *     counts[ntuples] (elements of the tuple),
 *     per measure: nn[ntuples], sum[ntuples] (as in the cube), then the smallest value of each tuple that has one and the largest
 *     (INT measure: zigzag of the key; float measure: 8 bytes each)
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/preagg_int.h"
#include "core/cmp.h"

static void
put_u(StringInfo out, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(out, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(out, (char) v);
}

static void
put_zz(StringInfo out, int64 v)
{
	put_u(out, ((uint64) v << 1) ^ (uint64) (v >> 63));
}

static void
put_zz128(StringInfo out, SiftCI128 v)
{
	unsigned __int128 u = ((unsigned __int128) v << 1) ^ (unsigned __int128) (v >> 127);

	while (u >= 0x80)
	{
		appendStringInfoChar(out, (char) ((uint8) u | 0x80));
		u >>= 7;
	}
	appendStringInfoChar(out, (char) (uint8) u);
}

static inline bool
is_null(const uint64 *nulls, uint32 i)
{
	return nulls != NULL && ((nulls[i / 64] >> (i % 64)) & 1);
}

void
sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a)
{
	memset(a, 0, sizeof(*a));
	for (uint32 i = 0; i < c->n; i++)
	{
		SiftCI128	k;

		if (is_null(c->nulls, i))
			continue;
		k = c->key[i];
		a->sum += k;				/* 74,496 values of 2^63 cannot leave int128 */
		if (__builtin_add_overflow(a->sumsq, k * k, &a->sumsq))
			a->flags |= SIFT_CAGG_SUMSQ_OVF;
	}
}

typedef struct GroupKey
{
	int64		key;
	uint32		g;
	uint32		isnull;
} GroupKey;

/* Work areas of one unit's cube, sized by the group cap */
typedef struct Scratch
{
	uint32		hsize;
	uint32	   *gid;				/* [nrows] */
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

typedef struct DimBuild
{
	int			spec;				/* index into spec->dim */
	StringInfoData blob;
	Size		bytes;				/* what the byte budget counts: the blob with the first spec dim lean measures only (all of them without a lean) */
	int			nm;					/* measures in the blob */
	Size		cut[SIFT_PAGG_MAX_MEAS + 1];	/* offset in the blob after the first i measure leaves (cut[0]: after the flags) */
	Size		tail[SIFT_PAGG_MAX_MEAS + 1];	/* offset after the counts (tail[0]) and after each measure's sums */
	bool		ok;
	bool		keep;
	int			grain;				/* a calendar dim: the grain of its cells (SIFT_CAL_*), else 0 */
	uint32		ngroups;
} DimBuild;

static int
cmp_group(const void *a, const void *b)
{
	const GroupKey *x = a,
			   *y = b;

	if (x->isnull != y->isnull)
		return x->isnull ? 1 : -1;
	return SIFT_CMP3(x->key, y->key);
}

/* The writer keeps the overflow bitmap of a leaf once a unit needed it, zeroed for the next ones: no bit set is no overflow value */
static bool
has_ovf(const SiftCPaggCol *c)
{
	for (uint32 w = 0; c->ovf != NULL && w < (c->n + 63) / 64; w++)
		if (c->ovf[w])
			return true;
	return false;
}

static inline bool
col_usable(const SiftCPaggCol *c, uint32 nrows)
{
	return c->ready && c->n == nrows && !has_ovf(c);
}

#define PAGG_USECS_DAY		INT64CONST(86400000000)
#define PAGG_EPOCH_DAYS		10957			/* 1970-01-01 to 2000-01-01 */
#define PAGG_TS_DAYS		106751991		/* the days a timestamp holds, either side of 2000-01-01 (INT64_MAX / USECS_PER_DAY) */
#define PAGG_DATE_DAYS		INT64CONST(2147483000)	/* and a date, which also has infinities */

/* Days since 1970-01-01 of a civil date and the civil month of a day number: the proleptic Gregorian calendar PostgreSQL's date_trunc uses (H. Hinnant's algorithms) */
static int64
civil_days(int64 y, unsigned m)
{
	int64		era;
	unsigned	yoe,
				doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned) (y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
	return era * 146097 + (int64) (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

static void
civil_month(int64 z, int64 *y, unsigned *m)
{
	int64		era;
	unsigned	doe,
				yoe,
				doy,
				mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned) (z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

static int64
day_of(int64 us)
{
	int64		d = us / PAGG_USECS_DAY;

	return us % PAGG_USECS_DAY < 0 ? d - 1 : d;
}

/* the first day a cell may start on: 4713 BC, so that the start of the cell of any day a timestamp holds is a timestamp too */
static int64
cal_min(void)
{
	return civil_days(-4712, 1) - PAGG_EPOCH_DAYS;
}

int64
sift_cpagg_cal_cell(int grain, int64 day)
{
	int64		y;
	unsigned	m;

	if (grain == SIFT_CAL_DAY)
		return day;
	civil_month(day + PAGG_EPOCH_DAYS, &y, &m);
	return civil_days(y, grain == SIFT_CAL_MONTH ? m : 1) - PAGG_EPOCH_DAYS;
}

int64
sift_cpagg_cal_next(int grain, int64 day)
{
	int64		y;
	unsigned	m;

	if (grain == SIFT_CAL_DAY)
		return day + 1;
	civil_month(day + PAGG_EPOCH_DAYS, &y, &m);
	if (grain == SIFT_CAL_MONTH && m < 12)
		return civil_days(y, m + 1) - PAGG_EPOCH_DAYS;
	return civil_days(y + 1, 1) - PAGG_EPOCH_DAYS;
}

bool
sift_cpagg_cal_key_ok(int type, int grain, int64 key)
{
	int64		lim = type == SIFT_COLT_DATE ? PAGG_DATE_DAYS : PAGG_TS_DAYS;

	return grain >= SIFT_CAL_DAY && grain <= SIFT_CAL_YEAR && key >= cal_min() && key <= lim && sift_cpagg_cal_cell(grain, key) == key;
}

bool
sift_cpagg_cal_type(const SiftColSchema *s, int c)
{
	const SiftColDef *d = &s->cols[c];

	return (d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ) && d->store == SIFT_COLS_INT && d->parent < 0;
}

/* The day of each row of a date or timestamp leaf (a timestamptz in UTC); false (the unit has no calendar dim) for an infinity or a day no cell can start on */
static bool
cal_days(const SiftCPaggCol *c, bool date, uint32 nrows, int64 *out)
{
	int64		lo = cal_min(),
				hi = date ? PAGG_DATE_DAYS : PAGG_TS_DAYS;

	for (uint32 i = 0; i < nrows; i++)
	{
		int64		k = c->key[i],
					d;

		if (is_null(c->nulls, i))
		{
			out[i] = 0;
			continue;
		}
		if (date ? (k == PG_INT32_MIN || k == PG_INT32_MAX) : (k == PG_INT64_MAX || k == PG_INT64_MIN))
			return false;
		d = date ? k : day_of(k);
		if (d < lo || d > hi)
			return false;
		out[i] = d;
	}
	return true;
}

/* the cell of a grain of each day of cal_days (the NULLs keep 0) */
static void
cal_cells(const SiftCPaggCol *c, uint32 nrows, int grain, const int64 *days, int64 *out)
{
	int64		last_day = 0,
				last_cell = 0;
	bool		have = false;

	for (uint32 i = 0; i < nrows; i++)
	{
		if (is_null(c->nulls, i))
		{
			out[i] = 0;
			continue;
		}
		if (!have || days[i] != last_day)
		{
			last_cell = sift_cpagg_cal_cell(grain, days[i]);
			last_day = days[i];
			have = true;
		}
		out[i] = last_cell;
	}
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
	int			mm[SIFT_PAGG_MAX_MEAS],
				nm = 0;
	GroupKey   *ord = sc->ord;

	if (!col_usable(dc, nrows) || nrows == 0)
		return false;
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
			uint32		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;

			while (hg[h] >= 0 && hk[h] != k)
				h = (h + 1) & mask;
			if (hg[h] < 0)
			{
				if (ng == maxg)
					return false;
				hk[h] = k;
				hg[h] = ng;
				gkey[ng] = k;
				cnt[ng++] = 0;
			}
			g = hg[h];
		}
		gid[i] = g;
		cnt[g]++;
	}
	for (uint32 g = 0; g < ng; g++)
		ord[g] = (GroupKey) {gkey[g], g, g == nullg};
	qsort(ord, ng, sizeof(GroupKey), cmp_group);
	for (int i = 0; i < sd->nmeas; i++)
		if (sd->meas[i] != sd->leaf && col_usable(&cols[sd->meas[i]], nrows))
			mm[nm++] = sd->meas[i];
	b->ngroups = ng;
	appendStringInfoChar(out, (char) nm);
	put_u(out, ng);
	appendStringInfoChar(out, (nullg == UINT32_MAX ? 0 : PAGG_NULLGROUP) | (sc->minmax ? PAGG_MINMAX : 0));
	b->cut[0] = out->len;
	for (int i = 0; i < nm; i++)
	{
		put_u(out, mm[i]);
		b->cut[i + 1] = out->len;
	}
	b->nm = nm;
	{
		int64		prev = 0;
		uint32		nk = ng - (nullg == UINT32_MAX ? 0 : 1);

		for (uint32 i = 0; i < nk; i++)
		{
			if (i == 0)
				put_zz(out, ord[i].key);
			else
				put_u(out, (uint64) ord[i].key - (uint64) prev);
			prev = ord[i].key;
		}
	}
	for (uint32 i = 0; i < ng; i++)
		put_u(out, cnt[ord[i].g]);
	b->tail[0] = out->len;
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
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
			{
				double		v = nn[ord[i].g] ? sf[ord[i].g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
		}
		else
		{
			memset(si, 0, ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < nrows; i++)
				if (!is_null(mc->nulls, i))
				{
					uint32		g = gid[i];
					int64		v = mc->key[i];

					si[g] += v;
					if (nn[g]++ == 0)
						mn[g] = mx[g] = v;
					else
					{
						mn[g] = Min(mn[g], v);
						mx[g] = Max(mx[g], v);
					}
				}
			for (uint32 i = 0; i < ng; i++)
				put_u(out, nn[ord[i].g]);
			for (uint32 i = 0; i < ng; i++)
				put_zz128(out, si[ord[i].g]);
			for (int pass = 0; sc->minmax && pass < 2; pass++)
				for (uint32 i = 0; i < ng; i++)
					if (nn[ord[i].g] > 0)
						put_zz(out, pass ? mx[ord[i].g] : mn[ord[i].g]);
		}
		b->tail[m + 1] = out->len;
	}
	return true;
}

/* The kept dims as a cube blob: the directory of (leaf, bytes), then the bodies; nothing when none is kept */
static void
emit_cube(const SiftCPaggSpec *spec, const DimBuild *db, int nd, StringInfo out)
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
			put_u(out, spec->dim[db[d].spec].leaf + db[d].grain * SIFT_PAGG_CAL);
			put_u(out, db[d].blob.len);
		}
	for (int d = 0; d < nd; d++)
		if (db[d].keep)
			appendBinaryStringInfo(out, db[d].blob.data, db[d].blob.len);
}

/* the bytes a dim counts against the byte budget: the blob with the first measures only (all of them without a lean) */
static Size
dim_bytes(const SiftCPaggDim *sd, const DimBuild *b)
{
	int			lean = sd->lean > 0 ? Min(sd->lean, b->nm) : b->nm;

	return b->ok && lean < b->nm ? b->cut[lean] + b->tail[lean] - b->cut[b->nm] : b->blob.len;
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
	if (cal_days(dc, s->cols[sd->leaf].type == SIFT_COLT_DATE, nrows, days))
		for (int g = sd->grain ? sd->grain : SIFT_CAL_DAY; g <= (sd->grain ? sd->grain : SIFT_CAL_YEAR) && !b->ok; g++)
		{
			cal_cells(dc, nrows, g, days, cells);
			mc = (SiftCPaggCol) {cells, dc->nulls, NULL, nrows, true};
			resetStringInfo(&b->blob);
			b->grain = g;
			b->ok = build_dim(s, sd, &mc, cols, nrows, SIFT_PAGG_CAL_GROUPS, sc, b);
			b->bytes = dim_bytes(sd, b);
			if (!sd->grain)
				b->ok = b->ok && (uint64) b->ngroups * SIFT_PAGG_CAL_ROWS <= nrows && (!budget || b->bytes + 1 <= *left);
		}
	if (b->ok)
	{
		b->keep = true;
		*left -= budget ? b->bytes : 0;
	}
	pfree(days);
	pfree(cells);
}

/* The histogram of every dim of the spec; the calendar dims are chosen here, in spec order, from the budget they share (cal_budget) */
static void
build_dims(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, uint32 cubeg, Size cal_budget, DimBuild *db)
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
	sc.minmax = spec->minmax;
	for (int d = 0; d < spec->ndims; d++)
	{
		db[d] = (DimBuild) {.spec = d};
		initStringInfo(&db[d].blob);
		if (spec->dim[d].cal)
		{
			build_cal(s, &spec->dim[d], cols, nrows, &sc, &left, cal_budget > 0, &db[d]);
			continue;
		}
		db[d].ok = build_dim(s, &spec->dim[d], &cols[spec->dim[d].leaf], cols, nrows, cubeg, &sc, &db[d]);
		/* the dims are chosen by the size they have with the first measures: the others come with a dim that is kept, as the planner expects */
		db[d].bytes = dim_bytes(&spec->dim[d], &db[d]);
	}
	pfree(sc.gid);
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

typedef struct RuBuild
{
	int			nd,
				nm;
	const SiftCPaggCol *dc[SIFT_PAGG_MAX_RDIMS],
			   *mc[SIFT_PAGG_MAX_MEAS];
	int16		dleaf[SIFT_PAGG_MAX_RDIMS],
				mleaf[SIFT_PAGG_MAX_MEAS];
	bool		mfl[SIFT_PAGG_MAX_MEAS];
	uint32		n,					/* elements */
				cap,				/* the most tuples */
				nt,
				hsize;
	uint32	   *gid,				/* [n] the tuple of each element */
			   *cnt;				/* [cap] */
	int64	   *tkey;				/* [cap * nd] */
	uint8	   *tnull;				/* [cap] bit j: dim j is NULL */
	int32	   *hslot;				/* [hsize] */
} RuBuild;

/* the order of the tuples: dim 0 first, the NULL of a dim after its values; the sort callback has no argument, the backend is one thread */
typedef struct RuOrder
{
	int			nd;
	const int64 *key;
	const uint8 *null;
} RuOrder;

static const RuOrder *ru_order;

static int
cmp_tuple(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	for (int j = 0; j < ru_order->nd; j++)
	{
		bool		nx = ru_order->null[x] >> j & 1,
					ny = ru_order->null[y] >> j & 1;
		int64		kx = ru_order->key[(Size) x * ru_order->nd + j],
					ky = ru_order->key[(Size) y * ru_order->nd + j];

		if (nx != ny)
			return nx ? 1 : -1;
		if (kx != ky)
			return SIFT_CMP3(kx, ky);
	}
	return SIFT_CMP3(x, y);
}

/* The distinct tuples of the dims over the elements, with the elements of each; false past the cap */
static bool
ru_tuples(RuBuild *b)
{
	uint32		mask = b->hsize - 1;

	memset(b->hslot, 0xFF, b->hsize * sizeof(int32));
	b->nt = 0;
	for (uint32 i = 0; i < b->n; i++)
	{
		int64		k[SIFT_PAGG_MAX_RDIMS];
		uint8		nm = 0;
		uint64		h = 0;
		uint32		pos,
					t;

		for (int j = 0; j < b->nd; j++)
		{
			if (is_null(b->dc[j]->nulls, i))
			{
				nm |= 1 << j;
				k[j] = 0;
			}
			else
				k[j] = b->dc[j]->key[i];
			h = (h ^ (uint64) k[j]) * UINT64CONST(0x9E3779B97F4A7C15);
			h ^= h >> 31;
		}
		h = (h ^ nm) * UINT64CONST(0x9E3779B97F4A7C15);
		pos = (uint32) (h >> 24) & mask;
		for (;;)
		{
			int32		sl = b->hslot[pos];

			if (sl < 0)
			{
				if (b->nt == b->cap)
					return false;
				t = b->nt++;
				memcpy(b->tkey + (Size) t * b->nd, k, b->nd * sizeof(int64));
				b->tnull[t] = nm;
				b->cnt[t] = 0;
				b->hslot[pos] = t;
				break;
			}
			if (b->tnull[sl] == nm && memcmp(b->tkey + (Size) sl * b->nd, k, b->nd * sizeof(int64)) == 0)
			{
				t = sl;
				break;
			}
			pos = (pos + 1) & mask;
		}
		b->gid[i] = t;
		b->cnt[t]++;
	}
	return true;
}

/* The blob of the tuples found by ru_tuples (format at the top of the file) */
static void
ru_encode(const RuBuild *b, StringInfo out)
{
	uint32		nt = b->nt,
			   *ord = palloc(nt * sizeof(uint32)),
			   *nn = palloc(nt * sizeof(uint32));
	RuOrder		od = {b->nd, b->tkey, b->tnull};
	SiftCI128  *si = palloc(nt * sizeof(SiftCI128));
	int64	   *mn = palloc(nt * sizeof(int64)),
			   *mx = palloc(nt * sizeof(int64));
	double	   *sf = palloc(nt * sizeof(double)),
			   *fmn = palloc(nt * sizeof(double)),
			   *fmx = palloc(nt * sizeof(double));

	for (uint32 t = 0; t < nt; t++)
		ord[t] = t;
	ru_order = &od;
	qsort(ord, nt, sizeof(uint32), cmp_tuple);
	appendStringInfoChar(out, (char) b->nd);
	appendStringInfoChar(out, (char) b->nm);
	put_u(out, nt);
	for (int j = 0; j < b->nd; j++)
		put_u(out, b->dleaf[j]);
	for (int m = 0; m < b->nm; m++)
		put_u(out, b->mleaf[m]);
	for (int j = 0; j < b->nd; j++)
	{
		bool		any = false;
		int64		prev = 0;

		for (uint32 t = 0; t < nt; t++)
			any |= (b->tnull[t] >> j & 1) != 0;
		appendStringInfoChar(out, any);
		if (any)
		{
			uint32		nb = (nt + 7) / 8,
						at = out->len;

			for (uint32 i = 0; i < nb; i++)
				appendStringInfoChar(out, 0);
			for (uint32 i = 0; i < nt; i++)
				if (b->tnull[ord[i]] >> j & 1)
					out->data[at + i / 8] |= (char) (1 << (i % 8));
		}
		for (uint32 i = 0; i < nt; i++)
		{
			int64		k;

			if (b->tnull[ord[i]] >> j & 1)
				continue;
			k = b->tkey[(Size) ord[i] * b->nd + j];
			put_zz(out, (int64) ((uint64) k - (uint64) prev));
			prev = k;
		}
	}
	for (uint32 i = 0; i < nt; i++)
		put_u(out, b->cnt[ord[i]]);
	for (int m = 0; m < b->nm; m++)
	{
		const SiftCPaggCol *mc = b->mc[m];

		memset(nn, 0, nt * sizeof(uint32));
		for (uint32 i = 0; i < b->n; i++)
		{
			uint32		g = b->gid[i];

			if (is_null(mc->nulls, i))
				continue;
			if (b->mfl[m])
			{
				double		v = sift_col_f8_from_key(mc->key[i]);

				if (nn[g]++ == 0)
					sf[g] = fmn[g] = fmx[g] = v;
				else
				{
					sf[g] += v;
					fmn[g] = v < fmn[g] ? v : fmn[g];
					fmx[g] = v > fmx[g] ? v : fmx[g];
				}
			}
			else
			{
				int64		v = mc->key[i];

				if (nn[g]++ == 0)
				{
					si[g] = 0;
					mn[g] = mx[g] = v;
				}
				si[g] += v;
				mn[g] = Min(mn[g], v);
				mx[g] = Max(mx[g], v);
			}
		}
		for (uint32 i = 0; i < nt; i++)
			put_u(out, nn[ord[i]]);
		for (uint32 i = 0; i < nt; i++)
		{
			uint32		g = ord[i];

			if (b->mfl[m])
			{
				double		v = nn[g] ? sf[g] : 0;

				appendBinaryStringInfo(out, (const char *) &v, sizeof(double));
			}
			else
				put_zz128(out, nn[g] ? si[g] : 0);
		}
		for (int pass = 0; pass < 2; pass++)
			for (uint32 i = 0; i < nt; i++)
			{
				uint32		g = ord[i];

				if (nn[g] == 0)
					continue;
				if (b->mfl[m])
					appendBinaryStringInfo(out, (const char *) (pass ? &fmx[g] : &fmn[g]), sizeof(double));
				else
					put_zz(out, pass ? mx[g] : mn[g]);
			}
	}
	pfree(ord);
	pfree(nn);
	pfree(si);
	pfree(mn);
	pfree(mx);
	pfree(sf);
	pfree(fmn);
	pfree(fmx);
}

/* The distinct values (a NULL one more) of an element column, or limit + 1 when there are more than limit */
static uint32
ru_distinct(const SiftCPaggCol *c, uint32 n, uint32 limit, int64 *hk, uint8 *hu, uint32 hsize)
{
	uint32		mask = hsize - 1,
				nd = 0;
	bool		nullseen = false;

	memset(hu, 0, hsize);
	for (uint32 i = 0; i < n; i++)
	{
		int64		k;
		uint32		h;

		if (is_null(c->nulls, i))
		{
			if (!nullseen && ++nd > limit)
				return limit + 1;
			nullseen = true;
			continue;
		}
		k = c->key[i];
		h = (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;
		while (hu[h] && hk[h] != k)
			h = (h + 1) & mask;
		if (!hu[h])
		{
			if (++nd > limit)
				return limit + 1;
			hu[h] = 1;
			hk[h] = k;
		}
	}
	return nd;
}

static int
cmp_i16(const void *a, const void *b)
{
	return *(const int16 *) a - *(const int16 *) b;
}

/*
 * Builds the rollup of one list over the unit (of the rows of the unit when ru->list is -1: the dims are top-level leaves and the cap is a 10th of
 * the rows, at most SIFT_PAGG_ROWS_CELLS). fixed: exactly the dims of ru (the cap of tuples is the largest one, an explicit rollup(...) or
 * the check of a stored one); else the dims go from the most distinct values down until the tuples are at most a 32nd of the elements, and
 * the blob fits room (limited: the budget of the list). false when no rollup fits or the list cannot be used.
 */
static bool
build_rollup(const SiftColSchema *s, const SiftCPaggRollup *ru, const SiftCPaggCol *cols, uint32 nrows, bool fixed, bool limited, Size room, StringInfo out)
{
	RuBuild		b;
	int			cand[SIFT_PAGG_MAX_RDIMS],
				nc = 0,
				nmeas = 0;
	uint32		dist[SIFT_PAGG_MAX_RDIMS],
				n = 0,
				hs = 16,
				mhs = 16;
	int64	   *hk;
	uint8	   *hu;
	bool		done = false,
				rows = ru->list < 0;
	StringInfoData tmp;

	if ((!rows && !col_usable(&cols[ru->list], nrows)) || (limited && room == 0))
		return false;
	for (int j = 0; j < ru->ndims; j++)
	{
		const SiftCPaggCol *c = &cols[ru->dim[j]];

		if (!c->ready || has_ovf(c) || (nc > 0 && c->n != n) || (rows && c->n != nrows))
			continue;
		n = c->n;
		cand[nc++] = j;
	}
	if (n == 0 || nc == 0)
		return false;
	b.n = n;
	b.cap = rows ? Min((uint32) SIFT_PAGG_ROWS_CELLS, Max((uint32) SIFT_PAGG_ROLLUP_MIN, nrows / SIFT_PAGG_ROWS_RATIO)) : fixed ? SIFT_PAGG_ROLLUP_TUPLES : Min((uint32) SIFT_PAGG_ROLLUP_TUPLES, Max((uint32) SIFT_PAGG_ROLLUP_MIN, n / 32));
	while (mhs < 2 * (b.cap + 2))
		mhs *= 2;
	hk = palloc(mhs * sizeof(int64));
	hu = palloc(mhs);
	for (int i = 0; i < nc; i++)
		dist[i] = ru_distinct(&cols[ru->dim[cand[i]]], n, b.cap, hk, hu, mhs);
	pfree(hk);
	pfree(hu);
	for (int m = 0; m < ru->nmeas && nmeas < SIFT_PAGG_MAX_MEAS; m++)
	{
		const SiftCPaggCol *c = &cols[ru->meas[m]];

		if (c->ready && !has_ovf(c) && c->n == n)
		{
			b.mc[nmeas] = c;
			b.mleaf[nmeas] = ru->meas[m];
			b.mfl[nmeas++] = s->cols[ru->meas[m]].store == SIFT_COLS_F4 || s->cols[ru->meas[m]].store == SIFT_COLS_F8;
		}
	}
	b.nm = nmeas;
	/* the order of the attempts: the dims of fewer distinct values first (the position in ru breaks a tie); the stored order is by leaf */
	for (int i = 1; i < nc; i++)
		for (int j = i; j > 0 && dist[j] < dist[j - 1]; j--)
		{
			uint32		d = dist[j];
			int			c = cand[j];

			dist[j] = dist[j - 1];
			cand[j] = cand[j - 1];
			dist[j - 1] = d;
			cand[j - 1] = c;
		}
	{
		uint64		prod = 1;
		int			k = 0;

		if (fixed)
			k = nc;
		else
		{
			while (k < nc && dist[k] <= b.cap && prod * dist[k] <= (uint64) b.cap * 8)
				prod *= dist[k++];
			k = Max(k, 1);
		}
		while (hs < 2 * (b.cap + 1))
			hs *= 2;
		b.hsize = hs;
		b.gid = palloc(n * sizeof(uint32));
		b.cnt = palloc((b.cap + 1) * sizeof(uint32));
		b.tkey = palloc((Size) (b.cap + 1) * SIFT_PAGG_MAX_RDIMS * sizeof(int64));
		b.tnull = palloc(b.cap + 1);
		b.hslot = palloc(hs * sizeof(int32));
		initStringInfo(&tmp);
		for (; k >= 1 && !done; k--)
		{
			int16		leaves[SIFT_PAGG_MAX_RDIMS];

			if (!fixed && dist[k - 1] > b.cap)
				continue;
			for (int i = 0; i < k; i++)
				leaves[i] = ru->dim[cand[i]];
			qsort(leaves, k, sizeof(int16), cmp_i16);
			b.nd = k;
			for (int i = 0; i < k; i++)
			{
				b.dleaf[i] = leaves[i];
				b.dc[i] = &cols[leaves[i]];
			}
			if (ru_tuples(&b))
			{
				resetStringInfo(&tmp);
				ru_encode(&b, &tmp);
				if (!limited || tmp.len <= room)
				{
					appendBinaryStringInfo(out, tmp.data, tmp.len);
					done = true;
				}
			}
			if (fixed)
				break;
		}
	}
	pfree(b.gid);
	pfree(b.cnt);
	pfree(b.tkey);
	pfree(b.tnull);
	pfree(b.hslot);
	pfree(tmp.data);
	return done;
}

/* The element blob: the rollups of the spec's lists that a unit can hold, in ascending list order; limited: each within room bytes (a list's own budget, the cube's is not shared) */
static void
emit_rollups(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, bool limited, Size room, StringInfo out)
{
	StringInfoData body[SIFT_PAGG_MAX_ROLLUPS];
	int			list[SIFT_PAGG_MAX_ROLLUPS],
				nl = 0;

	for (int i = 0; i < spec->nrollups; i++)
	{
		bool		rows = spec->rollup[i].list < 0;

		initStringInfo(&body[nl]);
		if (build_rollup(s, &spec->rollup[i], cols, nrows, spec->rfixed || rows, limited && !rows, room, &body[nl]))
			list[nl++] = spec->rollup[i].list;
		else
			pfree(body[nl].data);
	}
	if (nl == 0)
		return;
	appendStringInfoChar(out, (char) nl);
	for (int i = 0; i < nl; i++)
	{
		put_u(out, list[i] < 0 ? s->ncols : list[i]);	/* the rows of the unit: the number of leaves, which no list is */
		put_u(out, body[i].len);
	}
	for (int i = 0; i < nl; i++)
	{
		appendBinaryStringInfo(out, body[i].data, body[i].len);
		pfree(body[i].data);
	}
}

void
sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
				StringInfo out, StringInfo eout)
{
	DimBuild	db[SIFT_PAGG_MAX_DIMS];
	Size		budget = 0,
				cal_budget = 0,		/* the calendar dims have a larger budget of their own: a cell is a few dozen bytes, for the question most asked of a time column */
				used = 0;

	if (spec->off || nrows == 0 || (spec->ndims == 0 && (eout == NULL || spec->nrollups == 0)))
		return;
	if (spec->permille > 0)
	{
		budget = Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * spec->permille);
		cal_budget = Max((Size) SIFT_PAGG_MIN_BUDGET * 2, unit_bytes / 1000 * SIFT_PAGG_CAL_PERMILLE);
	}
	if (spec->ndims > 0)
	{
		build_dims(s, spec, cols, nrows, Min(spec->max_groups, SIFT_PAGG_CUBE_GROUPS), cal_budget, db);
		/* smallest dims first under the budget: the cheapest histograms answer the most questions per byte */
		for (int round = 0; round < spec->ndims; round++)
		{
			int			best = -1;

			for (int d = 0; d < spec->ndims; d++)
				if (db[d].ok && !db[d].keep && (best < 0 || db[d].bytes < db[best].bytes))
					best = d;
			if (best < 0)
				break;
			if (budget > 0 && used + db[best].bytes + 1 > budget)
			{
				db[best].ok = false;
				continue;
			}
			db[best].keep = true;
			used += db[best].bytes;
		}
		emit_cube(spec, db, spec->ndims, out);
		for (int d = 0; d < spec->ndims; d++)
			pfree(db[d].blob.data);
	}
	if (eout && spec->nrollups > 0)
		emit_rollups(s, spec, cols, nrows, !spec->rfixed, Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * SIFT_PAGG_DEF_PERMILLE), eout);
}
