/*
 * col/preagg_read.c - the pre-aggregates read: varint decoding of the cube and rollup blobs, and their verification against the chunks.
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/preagg_int.h"
#include "core/cmp.h"

typedef struct Rd
{
	const uint8 *p;
	const uint8 *end;
	bool		bad;
} Rd;

static uint64
get_u(Rd *r)
{
	uint64		v = 0;

	for (int sh = 0; sh < 70; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		v |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return v;
	}
	r->bad = true;
	return 0;
}

static int64
get_zz(Rd *r)
{
	uint64		u = get_u(r);

	return (int64) (u >> 1) ^ -(int64) (u & 1);
}

static SiftCI128
get_zz128(Rd *r)
{
	unsigned __int128 u = 0;
	uint64		w = 0;
	const uint8 *p = r->p;

	/* nearly every sum fits 63 bits: no 128-bit shifts for those */
	for (int sh = 0; sh < 63; sh += 7)
	{
		uint8		b;

		if (p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *p++;
		w |= (uint64) (b & 0x7F) << sh;
		if (!(b & 0x80))
		{
			r->p = p;
			return (SiftCI128) ((int64) (w >> 1) ^ -(int64) (w & 1));
		}
	}
	for (int sh = 0; sh < 133; sh += 7)
	{
		uint8		b;

		if (r->p >= r->end)
		{
			r->bad = true;
			return 0;
		}
		b = *r->p++;
		u |= (unsigned __int128) (b & 0x7F) << sh;
		if (!(b & 0x80))
			return (SiftCI128) (u >> 1) ^ -(SiftCI128) (u & 1);
	}
	r->bad = true;
	return 0;
}

/* One dim's body (cube blob format above, after the directory): false when it is not consistent with the schema */
static bool
decode_dim(const SiftColSchema *s, Rd *r, int leaf, SiftCHistDim *hd)
{
	uint32		ng,
				nk;
	uint8		fl;

	memset(hd, 0, sizeof(*hd));
	hd->leaf = leaf;
	if (r->p + 2 > r->end)
		return false;
	hd->nmeas = *r->p++;
	ng = (uint32) get_u(r);
	if (r->p >= r->end || hd->nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	hd->ngroups = ng;
	fl = *r->p++;
	if (fl & ~(PAGG_NULLGROUP | PAGG_MINMAX))
		return false;
	hd->has_null = (fl & PAGG_NULLGROUP) != 0;
	hd->minmax = (fl & PAGG_MINMAX) != 0;
	for (int m = 0; m < hd->nmeas; m++)
	{
		uint64		ml = get_u(r);

		if (ml >= (uint64) s->ncols)
			return false;
		hd->meas[m] = (int16) ml;
	}
	hd->key = palloc0(ng * sizeof(int64));
	hd->count = palloc(ng * sizeof(uint32));
	nk = ng - (hd->has_null ? 1 : 0);
	for (uint32 i = 0; i < nk; i++)
		hd->key[i] = i == 0 ? get_zz(r) : (int64) ((uint64) hd->key[i - 1] + get_u(r));
	/* the keys of a calendar dim are the first days of cells of its grain, in ascending order: nothing else is a group of a cell */
	for (uint32 i = 0; leaf >= SIFT_PAGG_CAL && i < nk; i++)
		if (!sift_cpagg_cal_key_ok(s->cols[leaf % SIFT_PAGG_CAL].type, leaf / SIFT_PAGG_CAL, hd->key[i]) || (i > 0 && hd->key[i] <= hd->key[i - 1]))
			return false;
	for (uint32 i = 0; i < ng; i++)
		hd->count[i] = (uint32) get_u(r);
	for (int m = 0; m < hd->nmeas; m++)
	{
		const SiftColDef *md = &s->cols[hd->meas[m]];

		hd->nn[m] = palloc(ng * sizeof(uint32));
		for (uint32 i = 0; i < ng; i++)
			hd->nn[m][i] = (uint32) get_u(r);
		if (md->store == SIFT_COLS_F4 || md->store == SIFT_COLS_F8)
		{
			if ((Size) (r->end - r->p) < (Size) ng * sizeof(double))
				return false;
			hd->sumf[m] = palloc(ng * sizeof(double));
			memcpy(hd->sumf[m], r->p, ng * sizeof(double));
			r->p += ng * sizeof(double);
		}
		else
		{
			hd->sum[m] = palloc(ng * sizeof(SiftCI128));
			for (uint32 i = 0; i < ng; i++)
				hd->sum[m][i] = get_zz128(r);
			if (hd->minmax)
			{
				hd->min[m] = palloc0(ng * sizeof(int64));
				hd->max[m] = palloc0(ng * sizeof(int64));
				for (int pass = 0; pass < 2; pass++)
					for (uint32 i = 0; i < ng; i++)
						if (hd->nn[m][i] > 0)
							(pass ? hd->max[m] : hd->min[m])[i] = get_zz(r);
				for (uint32 i = 0; i < ng; i++)
					if (hd->min[m][i] > hd->max[m][i])
						return false;
			}
		}
	}
	return !r->bad;
}

/* is l the directory leaf of the cells of a date or timestamp leaf of the schema? */
static bool
is_cal_dim(const SiftColSchema *s, uint64 l)
{
	return l >= SIFT_PAGG_CAL && l / SIFT_PAGG_CAL <= SIFT_CAL_YEAR && l % SIFT_PAGG_CAL < (uint64) s->ncols && sift_cpagg_cal_type(s, (int) (l % SIFT_PAGG_CAL));
}

/*
 * The directory of a blob: leaf and body bytes of each entry (at most max of them; the dims of a cube, or the lists of an element blob, where the
 * leaf ncols is the rows of the unit; cube: the dims of a cube, which may name calendar cells, else a list); body points at the first body. false when inconsistent
 */
static bool
read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, bool cube, int *nd, int *leaf, uint32 *blen, const uint8 **body)
{
	Rd			r = {blob, blob + len, false};
	uint64		total = 0;

	if (len == 0)
		return false;
	*nd = *r.p++;
	if (*nd == 0 || *nd > max)
		return false;
	for (int d = 0; d < *nd; d++)
	{
		uint64		l = get_u(&r),
					n = get_u(&r);

		/* a cube's directory may name the cells of a date or timestamp leaf (column format minor 8), an element blob's the rows of the unit (leaf ncols, minor 9) */
		if (r.bad || n == 0 || n > len || (l >= (uint64) s->ncols + !cube && !(cube && is_cal_dim(s, l))))
			return false;
		leaf[d] = l == (uint64) s->ncols ? -1 : (int) l;
		blen[d] = (uint32) n;
		total += n;
	}
	*body = r.p;
	return (uint64) (r.end - r.p) == total;
}

bool
sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				leaf[SIFT_PAGG_MAX_DIMS];
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;
	bool		ok = false;

	memset(h, 0, sizeof(*h));
	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
		goto done;
	for (int d = 0; d < nd; d++)
	{
		Rd			r = {p, p + blen[d], false};

		if (!decode_dim(s, &r, leaf[d], &h->dim[d]) || r.p != r.end)
			goto done;
		p += blen[d];
	}
	h->ndims = nd;
	ok = true;
done:
	MemoryContextSwitchTo(old);
	return ok;
}

int
sift_cpagg_decode_dim(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, int leaf, SiftCHistDim *out)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nd,
				lf[SIFT_PAGG_MAX_DIMS],
				res = -1;
	uint32		blen[SIFT_PAGG_MAX_DIMS];
	const uint8 *p;

	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, lf, blen, &p))
		goto done;
	res = 0;
	for (int d = 0; d < nd; d++)
	{
		/* SIFT_PAGG_CAL + a leaf: its calendar dim, whatever the grain of the unit */
		if (lf[d] == leaf || (leaf >= SIFT_PAGG_CAL && lf[d] >= SIFT_PAGG_CAL && lf[d] % SIFT_PAGG_CAL == leaf % SIFT_PAGG_CAL))
		{
			Rd			r = {p, p + blen[d], false};

			res = decode_dim(s, &r, lf[d], out) && r.p == r.end ? 1 : -1;
			break;
		}
		p += blen[d];
	}
done:
	MemoryContextSwitchTo(old);
	return res;
}

const uint8 *
sift_cpagg_elem_cube(const SiftColSchema *s, const uint8 *eblob, uint32 elen, int group, uint32 *len)
{
	int			nl,
				list[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	*len = 0;
	if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nl, list, blen, &p))
	{
		*len = UINT32_MAX;
		return NULL;
	}
	for (int i = 0; i < nl; p += blen[i++])
		if (list[i] == group)
		{
			*len = blen[i];
			return p;
		}
	return NULL;
}

/* The flags and the measure leaves of the body of a dim (the header decode_dim reads); false when it is not one of this schema */
static bool
peek_dim(const SiftColSchema *s, const uint8 *p, uint32 len, uint8 *flags, int16 *meas, int *nmeas)
{
	Rd			r = {p, p + len, false};
	uint64		ng;

	if (len < 3)
		return false;
	*nmeas = *r.p++;
	ng = get_u(&r);
	if (r.bad || r.p >= r.end || *nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	*flags = *r.p++;
	for (int m = 0; m < *nmeas; m++)
	{
		uint64		l = get_u(&r);

		if (r.bad || l >= (uint64) s->ncols)
			return false;
		meas[m] = (int16) l;
	}
	return true;
}

/* The dim and measure leaves of the head of a rollup (the head read_head reads) */
static bool
peek_rollup(const SiftColSchema *s, const uint8 *p, uint32 len, int *nd, int16 *dim, int *nm, int16 *meas)
{
	Rd			r = {p, p + len, false};
	uint64		nt;

	if (len < 3)
		return false;
	*nd = *r.p++;
	*nm = *r.p++;
	nt = get_u(&r);
	if (*nd == 0 || *nd > SIFT_PAGG_MAX_RDIMS || *nm > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > PAGG_MAX_TUPLES)
		return false;
	for (int j = 0; j < *nd + *nm; j++)
	{
		uint64		l = get_u(&r);

		if (r.bad || l >= (uint64) s->ncols)
			return false;
		(j < *nd ? dim : meas)[j < *nd ? j : j - *nd] = (int16) l;
	}
	return true;
}

/*
 * The spec that builds, over other rows of the unit, the dims and rollups its own blobs hold: the same leaves, measures and calendar grains, no byte budget
 * and no minimum and maximum. false when a blob does not decode or holds something the spec does not name.
 */
bool
sift_cpagg_mirror(const SiftColSchema *s, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, SiftCPaggSpec *out)
{
	int			nd;
	int			leaf[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	*out = *spec;
	out->ndims = out->nrollups = 0;
	out->fixed = out->rfixed = true;
	out->minmax = false;
	out->permille = 0;
	if (len > 0)
	{
		if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
			return false;
		for (int d = 0; d < nd; p += blen[d++])
		{
			SiftCPaggDim *to;
			int16		meas[SIFT_PAGG_MAX_MEAS];
			int			nm,
						k = 0;
			uint8		fl;

			while (k < spec->ndims && (spec->dim[k].cal ? leaf[d] < SIFT_PAGG_CAL || leaf[d] % SIFT_PAGG_CAL != spec->dim[k].leaf : spec->dim[k].leaf != leaf[d]))
				k++;
			if (k == spec->ndims || !peek_dim(s, p, blen[d], &fl, meas, &nm))
				return false;
			to = &out->dim[out->ndims++];
			*to = spec->dim[k];
			to->grain = spec->dim[k].cal ? leaf[d] / SIFT_PAGG_CAL : 0;
			to->nmeas = nm;
			memcpy(to->meas, meas, nm * sizeof(int16));
		}
	}
	if (elen > 0)
	{
		if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nd, leaf, blen, &p))
			return false;
		for (int i = 0; i < nd; p += blen[i++])
		{
			SiftCPaggRollup *to;
			int16		dim[SIFT_PAGG_MAX_RDIMS],
						meas[SIFT_PAGG_MAX_MEAS];
			int			nrd,
						nm,
						k = 0;

			while (k < spec->nrollups && spec->rollup[k].list != leaf[i])
				k++;
			if (k == spec->nrollups || !peek_rollup(s, p, blen[i], &nrd, dim, &nm, meas))
				return false;
			to = &out->rollup[out->nrollups++];
			*to = spec->rollup[k];
			to->ndims = nrd;
			memcpy(to->dim, dim, nrd * sizeof(int16));
			to->nmeas = nm;
			memcpy(to->meas, meas, nm * sizeof(int16));
		}
	}
	return true;
}

/* Are all of have among the spec's (and, when exact, no other)? */
static bool
leaves_within(const int16 *have, int nhave, const int16 *spec, int nspec, bool exact)
{
	if (exact && nhave != nspec)
		return false;
	for (int i = 0; i < nhave; i++)
	{
		int			j = 0;

		while (j < nspec && spec[j] != have[i])
			j++;
		if (j == nspec)
			return false;
	}
	return true;
}

void
sift_cpagg_census(const SiftColSchema *s, const SiftCPaggSpec *spec, bool minmax, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
				  SiftCPaggCensus *c)
{
	bool		odd = false;
	int			nd;
	int			leaf[SIFT_PAGG_SPEC_DIMS];
	uint32		blen[SIFT_PAGG_SPEC_DIMS];
	const uint8 *p;

	c->units++;
	if (len > 0 && !read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
		odd = true;
	else if (len > 0)
		for (int d = 0; d < nd; p += blen[d++])
		{
			int16		meas[SIFT_PAGG_MAX_MEAS];
			int			nm,
						k = 0;
			uint8		fl;

			while (k < spec->ndims && (spec->dim[k].cal ? leaf[d] < SIFT_PAGG_CAL || leaf[d] % SIFT_PAGG_CAL != spec->dim[k].leaf : spec->dim[k].leaf != leaf[d]))
				k++;
			if (k == spec->ndims || !peek_dim(s, p, blen[d], &fl, meas, &nm))
			{
				odd = true;
				continue;
			}
			c->dim[k]++;
			c->mm[k] += (fl & PAGG_MINMAX) != 0;
			odd |= ((fl & PAGG_MINMAX) != 0) != minmax || !leaves_within(meas, nm, spec->dim[k].meas, spec->dim[k].nmeas, spec->fixed);
		}
	if (elen > 0 && !read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nd, leaf, blen, &p))
		odd = true;
	else if (elen > 0)
		for (int i = 0; i < nd; p += blen[i++])
		{
			int16		dim[SIFT_PAGG_MAX_RDIMS],
						meas[SIFT_PAGG_MAX_MEAS];
			int			nrd,
						nm,
						k = 0;

			while (k < spec->nrollups && spec->rollup[k].list != leaf[i])
				k++;
			if (k == spec->nrollups || !peek_rollup(s, p, blen[i], &nrd, dim, &nm, meas))
			{
				odd = true;
				continue;
			}
			c->roll[k]++;
			odd |= !leaves_within(dim, nrd, spec->rollup[k].dim, spec->rollup[k].ndims, spec->rfixed || spec->rollup[k].list < 0) ||
				!leaves_within(meas, nm, spec->rollup[k].meas, spec->rollup[k].nmeas, false);
		}
	c->odd += odd;
}

int
sift_cpagg_meas_slot(const SiftCHistDim *d, int m)
{
	for (int i = 0; i < d->nmeas; i++)
		if (d->meas[i] == m)
			return i;
	return -1;
}

/* the varints of n values are passed over */
static void
skip_u(Rd *r, uint32 n)
{
	while (n-- > 0)
	{
		const uint8 *p = r->p;

		while (p < r->end && (*p & 0x80))
			p++;
		if (p >= r->end)
		{
			r->bad = true;
			return;
		}
		r->p = p + 1;
	}
}

static bool
need_dim(const SiftCRollupNeed *need, int leaf)
{
	for (int i = 0; need && i < need->ndims; i++)
		if (need->dim[i] == leaf)
			return true;
	return need == NULL;
}

/* the position of the measure leaf in need, -1 if it is skipped (need NULL: every measure, whole) */
static int
need_meas(const SiftCRollupNeed *need, int leaf)
{
	for (int i = 0; need && i < need->nmeas; i++)
		if (need->meas[i].leaf == leaf)
			return i;
	return -1;
}

uint32
sift_cpagg_rollup_tuples(const uint8 *blob, uint32 len)
{
	Rd			rd = {blob, blob + len, false};
	uint64		nt;

	if (len < 3)
		return 0;
	rd.p += 2;
	nt = get_u(&rd);
	return rd.bad || nt > PAGG_MAX_TUPLES ? 0 : (uint32) nt;
}

bool
sift_cpagg_rollup_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCRollup *r)
{
	return sift_cpagg_rollup_decode_need(s, blob, len, mcxt, NULL, r);
}

/* n doubles are passed over; false when the blob ends first */
static bool
skip_f8(Rd *r, Size n)
{
	if ((Size) (r->end - r->p) < n * sizeof(double))
		return false;
	r->p += n * sizeof(double);
	return true;
}

/* the count, the dimension and the measure leaves of a rollup */
static bool
read_head(const SiftColSchema *s, Rd *rd, SiftCRollup *r)
{
	uint32		nt;

	if (rd->end - rd->p < 3)
		return false;
	r->ndims = *rd->p++;
	r->nmeas = *rd->p++;
	nt = (uint32) get_u(rd);
	if (r->ndims == 0 || r->ndims > SIFT_PAGG_MAX_RDIMS || r->nmeas > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > PAGG_MAX_TUPLES)
		return false;
	r->ntuples = nt;
	for (int j = 0; j < r->ndims; j++)
	{
		uint64		l = get_u(rd);

		/* the dims of a list's rollup are leaves of that list, those of the rows' are top-level leaves: all of one parent */
		if (rd->bad || l >= (uint64) s->ncols || (j > 0 && r->list != s->cols[l].parent))
			return false;
		r->dim[j] = (int16) l;
		r->list = s->cols[l].parent;
	}
	if (r->list < 0 ? nt > SIFT_PAGG_ROWS_CELLS : nt > SIFT_PAGG_ROLLUP_TUPLES)
		return false;
	for (int m = 0; m < r->nmeas; m++)
	{
		uint64		l = get_u(rd);

		if (rd->bad || l >= (uint64) s->ncols || s->cols[l].parent != r->list)
			return false;
		r->meas[m] = (int16) l;
	}
	return true;
}

/* the null flags and the delta coded keys of dimension j; passed over unless want */
static bool
read_dim(Rd *rd, SiftCRollup *r, int j, bool want)
{
	uint32		nt = r->ntuples,
				nnull = 0;
	int64		prev = 0;
	uint8	   *nulls = NULL;

	if (rd->p >= rd->end || *rd->p > 1)
		return false;
	if (*rd->p++)
	{
		uint32		nb = (nt + 7) / 8;

		if ((uint32) (rd->end - rd->p) < nb)
			return false;
		nulls = palloc0(nt);
		for (uint32 t = 0; t < nt; t++)
			nnull += nulls[t] = rd->p[t / 8] >> (t % 8) & 1;
		rd->p += nb;
	}
	if (!want)
	{
		skip_u(rd, nt - nnull);
		return true;
	}
	r->key[j] = palloc0(nt * sizeof(int64));
	r->isnull[j] = nulls;
	for (uint32 t = 0; t < nt; t++)
	{
		if (nulls && nulls[t])
			continue;
		prev = (int64) ((uint64) prev + (uint64) get_zz(rd));
		r->key[j][t] = prev;
	}
	return true;
}

/* the non-null counts of measure m, and how many tuples have any */
static uint32
read_nn(Rd *rd, SiftCRollup *r, int m, bool want)
{
	uint32		nnz = 0;

	if (want)
		r->nn[m] = palloc(r->ntuples * sizeof(uint32));
	for (uint32 t = 0; t < r->ntuples; t++)
	{
		uint32		n = (uint32) get_u(rd);

		if (want)
			r->nn[m][t] = n;
		nnz += n != 0;
	}
	return nnz;
}

/* the sums of measure m (doubles for a float measure, else 128 bit integers); passed over unless want */
static bool
read_sums(Rd *rd, SiftCRollup *r, int m, bool fl, bool want)
{
	uint32		nt = r->ntuples;
	const uint8 *at = rd->p;

	if (fl)
	{
		if (!skip_f8(rd, nt))
			return false;
		if (want)
			memcpy(r->sumf[m] = palloc(nt * sizeof(double)), at, nt * sizeof(double));
	}
	else if (!want)
		skip_u(rd, nt);
	else
	{
		r->sum[m] = palloc(nt * sizeof(SiftCI128));
		for (uint32 t = 0; t < nt; t++)
			r->sum[m][t] = get_zz128(rd);
	}
	return true;
}

/* the minima then the maxima of measure m over its tuples with a value (nnz); passed over unless want */
static bool
read_minmax(Rd *rd, SiftCRollup *r, int m, bool fl, bool want, uint32 nnz)
{
	if (!want)
	{
		if (fl)
			return skip_f8(rd, (Size) nnz * 2);
		skip_u(rd, nnz * 2);
		return true;
	}
	if (fl)
	{
		r->minf[m] = palloc0(r->ntuples * sizeof(double));
		r->maxf[m] = palloc0(r->ntuples * sizeof(double));
	}
	else
	{
		r->min[m] = palloc0(r->ntuples * sizeof(int64));
		r->max[m] = palloc0(r->ntuples * sizeof(int64));
	}
	for (int pass = 0; pass < 2; pass++)
		for (uint32 t = 0; t < r->ntuples; t++)
		{
			if (r->nn[m][t] == 0)
				continue;
			if (!fl)
				(pass ? r->max[m] : r->min[m])[t] = get_zz(rd);
			else if (!skip_f8(rd, 1))
				return false;
			else
				memcpy(&(pass ? r->maxf[m] : r->minf[m])[t], rd->p - sizeof(double), sizeof(double));
		}
	return true;
}

static bool
read_meas(const SiftColSchema *s, Rd *rd, SiftCRollup *r, int m, const SiftCRollupNeed *need)
{
	bool		fl = s->cols[r->meas[m]].store == SIFT_COLS_F4 || s->cols[r->meas[m]].store == SIFT_COLS_F8;
	int			k = need_meas(need, r->meas[m]);
	bool		want = need == NULL || k >= 0;
	uint32		nnz = read_nn(rd, r, m, want);

	return read_sums(rd, r, m, fl, need == NULL || (k >= 0 && need->meas[k].sums)) &&
		read_minmax(rd, r, m, fl, need == NULL || (k >= 0 && need->meas[k].minmax), nnz);
}

/* what a rollup written from rows cannot break: a tuple stands for at least one element, a measure has no more values than the tuple has elements, and min <= max */
static bool
rollup_sound(const SiftCRollup *r)
{
	for (uint32 t = 0; t < r->ntuples; t++)
		if (r->count[t] == 0)
			return false;
	for (int m = 0; m < r->nmeas; m++)
		for (uint32 t = 0; r->nn[m] != NULL && t < r->ntuples; t++)
			if (r->nn[m][t] > r->count[t] ||
				(r->nn[m][t] > 0 && ((r->min[m] != NULL && r->min[m][t] > r->max[m][t]) || (r->minf[m] != NULL && r->minf[m][t] > r->maxf[m][t]))))
				return false;
	return true;
}

bool
sift_cpagg_rollup_decode_need(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	Rd			rd = {blob, blob + len, false};
	bool		ok;

	memset(r, 0, sizeof(*r));
	r->list = -1;
	ok = read_head(s, &rd, r);
	for (int j = 0; ok && j < r->ndims; j++)
		ok = read_dim(&rd, r, j, need_dim(need, r->dim[j]));
	if (ok)
		r->count = palloc(r->ntuples * sizeof(uint32));
	for (uint32 t = 0; ok && t < r->ntuples; t++)
		r->count[t] = (uint32) get_u(&rd);
	for (int m = 0; ok && m < r->nmeas; m++)
		ok = read_meas(s, &rd, r, m, need);
	MemoryContextSwitchTo(old);
	return ok && !rd.bad && rd.p == rd.end && rollup_sound(r);
}

/* the order of the tuples by one dim, ties by position (so that the sums of a group add in the order of the tuples) */
static const SiftCRollup *proj_r;
static int	proj_j;

static int
cmp_proj(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;
	bool		nx = proj_r->isnull[proj_j] && proj_r->isnull[proj_j][x],
				ny = proj_r->isnull[proj_j] && proj_r->isnull[proj_j][y];

	if (nx != ny)
		return nx ? 1 : -1;
	if (!nx && proj_r->key[proj_j][x] != proj_r->key[proj_j][y])
		return SIFT_CMP3(proj_r->key[proj_j][x], proj_r->key[proj_j][y]);
	return SIFT_CMP3(x, y);
}

bool
sift_cpagg_rollup_dim(const SiftColSchema *s, const SiftCRollup *r, int leaf, MemoryContext mcxt, SiftCHistDim *out)
{
	MemoryContext old;
	int			j = -1;
	uint32	   *ord,
				ng = 0;
	bool		isnull_group;

	for (int i = 0; i < r->ndims; i++)
		if (r->dim[i] == leaf)
			j = i;
	if (j < 0)
		return false;
	old = MemoryContextSwitchTo(mcxt);
	memset(out, 0, sizeof(*out));
	out->leaf = leaf;
	out->nmeas = r->nmeas;
	ord = palloc(r->ntuples * sizeof(uint32));
	for (uint32 t = 0; t < r->ntuples; t++)
		ord[t] = t;
	proj_r = r;
	proj_j = j;
	qsort(ord, r->ntuples, sizeof(uint32), cmp_proj);
	out->key = palloc0(r->ntuples * sizeof(int64));
	out->count = palloc0(r->ntuples * sizeof(uint32));
	for (int m = 0; m < r->nmeas; m++)
	{
		out->meas[m] = r->meas[m];
		out->nn[m] = palloc0(r->ntuples * sizeof(uint32));
		if (r->sum[m])
			out->sum[m] = palloc0(r->ntuples * sizeof(SiftCI128));
		else
			out->sumf[m] = palloc0(r->ntuples * sizeof(double));
	}
	for (uint32 i = 0; i < r->ntuples; i++)
	{
		uint32		t = ord[i];
		bool		nul = r->isnull[j] && r->isnull[j][t];

		if (i == 0 || nul != (r->isnull[j] && r->isnull[j][ord[i - 1]]) || (!nul && r->key[j][t] != r->key[j][ord[i - 1]]))
			ng++;
		out->key[ng - 1] = r->key[j][t];
		out->count[ng - 1] += r->count[t];
		for (int m = 0; m < r->nmeas; m++)
		{
			if (r->nn[m][t] > 0 && out->nn[m][ng - 1] == 0 && out->sumf[m])
				out->sumf[m][ng - 1] = r->sumf[m][t];
			else if (out->sumf[m])
				out->sumf[m][ng - 1] += r->sumf[m][t];
			if (out->sum[m])
				out->sum[m][ng - 1] += r->sum[m][t];
			out->nn[m][ng - 1] += r->nn[m][t];
		}
	}
	isnull_group = ng > 0 && r->isnull[j] && r->isnull[j][ord[r->ntuples - 1]];
	out->ngroups = ng;
	out->has_null = isnull_group;
	pfree(ord);
	MemoryContextSwitchTo(old);
	(void) s;
	return true;
}

/* Marks the leaves the cube and the rollups read; false when a blob does not decode */
bool
sift_cpagg_leaves(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, bool *need)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg leaves", ALLOCSET_SMALL_SIZES);
	SiftCHist	h;
	bool		ok = true;

	if (len > 0 && sift_cpagg_decode(s, blob, len, cx, &h))
	{
		for (int d = 0; d < h.ndims; d++)
		{
			need[h.dim[d].leaf % SIFT_PAGG_CAL] = true;
			for (int m = 0; m < h.dim[d].nmeas; m++)
				need[h.dim[d].meas[m]] = true;
		}
	}
	else if (len > 0)
		ok = false;
	if (ok && elen > 0)
	{
		int			nl,
					list[SIFT_PAGG_MAX_ROLLUPS];
		uint32		blen[SIFT_PAGG_MAX_ROLLUPS];
		const uint8 *p;

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, false, &nl, list, blen, &p))
			ok = false;
		for (int i = 0; ok && i < nl; p += blen[i++])
		{
			SiftCRollup r;

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r) || r.list != list[i])
			{
				ok = false;
				break;
			}
			if (r.list >= 0)
				need[r.list] = true;
			for (int j = 0; j < r.ndims; j++)
				need[r.dim[j]] = true;
			for (int m = 0; m < r.nmeas; m++)
				need[r.meas[m]] = true;
		}
	}
	MemoryContextDelete(cx);
	return ok;
}

static bool
same_bytes(const StringInfoData *a, const uint8 *b, uint32 len)
{
	return a->len == (int) len && (len == 0 || memcmp(a->data, b, len) == 0);
}

/* The spec that recomputes a stored cube and element blob: the dims, rollups and measures they hold, exactly */
static bool
spec_of(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, int format, MemoryContext cx, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->fixed = spec->rfixed = true;
	spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
	if (len > 0)
	{
		SiftCHist	h;

		if (!sift_cpagg_decode(s, blob, len, cx, &h) || (h.dim[0].minmax && format < SIFT_COL_FORMAT_MINMAX))
			return false;
		spec->minmax = h.dim[0].minmax;
		for (int d = 0; d < h.ndims; d++, spec->ndims++)
		{
			spec->dim[spec->ndims].leaf = h.dim[d].leaf % SIFT_PAGG_CAL;
			spec->dim[spec->ndims].cal = h.dim[d].leaf >= SIFT_PAGG_CAL;
			spec->dim[spec->ndims].grain = h.dim[d].leaf / SIFT_PAGG_CAL;
			spec->dim[spec->ndims].no_minmax = !h.dim[d].minmax;
			if (spec->dim[spec->ndims].cal && format < SIFT_COL_FORMAT_CALENDAR)
				return false;
			spec->dim[spec->ndims].nmeas = h.dim[d].nmeas;
			memcpy(spec->dim[spec->ndims].meas, h.dim[d].meas, sizeof(spec->dim[0].meas));
		}
	}
	if (elen > 0)
	{
		int			nl,
					list[SIFT_PAGG_MAX_ROLLUPS];
		uint32		blen[SIFT_PAGG_MAX_ROLLUPS];
		const uint8 *p;

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, false, &nl, list, blen, &p))
			return false;
		for (int i = 0; i < nl; p += blen[i++])
		{
			SiftCRollup r;
			SiftCPaggRollup *ru = &spec->rollup[spec->nrollups++];

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r) || r.list != list[i] || (r.list < 0 && format < SIFT_COL_FORMAT_ROWS))
				return false;
			*ru = (SiftCPaggRollup) {.list = r.list, .ndims = r.ndims, .nmeas = r.nmeas};
			memcpy(ru->dim, r.dim, sizeof(ru->dim));
			memcpy(ru->meas, r.meas, sizeof(ru->meas));
		}
	}
	return true;
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
				 int format)
{
	SiftCPaggSpec spec;
	StringInfoData again,
				eagain;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok = spec_of(s, blob, len, eblob, elen, format, cx, &spec);

	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&again);
	initStringInfo(&eagain);
	sift_cpagg_cube(s, &spec, cols, nrows, 0, &again, elen > 0 ? &eagain : NULL);
	ok = same_bytes(&again, blob, len) && same_bytes(&eagain, eblob, elen);
	pfree(again.data);
	pfree(eagain.data);
	return ok;
}

/* Exceptions are optional. Rebuild the stored leaf: older writers could skip an earlier candidate whose reused overflow bitmap was empty. */
bool
sift_cpagg_check_excepts(const SiftColSchema *s, const SiftCPaggCol *cols, const uint32 *const *eoff, uint32 nrows, const uint8 *blob, uint32 len,
						 const uint8 *eblob, uint32 elen, int format, const uint8 *xblob, uint32 xlen, const uint8 *xeblob, uint32 xelen)
{
	Rd			rd = {xblob, xblob ? xblob + xlen : NULL, false};
	uint64		leaf;
	SiftCPaggSpec spec;
	StringInfoData xa,
				xb;
	MemoryContext cx;
	bool		ok;

	if (xlen == 0)
		return xelen == 0;
	leaf = get_u(&rd);
	if (rd.bad || leaf >= (uint64) s->ncols)
		return false;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	ok = spec_of(s, blob, len, eblob, elen, format, cx, &spec);
	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&xa);
	initStringInfo(&xb);
	sift_cpagg_excepts(s, &spec, cols, eoff, nrows, (int) leaf, blob, len, eblob, elen, &xa, &xb);
	ok = same_bytes(&xa, xblob, xlen) && same_bytes(&xb, xeblob, xelen);
	pfree(xa.data);
	pfree(xb.data);
	return ok;
}
