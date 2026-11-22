/*
 * col/granule/granule_point_test.c - test-only SQL function sift._test_s55_granule_point (SIFT_TESTING builds): a column of generated values is encoded as one granule, and the values of random sets of
 * its rows made by the sparse decoders (granule_point.h) are compared with those the whole-column decoders give for them.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "col/granule/granule_int.h"

PG_FUNCTION_INFO_V1(sift_test_s55_granule_point);

static uint64
rnd(uint64 *s)
{
	*s = *s * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
	return *s >> 17;
}

/* kind 0: int, 1: float, 2: string; pattern 0: a constant, 1: ascending, 2: few values, 3: random; nulls: every 5th row */
static int
case_failures(int kind, int pattern, int n, bool nulls, uint64 *rs)
{
	int64	   *iv = palloc0(n * sizeof(int64));
	double	   *fv = palloc0(n * sizeof(double));
	const char **sv = palloc0(n * sizeof(char *));
	uint32	   *sl = palloc0(n * sizeof(uint32));
	bool	   *nl = palloc0(n);
	uint64	   *tid = palloc(n * sizeof(uint64));
	SiftColIn	col;
	SiftGranuleIn in = {.nrows = n,.ncols = 1,.tier = SIFT_TIER_WARM_L1};
	StringInfoData out;
	SiftGranuleDesc desc;
	SiftGranuleView v;
	char	   *raw;
	int			failures = 0;

	for (int r = 0; r < n; r++)
	{
		const int64 x = pattern == 0 ? 7 : pattern == 1 ? r * 3 : pattern == 2 ? (int64) (rnd(rs) % 6) : (int64) (rnd(rs) % 1000003);

		tid[r] = ((uint64) (r / 100) << 16) | (uint64) (r % 100 + 1);
		nl[r] = nulls && r % 5 == 2;
		iv[r] = nl[r] ? 0 : x;
		fv[r] = nl[r] ? 0 : (double) x / 8;
		if (kind == 2 && !nl[r])
		{
			char	   *s = psprintf("v%ld-%s", (long) x, pattern == 3 ? "padding-of-some-length" : "p");

			sv[r] = s;
			sl[r] = strlen(s);
		}
	}
	col = (SiftColIn) {.kind = kind == 0 ? SIFT_CELL_INT : kind == 1 ? SIFT_CELL_F8 : SIFT_CELL_STR,.i = iv,.f = fv,.s = sv,.slen = sl,.isnull = nl};
	in.tid = tid;
	in.cols = &col;
	in.all_frozen = true;
	in.min_xid = in.max_xid = sift_xid_to_fxid(FrozenTransactionId);
	initStringInfo(&out);
	desc = sift_granule_encode(&out, &in);
	raw = palloc(out.len);
	memcpy(raw, out.data, out.len);
	v = sift_granule_open(NULL, raw, desc.data_len, &desc);
	for (int round = 0; round < 20; round++)
	{
		const int	m = 1 + (int) (rnd(rs) % Min(n, SIFT_POINT_ROWS_MAX));
		const int	stride = 1 + (int) (rnd(rs) % (round % 2 == 0 ? 3 : 400));
		uint16	   *rows = palloc(m * sizeof(uint16));
		bool	   *sn = palloc(m),
				   *fn = palloc(n);
		int			k = 0;

		for (int r = (int) (rnd(rs) % 7); r < n && k < m; r += 1 + (int) (rnd(rs) % stride))
			rows[k++] = r;
		if (kind == 0)
		{
			int64	   *so = palloc(Max(k, 1) * sizeof(int64)),
					   *fo = palloc(n * sizeof(int64));

			sift_granule_point_ints(&v, 0, rows, k, so, sn);
			sift_granule_ints(&v, 0, fo, fn);
			for (int j = 0; j < k; j++)
				failures += sn[j] != fn[rows[j]] || so[j] != fo[rows[j]];
		}
		else if (kind == 1)
		{
			double	   *so = palloc(Max(k, 1) * sizeof(double)),
					   *fo = palloc(n * sizeof(double));

			sift_granule_point_f8(&v, 0, rows, k, so, sn);
			sift_granule_f8(&v, 0, fo, fn);
			for (int j = 0; j < k; j++)
				failures += sn[j] != fn[rows[j]] || memcmp(&so[j], &fo[rows[j]], sizeof(double)) != 0;
		}
		else
		{
			SiftStrCell *so = palloc(Max(k, 1) * sizeof(SiftStrCell)),
					   *fo = palloc(n * sizeof(SiftStrCell));

			sift_granule_point_strs(&v, 0, rows, k, so);
			sift_granule_strs(&v, 0, fo);
			for (int j = 0; j < k; j++)
				failures += so[j].isnull != fo[rows[j]].isnull || (!so[j].isnull && (so[j].len != fo[rows[j]].len || memcmp(so[j].p, fo[rows[j]].p, so[j].len) != 0));
		}
	}
	return failures;
}

/* (nrows, seed): the cases of every kind, pattern and null setting; the number of values that differ, 0 when the sparse decoders agree with the whole-column ones */
Datum
sift_test_s55_granule_point(PG_FUNCTION_ARGS)
{
	const int	n = PG_GETARG_INT32(0);
	uint64		rs = (uint64) PG_GETARG_INT32(1);
	int			failures = 0;

	if (n < 1 || n > SIFT_GRANULE_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be 1 to %d", SIFT_GRANULE_ROWS)));
	for (int kind = 0; kind < 3; kind++)
		for (int pattern = 0; pattern < 4; pattern++)
			for (int nulls = 0; nulls < 2; nulls++)
				failures += case_failures(kind, pattern, n, nulls, &rs);
	PG_RETURN_INT32(failures);
}

#endif							/* SIFT_TESTING */
