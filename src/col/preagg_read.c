/*
 * col/preagg_read.c - the pre-aggregates read: varint decoding of the cube and rollup blobs, and their verification against the chunks.
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/preagg_int.h"

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

/* ---------- reading ---------- */

/* One dim's body (cube blob format above, after the directory): false when it is not consistent with the schema */
static bool
decode_dim(const SiftColSchema *s, Rd *r, int leaf, SiftCHistDim *hd)
{
	uint32		ng,
				nk;

	memset(hd, 0, sizeof(*hd));
	hd->leaf = leaf;
	if (r->p + 2 > r->end)
		return false;
	hd->nmeas = *r->p++;
	ng = (uint32) get_u(r);
	if (r->p >= r->end || hd->nmeas > SIFT_PAGG_MAX_MEAS || ng == 0 || ng > SIFT_PAGG_CUBE_GROUPS)
		return false;
	hd->ngroups = ng;
	hd->has_null = (*r->p++ & PAGG_NULLGROUP) != 0;
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
		}
	}
	return !r->bad;
}

/*
 * The directory of a blob: leaf and body bytes of each entry (at most max of them; the dims of a cube, or the lists of an element blob); body
 * points at the first body. false when inconsistent
 */
static bool
read_dir(const SiftColSchema *s, const uint8 *blob, uint32 len, int max, int *nd, int *leaf, uint32 *blen, const uint8 **body)
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

		if (r.bad || l >= (uint64) s->ncols || n == 0 || n > len)
			return false;
		leaf[d] = (int) l;
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
	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, leaf, blen, &p))
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

	if (!read_dir(s, blob, len, SIFT_PAGG_MAX_DIMS, &nd, lf, blen, &p))
		goto done;
	res = 0;
	for (int d = 0; d < nd; d++)
	{
		if (lf[d] == leaf)
		{
			Rd			r = {p, p + blen[d], false};

			res = decode_dim(s, &r, leaf, out) && r.p == r.end ? 1 : -1;
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
	if (!read_dir(s, eblob, elen, SIFT_PAGG_SPEC_DIMS, &nl, list, blen, &p))
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

const SiftCHistDim *
sift_cpagg_find(const SiftCHist *h, int leaf)
{
	for (int d = 0; d < h->ndims; d++)
		if (h->dim[d].leaf == leaf)
			return &h->dim[d];
	return NULL;
}

int
sift_cpagg_meas_slot(const SiftCHistDim *d, int m)
{
	for (int i = 0; i < d->nmeas; i++)
		if (d->meas[i] == m)
			return i;
	return -1;
}

/* ---------- the rollup, read ---------- */

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

bool
sift_cpagg_rollup_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCRollup *r)
{
	return sift_cpagg_rollup_decode_need(s, blob, len, mcxt, NULL, r);
}

bool
sift_cpagg_rollup_decode_need(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	Rd			rd = {blob, blob + len, false};
	uint32		nt;
	bool		ok = false;

	memset(r, 0, sizeof(*r));
	r->list = -1;
	if (len < 3)
		goto done;
	r->ndims = *rd.p++;
	r->nmeas = *rd.p++;
	nt = (uint32) get_u(&rd);
	if (r->ndims == 0 || r->ndims > SIFT_PAGG_MAX_RDIMS || r->nmeas > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > SIFT_PAGG_ROLLUP_TUPLES)
		goto done;
	r->ntuples = nt;
	for (int j = 0; j < r->ndims; j++)
	{
		uint64		l = get_u(&rd);

		if (rd.bad || l >= (uint64) s->ncols || s->cols[l].parent < 0 || (r->list >= 0 && r->list != s->cols[l].parent))
			goto done;
		r->dim[j] = (int16) l;
		r->list = s->cols[l].parent;
	}
	for (int m = 0; m < r->nmeas; m++)
	{
		uint64		l = get_u(&rd);

		if (rd.bad || l >= (uint64) s->ncols || s->cols[l].parent != r->list)
			goto done;
		r->meas[m] = (int16) l;
	}
	for (int j = 0; j < r->ndims; j++)
	{
		int64		prev = 0;
		bool		want = need_dim(need, r->dim[j]);
		uint8	   *nulls = NULL;
		uint32		nnull = 0;

		if (rd.p >= rd.end || *rd.p > 1)
			goto done;
		if (*rd.p++)
		{
			uint32		nb = (nt + 7) / 8;

			if ((uint32) (rd.end - rd.p) < nb)
				goto done;
			nulls = palloc0(nt);
			for (uint32 t = 0; t < nt; t++)
				nnull += nulls[t] = rd.p[t / 8] >> (t % 8) & 1;
			rd.p += nb;
		}
		if (!want)
		{
			skip_u(&rd, nt - nnull);
			continue;
		}
		r->key[j] = palloc0(nt * sizeof(int64));
		r->isnull[j] = nulls;
		for (uint32 t = 0; t < nt; t++)
		{
			if (nulls && nulls[t])
				continue;
			prev = (int64) ((uint64) prev + (uint64) get_zz(&rd));
			r->key[j][t] = prev;
		}
	}
	r->count = palloc(nt * sizeof(uint32));
	for (uint32 t = 0; t < nt; t++)
		r->count[t] = (uint32) get_u(&rd);
	for (int m = 0; m < r->nmeas; m++)
	{
		bool		fl = s->cols[r->meas[m]].store == SIFT_COLS_F4 || s->cols[r->meas[m]].store == SIFT_COLS_F8;
		int			k = need_meas(need, r->meas[m]);
		bool		want = need == NULL || k >= 0,
					sums = need == NULL || (k >= 0 && need->meas[k].sums),
					mm = need == NULL || (k >= 0 && need->meas[k].minmax);
		uint32		nnz = 0;

		if (want)
			r->nn[m] = palloc(nt * sizeof(uint32));
		for (uint32 t = 0; t < nt; t++)
		{
			uint32		n = (uint32) get_u(&rd);

			if (want)
				r->nn[m][t] = n;
			nnz += n != 0;
		}
		if (!sums)
		{
			if (fl)
			{
				if ((Size) (rd.end - rd.p) < (Size) nt * sizeof(double))
					goto done;
				rd.p += nt * sizeof(double);
			}
			else
				skip_u(&rd, nt);
		}
		else if (fl)
		{
			if ((Size) (rd.end - rd.p) < (Size) nt * sizeof(double))
				goto done;
			r->sumf[m] = palloc(nt * sizeof(double));
			memcpy(r->sumf[m], rd.p, nt * sizeof(double));
			rd.p += nt * sizeof(double);
		}
		else
		{
			r->sum[m] = palloc(nt * sizeof(SiftCI128));
			for (uint32 t = 0; t < nt; t++)
				r->sum[m][t] = get_zz128(&rd);
		}
		if (!mm)
		{
			if (fl)
			{
				if ((Size) (rd.end - rd.p) < (Size) nnz * 2 * sizeof(double))
					goto done;
				rd.p += (Size) nnz * 2 * sizeof(double);
			}
			else
				skip_u(&rd, nnz * 2);
			continue;
		}
		if (fl)
		{
			r->minf[m] = palloc0(nt * sizeof(double));
			r->maxf[m] = palloc0(nt * sizeof(double));
		}
		else
		{
			r->min[m] = palloc0(nt * sizeof(int64));
			r->max[m] = palloc0(nt * sizeof(int64));
		}
		for (int pass = 0; pass < 2; pass++)
			for (uint32 t = 0; t < nt; t++)
			{
				if (r->nn[m][t] == 0)
					continue;
				if (fl)
				{
					double		v;

					if (rd.end - rd.p < (long) sizeof(double))
						goto done;
					memcpy(&v, rd.p, sizeof(double));
					rd.p += sizeof(double);
					(pass ? r->maxf[m] : r->minf[m])[t] = v;
				}
				else
					(pass ? r->max[m] : r->min[m])[t] = get_zz(&rd);
			}
	}
	ok = !rd.bad && rd.p == rd.end;
done:
	MemoryContextSwitchTo(old);
	return ok;
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
		return proj_r->key[proj_j][x] < proj_r->key[proj_j][y] ? -1 : 1;
	return x < y ? -1 : x > y;
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

/* ---------- verification ---------- */

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
			need[h.dim[d].leaf] = true;
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

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, &nl, list, blen, &p))
			ok = false;
		for (int i = 0; ok && i < nl; p += blen[i++])
		{
			SiftCRollup r;

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r) || r.list != list[i])
			{
				ok = false;
				break;
			}
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
spec_of(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, MemoryContext cx, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->fixed = spec->rfixed = true;
	spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
	if (len > 0)
	{
		SiftCHist	h;

		if (!sift_cpagg_decode(s, blob, len, cx, &h))
			return false;
		for (int d = 0; d < h.ndims; d++, spec->ndims++)
		{
			spec->dim[spec->ndims].leaf = h.dim[d].leaf;
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

		if (!read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, &nl, list, blen, &p))
			return false;
		for (int i = 0; i < nl; p += blen[i++])
		{
			SiftCRollup r;
			SiftCPaggRollup *ru = &spec->rollup[spec->nrollups++];

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r))
				return false;
			*ru = (SiftCPaggRollup) {.list = r.list, .ndims = r.ndims, .nmeas = r.nmeas};
			memcpy(ru->dim, r.dim, sizeof(ru->dim));
			memcpy(ru->meas, r.meas, sizeof(ru->meas));
		}
	}
	return true;
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen)
{
	SiftCPaggSpec spec;
	StringInfoData again,
				eagain;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok = spec_of(s, blob, len, eblob, elen, cx, &spec);

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
