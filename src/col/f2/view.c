/*
 * col/f2/view.c - the values of a column of a granule as the arrays of a ColView (col/f2/view.h).
 */
#include "postgres.h"

#include "col/f2/view.h"
#include "col/granule/cfilt/str_simd.h"

/* The rows of a leaf of a key of a document that hold a value of a JSON type it could not store: its value is the payload of their exception */
void
sift_f2_decode_payloads(const SiftGranuleView *v, int c, ColView *cv)
{
	const int	n = sift_granule_chunk_len(v, c);
	const char **xp = NULL;
	uint32	   *xl = NULL;
	SiftExcReader er = {0};

	for (int r = 0; r < n; r++)
		if (cv->isnull[r] && cv->tag[r] >= SIFT_COLTAG_STRING && cv->tag[r] <= SIFT_COLTAG_ARRAY)
		{
			if (xp == NULL)
			{
				xp = palloc0(n * sizeof(char *));
				xl = palloc0(n * sizeof(uint32));
			}
			xp[r] = sift_granule_exc_payload_in(v, c, r, &er, &xl[r]);
		}
	sift_exc_reader_close(&er);
	cv->xpay = xp;
	cv->xplen = xl;
}

static void
strs_view(const SiftStrMemo *memo, const SiftGranuleView *v, int c, int n, bool *nl, bool codes, ColView *cv)
{
	SiftStrCell *sc;
	const char **sp = sift_alloc(n * sizeof(char *));
	uint32	   *sl = sift_alloc(n * sizeof(uint32));

	if (codes)
	{
		uint32	   *dc = sift_alloc(n * sizeof(uint32));
		SiftGranuleDict *d = sift_alloc(sizeof(SiftGranuleDict));

		if (sift_granule_str_codes(v, c, dc, nl, d))
		{
			cv->dcode = dc;
			cv->dict = d;
			return;
		}
	}
	sc = sift_alloc(n * sizeof(SiftStrCell));
	if (memo == NULL || !sift_cfstr_memo_strs_in(memo, v, c, NULL, n, sc))
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

/* The codes of the rows only, placed at their rows of an array of n: false when the column is not a dictionary of strings */
static bool
sparse_codes(const SiftGranuleView *v, int c, int n, const uint16 *rows, int nrows, bool *sn, ColView *cv)
{
	uint32	   *sc = sift_alloc(nrows * sizeof(uint32)),
			   *dc = sift_alloc0(n * sizeof(uint32));
	SiftGranuleDict *d = sift_alloc(sizeof(SiftGranuleDict));

	if (!sift_granule_point_str_codes(v, c, rows, nrows, sc, sn, d))
		return false;
	for (int j = 0; j < nrows; j++)
		dc[rows[j]] = sc[j];
	cv->dcode = dc;
	cv->dict = d;
	return true;
}

/* The values of the rows only, placed at their rows of arrays of n */
static void
sparse_view(const SiftStrMemo *memo, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows, bool *nl, ColView *cv)
{
	bool	   *sn = sift_alloc(nrows * sizeof(bool));

	if (cv->kind == SIFT_CELL_INT)
	{
		int64	   *sv = sift_alloc(nrows * sizeof(int64)),
				   *iv = sift_alloc0(n * sizeof(int64));

		sift_granule_point_ints(v, c, rows, nrows, sv, sn);
		for (int j = 0; j < nrows; j++)
			iv[rows[j]] = sv[j];
		cv->i = iv;
	}
	else if (cv->kind == SIFT_CELL_F8)
	{
		double	   *sv = sift_alloc(nrows * sizeof(double)),
				   *fv = sift_alloc0(n * sizeof(double));

		sift_granule_point_f8(v, c, rows, nrows, sv, sn);
		for (int j = 0; j < nrows; j++)
			fv[rows[j]] = sv[j];
		cv->f = fv;
	}
	else if (!codes || !sparse_codes(v, c, n, rows, nrows, sn, cv))
	{
		SiftStrCell *sv = sift_alloc(nrows * sizeof(SiftStrCell));
		const char **sp = sift_alloc0(n * sizeof(char *));
		uint32	   *sl = sift_alloc0(n * sizeof(uint32));

		if (memo == NULL || !sift_cfstr_memo_strs_in(memo, v, c, rows, nrows, sv))
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

static ColView
decode_view(const SiftStrMemo *memo, const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows)
{
	ColView		cv = {0};
	bool	   *nl;
	uint8	   *tg;

	if (d->parent >= 0 || d->kind == SIFT_COLK_GROUP)
	{
		n = sift_granule_chunk_len(v, c);
		rows = NULL;
	}
	nl = sift_alloc0(n);
	tg = sift_alloc(n);
	cv.kind = d->store == SIFT_COLS_INT ? SIFT_CELL_INT : (d->store == SIFT_COLS_STR || d->store == SIFT_COLS_VAR) ? SIFT_CELL_STR : SIFT_CELL_F8;
	if (rows != NULL)
		sparse_view(memo, v, c, n, codes, rows, nrows, nl, &cv);
	else if (cv.kind == SIFT_CELL_INT)
	{
		int64	   *iv = sift_alloc(n * sizeof(int64));

		sift_granule_ints(v, c, iv, nl);
		cv.i = iv;
	}
	else if (cv.kind == SIFT_CELL_F8)
	{
		double	   *fv = sift_alloc(n * sizeof(double));

		sift_granule_f8(v, c, fv, nl);
		cv.f = fv;
	}
	else
		strs_view(memo, v, c, n, nl, codes, &cv);
	sift_granule_tags(v, c, tg);
	cv.isnull = nl;
	cv.tag = tg;
	return cv;
}

ColView
sift_f2_decode_view(const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows)
{
	return decode_view(sift_cfstr_memo_active(), d, v, c, n, codes, rows, nrows);
}

bool
sift_f2_decode_view_a(SiftArena *a, SiftErr *err, const SiftStrMemo *memo, const SiftColDef *d, const SiftGranuleView *v, int c, int n, bool codes, const uint16 *rows, int nrows, ColView *out)
{
	SIFT_ARENA_TRY(a, err)
		*out = decode_view(memo, d, v, c, n, codes, rows, nrows);
	SIFT_ARENA_END
}
