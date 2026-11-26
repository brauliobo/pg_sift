/*
 * table/f2/col_delta/batch_enc.h - the records of a run of rows of one part and granule written as one record (SIFT_CDENT_FLAG_BATCH).
 *
 * A batch record is a SiftColDeltaEntryHdr (the part, the granule, the writer and the flags every row of the run shares; row_ordinal and the address are those of its first row, rec_bytes the
 * bytes of the batch) and then: the number of rows (uint16, 2 or more) and a pad uint16; the SiftColDeltaColHdr of each of the ncols columns (kinds SIFT_CELL_INT, SIFT_CELL_F8 and SIFT_CELL_STR, no
 * tag; SIFT_CDCOL_IS_NULL says the column is null in every row of the batch, and a LEAF_ADDR batch starts with the column SIFT_CD_ATT_TID_REMAP, the new address of each row); and for each row the
 * zigzag varint of the change of its row ordinal and of its address from the row before (the header's for the first), and for each column that is not null the zigzag varint of the change of an
 * integer from the row before (0 before the first), the 8 bytes of a double, or a string: the varint 1 when it is the string of the row before (the last row it is not null in), the varint 3
 * when it is null in the row, else the varint of twice its length and its bytes (the writer never makes a string column SIFT_CDCOL_IS_NULL: its rows are alike null or not).
 * The record is padded to 8 bytes and is at most SIFT_COL_DELTA_MAX_REC_BYTES, as any record is.
 *
 * A page counts its rows, not its records, in nentries: what a reader meets is the rows of the page as the records they were (sift_col_delta_recs_next), in the order they were written, and
 * writes them back through a run (sift_col_delta_run_feed) so that they take what they took.
 */
#ifndef SIFT_TBL_F2_COL_DELTA_BATCH_ENC_H
#define SIFT_TBL_F2_COL_DELTA_BATCH_ENC_H

#include "table/f2/col_delta/col_delta.h"

#define SIFT_CDENT_FLAG_BATCH			(UINT8_C(1) << 7)	/* the record holds the rows of a run, see above */
#define SIFT_CDENT_BATCH_FLAGS			(SIFT_CDENT_FLAG_SUBXID | SIFT_CDENT_FLAG_HAS_TIME0 | SIFT_CDENT_FLAG_HAS_TIME1 | SIFT_CDENT_FLAG_LEAF_ADDR | SIFT_CDENT_FLAG_DIRTY_SCOPE)	/* the flags the rows of a batch may have */

/* ---- reading: the rows of a stretch of a page, as the records they were ---- */

typedef struct SiftColDeltaRecs
{
	const uint8 *p;				/* the next record of the page */
	const uint8 *end;
	const uint8 *rows;			/* the batch being read: where its next row starts, NULL when none */
	const uint8 *rows_end;
	SiftColDeltaEntryHdr bh;	/* its header */
	uint32		left;			/* its rows left */
	uint32		prev_ord;
	uint64		prev_tid;
	int			ncols;
	int16		attnum[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	uint8		cflags[SIFT_COL_DELTA_MAX_COLS_PER_REC];	/* SIFT_CDCOL_PACK_FLAGS of each column */
	int64		prev_i[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	const uint8 *prev_s[SIFT_COL_DELTA_MAX_COLS_PER_REC];	/* the string of the row before, with its length */
	uint16		prev_slen[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC];	/* the columns of the row of the batch last read */
	bool		strs;			/* the batch has a string column that is not null: its rows always come as records, whose strings a collector keeps */
	bool		hdrs_only;		/* the rows of a batch come as their headers, with no record (unless strs) */
	uint8		buf[SIFT_COL_DELTA_MAX_REC_BYTES];	/* the record of a row of a batch, until the next */
} SiftColDeltaRecs;

/* The records in [p, end), whole records of a page */
extern void sift_col_delta_recs_init(SiftColDeltaRecs *it, const uint8 *p, const uint8 *end);

/* The same for a reader that has no use for the columns: a row of a batch is a header (rec_bytes that of its batch) and no record, which is NULL; a row of a batch with strings is a record as in sift_col_delta_recs_init */
extern void sift_col_delta_recs_init_hdrs(SiftColDeltaRecs *it, const uint8 *p, const uint8 *end);

/* Whether a record is left */
static inline bool
sift_col_delta_recs_more(const SiftColDeltaRecs *it)
{
	return it->left > 0 || it->p < it->end;
}

/* The column k of the row of a batch just read, as the patch it is (any row, in the mode that has headers only too) */
extern void sift_col_delta_recs_patch(const SiftColDeltaRecs *it, int k, SiftColDeltaPatch *out);

/* The next record (the row of a batch as a record of its own, valid until the next call) and its header; false when there is none */
extern bool sift_col_delta_recs_next(SiftColDeltaRecs *it, const uint8 **rec, SiftColDeltaEntryHdr *h);

/* The rows a record holds: 1 for a record of one row */
extern uint32 sift_col_delta_rec_rows(const uint8 *rec);

/* NULL when the avail bytes at rec start a whole batch record, else why they do not */
extern const char *sift_col_delta_batch_check(const uint8 *rec, uint32 avail);

/* ---- writing: the records that are alike as the batch they make ---- */

typedef struct SiftColDeltaRun
{
	uint32		nrows;
	uint32		nbytes;			/* of the rows */
	uint32		prev_ord;
	uint64		prev_tid;
	SiftColDeltaEntryHdr first;	/* the header of the first row */
	int16		attnum[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	uint8		cflags[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	int64		prev_i[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	int32		prev_soff[SIFT_COL_DELTA_MAX_COLS_PER_REC];	/* where in rows the string of the row before starts, -1 for none */
	uint16		prev_slen[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	uint8		rows[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint8		single[SIFT_COL_DELTA_MAX_REC_BYTES];	/* the first record as it came, for a run of one */
} SiftColDeltaRun;

extern void sift_col_delta_run_init(SiftColDeltaRun *r);

/*
 * The record (one row) goes on: it joins the run when it is alike (the same part, granule, writer, flags, columns, and nulls) and the run has the room, else the run is written to out as one
 * record, and the record starts the next run or, when it cannot be a row of a batch, is written to out as it is. Returns the bytes written to out, at most two records of
 * SIFT_COL_DELTA_MAX_REC_BYTES each. rec NULL: the run is written, whatever it holds.
 */
extern uint32 sift_col_delta_run_feed(SiftColDeltaRun *r, const uint8 *rec, uint8 *out);

#define SIFT_COL_DELTA_RUN_OUT_BYTES	(2 * SIFT_COL_DELTA_MAX_REC_BYTES)

#endif
