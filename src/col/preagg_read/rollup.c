/*
 * col/preagg_read/rollup.c - reading a rollup: the head, the dimensions and only the measures asked for
 * the tuples, a projection of the dimensions, the leaves of the unit
 * the inverse of preagg/rollup.c
 */
/* check-memory: allow-allocs a rollup holds at most SIFT_PAGG_ROLLUP_TUPLES (4096) tuples */
#include "core/cmp.h"
#include "col/preagg_read/preagg_read_int.h"

/* the varints of n values are passed over; eight bytes at a time while they end fewer values than are left (a value that continues into the next eight is counted there) */
static void
skip_u(Rd *r, uint32 n)
{
	while (n > 8 && r->end - r->p >= 8)
	{
		uint64		w,
					ends;
		uint32		c;

		memcpy(&w, r->p, sizeof(w));
		ends = ~w & UINT64CONST(0x8080808080808080);
		c = (uint32) (((ends >> 7) * UINT64CONST(0x0101010101010101)) >> 56);
		if (c >= n)
			break;
		n -= c;
		r->p += 8;
	}
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
	nt = cpread_get_u(&rd);
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
bool
cpread_read_head(const SiftColSchema *s, Rd *rd, SiftCRollup *r)
{
	uint32		nt;

	if (rd->end - rd->p < 3)
		return false;
	r->ndims = *rd->p++;
	r->nmeas = *rd->p++;
	rd->measlen = r->measlen = (r->nmeas & PAGG_MEASLEN) != 0;
	r->nmeas &= ~PAGG_MEASLEN;
	nt = (uint32) cpread_get_u(rd);
	if (r->ndims == 0 || r->ndims > SIFT_PAGG_MAX_RDIMS || r->nmeas > SIFT_PAGG_MAX_MEAS || nt == 0 || nt > PAGG_MAX_TUPLES)
		return false;
	r->ntuples = nt;
	for (int j = 0; j < r->ndims; j++)
	{
		uint64		l = cpread_get_u(rd);

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
		uint64		l = cpread_get_u(rd);

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
		prev = (int64) ((uint64) prev + (uint64) cpread_get_zz(rd));
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
		uint32		n = (uint32) cpread_get_u(rd);

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
			r->sum[m][t] = cpread_get_zz128(rd);
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
				(pass ? r->max[m] : r->min[m])[t] = cpread_get_zz(rd);
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
	const uint8 *mend = NULL;
	uint32		nnz;

	if (rd->measlen)
	{
		uint64		mlen = cpread_get_u(rd);

		if (rd->bad || mlen > (uint64) (rd->end - rd->p))
			return false;
		mend = rd->p + mlen;
		if (!want)
		{
			rd->p = mend;
			return true;
		}
	}
	nnz = read_nn(rd, r, m, want);
	return read_sums(rd, r, m, fl, need == NULL || (k >= 0 && need->meas[k].sums)) &&
		read_minmax(rd, r, m, fl, need == NULL || (k >= 0 && need->meas[k].minmax), nnz) && (mend == NULL || rd->p == mend);
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
	ok = cpread_read_head(s, &rd, r);
	for (int j = 0; ok && j < r->ndims; j++)
		ok = read_dim(&rd, r, j, need_dim(need, r->dim[j]));
	if (ok)
		r->count = palloc(r->ntuples * sizeof(uint32));
	for (uint32 t = 0; ok && t < r->ntuples; t++)
		r->count[t] = (uint32) cpread_get_u(&rd);
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

		if (!cpread_read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, false, &nl, list, blen, &p))
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
