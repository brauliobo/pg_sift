/*
 * col/f2/f2_view.c - the values of a column of a granule as the arrays of a ColView (col/f2/f2_view.h).
 *
 * check-memory: allow-allocs every array follows one granule (at most 65,535 rows), freed with the context of the granule
 */
#include "postgres.h"

#include "col/f2/f2_view.h"
#include "col/granule/cfilt_str_simd.h"

static void
strs_view(const SiftGranuleView *v, int c, int n, bool *nl, bool codes, ColView *cv)
{
	SiftStrCell *sc;
	const char **sp = palloc(n * sizeof(char *));
	uint32	   *sl = palloc(n * sizeof(uint32));

	if (codes)
	{
		uint32	   *dc = palloc(n * sizeof(uint32));
		SiftGranuleDict *d = palloc(sizeof(SiftGranuleDict));

		if (sift_granule_str_codes(v, c, dc, nl, d))
		{
			cv->dcode = dc;
			cv->dict = d;
			return;
		}
	}
	sc = palloc(n * sizeof(SiftStrCell));
	if (!sift_cfstr_memo_strs(v, c, NULL, n, sc))
		sift_granule_strs(v, c, sc);
	for (int r = 0; r < n; r++)
	{
		sp[r] = sc[r].p;
		sl[r] = sc[r].len;
		nl[r] = sc[r].isnull;
	}
	cv->s = sp;
	cv->slen = sl;
}

/* The values of the rows only, placed at their rows of arrays of n */
static void
sparse_view(const SiftGranuleView *v, int c, int n, const uint16 *rows, int nrows, bool *nl, ColView *cv)
{
	bool	   *sn = palloc(nrows * sizeof(bool));

	if (cv->kind == SIFT_CELL_INT)
	{
		int64	   *sv = palloc(nrows * sizeof(int64)),
				   *iv = palloc0(n * sizeof(int64));

		sift_granule_point_ints(v, c, rows, nrows, sv, sn);
		for (int j = 0; j < nrows; j++)
			iv[rows[j]] = sv[j];
		cv->i = iv;
	}
	else if (cv->kind == SIFT_CELL_F8)
	{
		double	   *sv = palloc(nrows * sizeof(double)),
				   *fv = palloc0(n * sizeof(double));

		sift_granule_point_f8(v, c, rows, nrows, sv, sn);
		for (int j = 0; j < nrows; j++)
			fv[rows[j]] = sv[j];
		cv->f = fv;
	}
	else
	{
		SiftStrCell *sv = palloc(nrows * sizeof(SiftStrCell));
		const char **sp = palloc0(n * sizeof(char *));
		uint32	   *sl = palloc0(n * sizeof(uint32));

		if (!sift_cfstr_memo_strs(v, c, rows, nrows, sv))
			sift_granule_point_strs(v, c, rows, nrows, sv);
		for (int j = 0; j < nrows; j++)
		{
			sp[rows[j]] = sv[j].p;
			sl[rows[j]] = sv[j].len;
			sn[j] = sv[j].isnull;
		}
		cv->s = sp;
		cv->slen = sl;
	}
	for (int j = 0; j < nrows; j++)
		nl[rows[j]] = sn[j];
}

ColView
sift_f2_decode_view(const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows)
{
	ColView		cv = {0};
	bool	   *nl = palloc0(n);
	uint8	   *tg = palloc(n);

	cv.kind = d->store == SIFT_COLS_INT ? SIFT_CELL_INT : (d->store == SIFT_COLS_STR || d->store == SIFT_COLS_VAR) ? SIFT_CELL_STR : SIFT_CELL_F8;
	if (rows != NULL)
		sparse_view(v, c, n, rows, nrows, nl, &cv);
	else if (cv.kind == SIFT_CELL_INT)
	{
		int64	   *iv = palloc(n * sizeof(int64));

		sift_granule_ints(v, c, iv, nl);
		cv.i = iv;
	}
	else if (cv.kind == SIFT_CELL_F8)
	{
		double	   *fv = palloc(n * sizeof(double));

		sift_granule_f8(v, c, fv, nl);
		cv.f = fv;
	}
	else
		strs_view(v, c, n, nl, codes, &cv);
	sift_granule_tags(v, c, tg);
	cv.isnull = nl;
	cv.tag = tg;
	return cv;
}
