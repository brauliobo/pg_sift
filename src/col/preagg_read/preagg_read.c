/*
 * col/preagg_read/preagg_read.c - reading the pre-aggregates of a unit: the cube blob and its dimensions
 * the varints, the directory, the decoded dimension, the cheap looks (peek, mirror, census)
 * the other files read rollups and verify
 */
#include "col/preagg_read/preagg_read_int.h"

uint64
cpread_get_u(Rd *r)
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

int64
cpread_get_zz(Rd *r)
{
	uint64		u = cpread_get_u(r);

	return sift_unzigzag(u);
}

SiftCI128
cpread_get_zz128(Rd *r)
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
			return (SiftCI128) sift_unzigzag(w);
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
	ng = (uint32) cpread_get_u(r);
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
		uint64		ml = cpread_get_u(r);

		if (ml >= (uint64) s->ncols)
			return false;
		hd->meas[m] = (int16) ml;
	}
	hd->key = palloc0(ng * sizeof(int64));
	hd->count = palloc(ng * sizeof(uint32));
	nk = ng - (hd->has_null ? 1 : 0);
	for (uint32 i = 0; i < nk; i++)
		hd->key[i] = i == 0 ? cpread_get_zz(r) : (int64) ((uint64) hd->key[i - 1] + cpread_get_u(r));
	/* the keys of a calendar dim are the first days of cells of its grain, in ascending order: nothing else is a group of a cell */
	for (uint32 i = 0; leaf >= SIFT_PAGG_CAL && i < nk; i++)
		if (!sift_cpagg_cal_key_ok(s->cols[leaf % SIFT_PAGG_CAL].type, leaf / SIFT_PAGG_CAL, hd->key[i]) || (i > 0 && hd->key[i] <= hd->key[i - 1]))
			return false;
	for (uint32 i = 0; i < ng; i++)
		hd->count[i] = (uint32) cpread_get_u(r);
	for (int m = 0; m < hd->nmeas; m++)
	{
		const SiftColDef *md = &s->cols[hd->meas[m]];

		hd->nn[m] = palloc(ng * sizeof(uint32));
		for (uint32 i = 0; i < ng; i++)
			hd->nn[m][i] = (uint32) cpread_get_u(r);
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
				hd->sum[m][i] = cpread_get_zz128(r);
			if (hd->minmax)
			{
				hd->min[m] = palloc0(ng * sizeof(int64));
				hd->max[m] = palloc0(ng * sizeof(int64));
				for (int pass = 0; pass < 2; pass++)
					for (uint32 i = 0; i < ng; i++)
						if (hd->nn[m][i] > 0)
							(pass ? hd->max[m] : hd->min[m])[i] = cpread_get_zz(r);
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
bool
cpread_read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, bool cube, int *nd, int *leaf, uint32 *blen, const uint8 **body)
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
		uint64		l = cpread_get_u(&r),
					n = cpread_get_u(&r);

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
	if (!cpread_read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
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

	if (!cpread_read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, lf, blen, &p))
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
	if (!cpread_read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nl, list, blen, &p))
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
	ng = cpread_get_u(&r);
	if (r.bad || r.p >= r.end || *nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	*flags = *r.p++;
	for (int m = 0; m < *nmeas; m++)
	{
		uint64		l = cpread_get_u(&r);

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
	*nm = *r.p++ & ~PAGG_MEASLEN;
	nt = cpread_get_u(&r);
	if (*nd == 0 || *nd > SIFT_PAGG_MAX_RDIMS || *nm > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > PAGG_MAX_TUPLES)
		return false;
	for (int j = 0; j < *nd + *nm; j++)
	{
		uint64		l = cpread_get_u(&r);

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
		if (!cpread_read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
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
		if (!cpread_read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nd, leaf, blen, &p))
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
	if (len > 0 && !cpread_read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, true, &nd, leaf, blen, &p))
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
	if (elen > 0 && !cpread_read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, false, &nd, leaf, blen, &p))
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
