/*
 * col/f2/f2_point_dir_test.c - test-only SQL function sift._test_s55_point_dir (SIFT_TESTING builds): the point directory built for generated columns names, in the bucket of each value, every row
 * that holds it and in ascending order, and is built only for the columns that qualify.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "lib/stringinfo.h"

#include "col/f2/f2_point_dir.h"

PG_FUNCTION_INFO_V1(sift_test_s55_point_dir);

static int
expect(bool ok)
{
	return !ok;
}

/* The rows of each bucket the value of a column hashes to hold every row of that value, ascending; the directory of the column has as many entries as it has values */
static int
check_column(const char *blob, const SiftPointCol *pc, const SiftColIn *col, const int *gstart, int ng)
{
	const uint32 *start = (const uint32 *) (blob + pc->off_buckets),
			   *ref = (const uint32 *) (blob + pc->off_entries);
	int			failures = 0,
				nnz = 0;

	for (int g = 0; g < ng; g++)
		for (int r = 0; r < gstart[g + 1] - gstart[g]; r++)
		{
			const int	row = gstart[g] + r;
			const uint32 want = ((uint32) g << SIFT_POINT_ROW_BITS) | r;
			bool		found = false;

			if (col->isnull != NULL && col->isnull[row])
				continue;
			nnz++;
			for (uint32 e = start[sift_point_bucket(col->i[row], pc->log2_buckets)]; e < start[sift_point_bucket(col->i[row], pc->log2_buckets) + 1] && !found; e++)
				found = ref[e] == want;
			failures += expect(found);
		}
	failures += expect((int) pc->nentries == nnz);
	failures += expect(start[(UINT64CONST(1) << pc->log2_buckets)] == pc->nentries);
	for (uint64 b = 0; b < (UINT64CONST(1) << pc->log2_buckets); b++)
		for (uint32 e = start[b] + 1; e < start[b + 1]; e++)
			failures += expect(ref[e - 1] < ref[e]);
	return failures;
}

/* (nrows, seed): the number of expectations that failed, 0 when the directory is right */
Datum
sift_test_s55_point_dir(PG_FUNCTION_ARGS)
{
	const int	n = PG_GETARG_INT32(0);
	uint64		rs = (uint64) PG_GETARG_INT32(1);
	enum {UNIQUE, WIDE, NARROW, TIMELIKE, TAGGED, NULLY, NCOLS};
	int64	   *vals[NCOLS];
	bool	   *nulls = palloc0(n),
			   *none = palloc0(n);
	uint8	   *tags = palloc0(n);
	int		   *gstart = palloc((n / SIFT_GRANULE_ROWS + 3) * sizeof(int));
	SiftColIn	cols[NCOLS] = {{0}};
	SiftRows	rows = {.nrows = n,.ncols = NCOLS,.cols = cols,.time_col = 1 + TIMELIKE};
	StringInfoData out;
	int			ng = 0,
				failures = 0;
	const SiftPointHdr *hdr;

	if (n < SIFT_POINT_MIN_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be at least %d", SIFT_POINT_MIN_ROWS)));

	for (int c = 0; c < NCOLS; c++)
		vals[c] = palloc(n * sizeof(int64));
	for (int r = 0; r < n; r++)
	{
		rs = rs * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
		vals[UNIQUE][r] = r * 7919 + 13;
		vals[WIDE][r] = (int64) ((rs >> 20) % (uint64) (n / 2 + 1));
		vals[NARROW][r] = (int64) ((rs >> 20) % 9);
		vals[TIMELIKE][r] = r * 1000;
		vals[TAGGED][r] = r;
		vals[NULLY][r] = (int64) ((rs >> 24) % (uint64) (n / 2 + 1));
		nulls[r] = r % 3 == 0;
		tags[r] = r == n - 1 ? 1 : 0;
	}
	for (int c = 0; c < NCOLS; c++)
		cols[c] = (SiftColIn) {.kind = SIFT_CELL_INT,.i = vals[c],.isnull = c == NULLY ? nulls : none,.tag = c == TAGGED ? tags : NULL};
	for (int from = 0; from < n; from += SIFT_GRANULE_ROWS)
		gstart[ng++] = from;
	gstart[ng] = n;
	initStringInfo(&out);
	failures += expect(sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	hdr = (const SiftPointHdr *) out.data;
	failures += expect(hdr->magic == SIFT_POINT_MAGIC && hdr->ncols == SIFT_POINT_MAX_COLS);
	for (int k = 0; k < hdr->ncols; k++)
	{
		const SiftPointCol *pc = (const SiftPointCol *) (out.data + sizeof(SiftPointHdr)) + k;

		failures += expect((int) pc->leaf != TIMELIKE && (int) pc->leaf != TAGGED && (int) pc->leaf != NARROW);
		failures += check_column(out.data, pc, &cols[pc->leaf], gstart, ng);
	}
	resetStringInfo(&out);
	failures += expect(sift_point_dir_build(&rows, gstart, ng, (uint64) n * 40, &out));
	failures += expect(((const SiftPointHdr *) out.data)->ncols == 2);
	resetStringInfo(&out);
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 8, &out));
	rows.nsort = 1;
	resetStringInfo(&out);
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	rows.nsort = 0;
	rows.nrows = SIFT_POINT_MIN_ROWS - 1;
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	PG_RETURN_INT32(failures);
}

#endif							/* SIFT_TESTING */
