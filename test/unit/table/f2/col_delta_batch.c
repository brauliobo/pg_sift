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

typedef enum Shape
{
	SHAPE_INTS,					/* integers, the last a double when asked */
	SHAPE_LEAF_ADDR,			/* the new address of the row, then integers: a record of a sift_col index */
	SHAPE_STRS,					/* an integer and a string that is the same for most rows */
	SHAPE_NULLS					/* an integer and a column that is null in every row */
} Shape;

static const char *const words[] = {"shipped", "", "pending", "a longer string than the others of the column"};

static uint32
make(uint8 *buf, uint32 ord, uint64 tid, int ncols, int64 base, bool with_double, TransactionId xmin, Shape shape)
{
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC] = {0};
	SiftColDeltaTarget t = {.part_gen = 4,.granule_id = 9,.row_ordinal = (uint16) ord,.tid = tid};
	uint8		flags = shape == SHAPE_LEAF_ADDR ? SIFT_CDENT_FLAG_LEAF_ADDR : 0;
	int			first = shape == SHAPE_LEAF_ADDR ? 1 : 0;
	const char *word = words[(uint64) base % 17 == 0 ? 1 + (uint64) base / 17 % 3 : 0];

	if (shape == SHAPE_LEAF_ADDR)
		cols[0] = (SiftColDeltaCol) {.attnum = SIFT_CD_ATT_TID_REMAP,.kind = SIFT_CELL_INT,.cell = {.i = (int64) (tid + 3 + (uint64) ord % 5)}};
	for (int k = first; k < ncols; k++)
	{
		cols[k].attnum = (int16) (k + 2 - first);
		cols[k].kind = with_double && k == ncols - 1 ? SIFT_CELL_F8 : SIFT_CELL_INT;
		if (cols[k].kind == SIFT_CELL_F8)
			cols[k].cell.f = (double) base / 3.0;
		else
			cols[k].cell.i = base * (k + 1);
	}
	if (shape == SHAPE_STRS || shape == SHAPE_NULLS)
	{
		cols[ncols - 1].kind = shape == SHAPE_STRS ? SIFT_CELL_STR : SIFT_CELL_INT;
		cols[ncols - 1].cell.isnull = shape == SHAPE_NULLS;
		cols[ncols - 1].cell.s = word;
		cols[ncols - 1].cell.slen = (uint32) strlen(word);
	}
	return sift_col_delta_encode(buf, SIFT_COL_DELTA_MAX_REC_BYTES, &t, xmin, 0, flags, cols, ncols);
}

static void
fill(Feed *f, int n, int ncols, bool with_double, int jump, Shape shape)
{
	uint32		ord = 3;
	uint64		tid = ((uint64) 17 << 16) | 4;

	f->n = n;
	for (int i = 0; i < n; i++)
	{
		f->bytes[i] = make(f->rec[i], ord, tid, ncols, (int64) rnd() % 100000 - 50000, with_double, 10 + (i / 200), shape);
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
		Shape		shape;
		const char *what;
	}			cases[] = {
		{1, 1, 1, false, SHAPE_INTS, "one row"},
		{2, 1, 1, false, SHAPE_INTS, "two rows"},
		{50, 1, 1, false, SHAPE_INTS, "one int column, next rows"},
		{300, 2, 40, true, SHAPE_INTS, "an int and a double, jumps"},
		{MAX_RECS, 3, 5, false, SHAPE_INTS, "three int columns, many rows"},
		{200, 1, 1, true, SHAPE_INTS, "a double alone"},
		{MAX_RECS, 8, 3, true, SHAPE_INTS, "eight columns, a double"},
		{2, 2, 1, false, SHAPE_LEAF_ADDR, "an address change and a leaf, two rows"},
		{MAX_RECS, 2, 7, false, SHAPE_LEAF_ADDR, "an address change and a leaf, jumps"},
		{MAX_RECS, 9, 3, false, SHAPE_LEAF_ADDR, "an address change and eight leaves"},
		{MAX_RECS, 1, 1, false, SHAPE_LEAF_ADDR, "an address change alone"},
		{MAX_RECS, 2, 2, false, SHAPE_STRS, "strings, mostly the same"},
		{MAX_RECS, 3, 4, false, SHAPE_STRS, "an integer and strings"},
		{MAX_RECS, 2, 2, false, SHAPE_NULLS, "a column that is null in every row"},
	};

	for (int i = 0; i < (int) lengthof(cases); i++)
	{
		Feed	   *f = malloc(sizeof(*f));

		fill(f, cases[i].n, cases[i].ncols, cases[i].dbl, cases[i].jump, cases[i].shape);
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
