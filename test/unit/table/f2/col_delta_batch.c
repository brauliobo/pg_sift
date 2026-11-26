/*
 * test/unit/table/f2/col_delta_batch.c - the records of a run of rows as one batch (table/f2/col_delta/batch_enc.c): the rows of the batches read back are the records that were fed, byte for byte
 * and in order, whatever the mix of columns, the jumps of the rows and the size of the run; every batch passes its check and none is more than a record may be.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

bool		sift_test_col_delta = true;

#include "table/f2/col_delta/col_delta.c"
#include "table/f2/col_delta/batch_enc.c"

enum { MAX_RECS = 600, OUT_CAP = 64 * 1024 };

typedef struct Feed
{
	uint8		rec[MAX_RECS][SIFT_COL_DELTA_MAX_REC_BYTES];
	uint32		bytes[MAX_RECS];
	int			n;
} Feed;

static uint32
make(uint8 *buf, uint32 ord, uint64 tid, int ncols, int64 base, bool with_double, TransactionId xmin)
{
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC] = {0};
	SiftColDeltaTarget t = {.part_gen = 4,.granule_id = 9,.row_ordinal = (uint16) ord,.tid = tid};

	for (int k = 0; k < ncols; k++)
	{
		cols[k].attnum = (int16) (k + 2);
		cols[k].kind = with_double && k == ncols - 1 ? SIFT_CELL_F8 : SIFT_CELL_INT;
		if (cols[k].kind == SIFT_CELL_F8)
			cols[k].cell.f = (double) base / 3.0;
		else
			cols[k].cell.i = base * (k + 1);
	}
	return sift_col_delta_encode(buf, SIFT_COL_DELTA_MAX_REC_BYTES, &t, xmin, 0, 0, cols, ncols);
}

static void
fill(Feed *f, int n, int ncols, bool with_double, int jump)
{
	uint32		ord = 3;
	uint64		tid = ((uint64) 17 << 16) | 4;

	f->n = n;
	for (int i = 0; i < n; i++)
	{
		f->bytes[i] = make(f->rec[i], ord, tid, ncols, (int64) rnd() % 100000 - 50000, with_double, 10 + (i / 200));
		ord += 1 + (uint32) (rnd() % jump);
		tid += 1 + (uint64) (rnd() % jump);
	}
}

/* The feed through a run and back through the iterator: the records are the same, the bytes are fewer */
static void
check_round_trip(const Feed *f, const char *what)
{
	SiftColDeltaRun *run = malloc(sizeof(*run));
	uint8	   *out = malloc(OUT_CAP);
	uint32		len = 0,
				in_bytes = 0;
	SiftColDeltaRecs *it = malloc(sizeof(*it));
	const uint8 *rec;
	SiftColDeltaEntryHdr h;
	int			got = 0;

	sift_col_delta_run_init(run);
	for (int i = 0; i <= f->n; i++)
	{
		uint32		n = sift_col_delta_run_feed(run, i < f->n ? f->rec[i] : NULL, out + len);

		for (uint32 at = len; at < len + n; at += ((SiftColDeltaEntryHdr *) (out + at))->rec_bytes)
		{
			const char *why = (((SiftColDeltaEntryHdr *) (out + at))->flags & SIFT_CDENT_FLAG_BATCH) != 0 ?
				sift_col_delta_batch_check(out + at, len + n - at) : sift_col_delta_rec_check(out + at, len + n - at);

			SIFT_CHECKF(why == NULL, "%s: a written record passes its check: %s", what, why != NULL ? why : "");
			SIFT_CHECKF(((SiftColDeltaEntryHdr *) (out + at))->rec_bytes <= SIFT_COL_DELTA_MAX_REC_BYTES, "%s: a record is not more than the most", what);
		}
		len += n;
		if (i < f->n)
			in_bytes += f->bytes[i];
	}
	sift_col_delta_recs_init(it, out, out + len);
	while (sift_col_delta_recs_next(it, &rec, &h) && got < f->n)
	{
		SIFT_CHECKF(h.rec_bytes == f->bytes[got] && memcmp(rec, f->rec[got], f->bytes[got]) == 0, "%s: record %d is the record that was fed", what, got);
		got++;
	}
	SIFT_CHECKF(got == f->n && !sift_col_delta_recs_more(it), "%s: %d of %d records came back", what, got, f->n);
	SIFT_CHECKF(len <= in_bytes, "%s: %u bytes written for %u fed", what, len, in_bytes);
	free(it);
	free(out);
	free(run);
}

static void
test_round_trip(void)
{
	static const struct
	{
		int			n,
					ncols,
					jump;
		bool		dbl;
		const char *what;
	}			cases[] = {
		{1, 1, 1, false, "one row"},
		{2, 1, 1, false, "two rows"},
		{50, 1, 1, false, "one int column, next rows"},
		{300, 2, 40, true, "an int and a double, jumps"},
		{MAX_RECS, 3, 5, false, "three int columns, many rows"},
		{200, 1, 1, true, "a double alone"},
		{MAX_RECS, 8, 3, true, "eight columns, a double"},
	};

	for (int i = 0; i < (int) lengthof(cases); i++)
	{
		Feed	   *f = malloc(sizeof(*f));

		fill(f, cases[i].n, cases[i].ncols, cases[i].dbl, cases[i].jump);
		check_round_trip(f, cases[i].what);
		free(f);
	}
}

int
main(void)
{
	test_round_trip();
	return sift_check_summary();
}
