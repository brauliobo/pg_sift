/*
 * table/f2/col_delta/batch_enc.c - the records of a run of rows written as one (SIFT_CDENT_FLAG_BATCH, table/f2/col_delta/batch_enc.h): the encoding of a run, the check of a batch record, and the
 * rows of a stretch of a page as the standard records they were. Nothing here calls the server.
 */
#include "postgres.h"

#include "table/f2/col_delta/batch_enc.h"

#define BATCH_FIXED_BYTES	((uint32) (sizeof(SiftColDeltaEntryHdr) + 2 * sizeof(uint16)))
#define STR_AGAIN			1	/* the string of a row is that of the row before; any other string is twice its length, then its bytes */

static inline uint64
tid_of(const SiftColDeltaEntryHdr *h)
{
	return sift_col_delta_hdr_tid(h);
}

static inline uint32
varint_len(uint64 x)
{
	uint8		tmp[10];

	return (uint32) (sift_put_varint(tmp, x) - tmp);
}

static inline int64
delta_of(int64 v, int64 prev)
{
	return (int64) ((uint64) v - (uint64) prev);
}

/* ---- the rows of a page ---- */

static inline uint16
batch_nrows(const uint8 *rec)
{
	uint16		n;

	memcpy(&n, rec + sizeof(SiftColDeltaEntryHdr), sizeof(n));
	return n;
}

uint32
sift_col_delta_rec_rows(const uint8 *rec)
{
	SiftColDeltaEntryHdr h;

	memcpy(&h, rec, sizeof(h));
	return (h.flags & SIFT_CDENT_FLAG_BATCH) != 0 ? batch_nrows(rec) : 1;
}

/* The columns of a batch record: their headers read into it, and where its rows start */
static const uint8 *
recs_open_batch(SiftColDeltaRecs *it, const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	const uint8 *p = rec + sizeof(*h) + 2 * sizeof(uint16);

	it->bh = *h;
	it->left = batch_nrows(rec);
	it->ncols = h->ncols;
	it->prev_ord = h->row_ordinal;
	it->prev_tid = tid_of(h);
	it->strs = false;
	for (int k = 0; k < it->ncols; k++)
	{
		SiftColDeltaColHdr ch;

		memcpy(&ch, p, sizeof(ch));
		p += sizeof(ch);
		it->attnum[k] = ch.attnum;
		it->cflags[k] = ch.flags;
		it->prev_i[k] = 0;
		it->prev_s[k] = NULL;
		it->prev_slen[k] = 0;
		it->strs |= SIFT_CDCOL_KIND(ch.flags) == SIFT_CELL_STR && (ch.flags & SIFT_CDCOL_IS_NULL) == 0;
	}
	it->rows_end = rec + h->rec_bytes;
	return p;
}

void
sift_col_delta_recs_init(SiftColDeltaRecs *it, const uint8 *p, const uint8 *end)
{
	it->p = p;
	it->end = end;
	it->rows = NULL;
	it->left = 0;
	it->hdrs_only = false;
}

void
sift_col_delta_recs_init_hdrs(SiftColDeltaRecs *it, const uint8 *p, const uint8 *end)
{
	sift_col_delta_recs_init(it, p, end);
	it->hdrs_only = true;
}

/* The next row of the batch being read as a record */
static void
recs_row(SiftColDeltaRecs *it, const uint8 **rec, SiftColDeltaEntryHdr *h)
{
	SiftColDeltaTarget t = {.part_gen = it->bh.part_gen,.granule_id = it->bh.granule_id};
	const uint8 *p = it->rows;
	uint64		x;
	uint32		n;

	p = sift_get_varint64_n(p, it->rows_end, &x);
	SIFT_INVARIANT(p != NULL, "a column delta batch was read before it was checked");
	it->prev_ord = (uint32) ((int64) it->prev_ord + sift_unzigzag(x));
	p = sift_get_varint64_n(p, it->rows_end, &x);
	SIFT_INVARIANT(p != NULL, "a column delta batch was read before it was checked");
	it->prev_tid = (uint64) ((int64) it->prev_tid + sift_unzigzag(x));
	t.row_ordinal = (uint16) it->prev_ord;
	t.tid = it->prev_tid;
	for (int k = 0; k < it->ncols; k++)
	{
		SiftCellKind kind = SIFT_CDCOL_KIND(it->cflags[k]);

		it->cols[k] = (SiftColDeltaCol) {.attnum = it->attnum[k],.kind = kind};
		if ((it->cflags[k] & SIFT_CDCOL_IS_NULL) != 0)
			it->cols[k].cell.isnull = true;
		else if (kind == SIFT_CELL_F8)
		{
			SIFT_INVARIANT(it->rows_end - p >= (ptrdiff_t) sizeof(double), "a column delta batch was read before it was checked");
			memcpy(&it->cols[k].cell.f, p, sizeof(double));
			p += sizeof(double);
		}
		else if (kind == SIFT_CELL_STR)
		{
			p = sift_get_varint_n(p, it->rows_end, &x);
			SIFT_INVARIANT(p != NULL, "a column delta batch was read before it was checked");
			if (x != STR_AGAIN)
			{
				it->prev_slen[k] = (uint16) (x >> 1);
				it->prev_s[k] = p;
				p += it->prev_slen[k];
			}
			it->cols[k].cell.s = (const char *) it->prev_s[k];
			it->cols[k].cell.slen = it->prev_slen[k];
		}
		else
		{
			p = sift_get_varint64_n(p, it->rows_end, &x);
			SIFT_INVARIANT(p != NULL, "a column delta batch was read before it was checked");
			it->prev_i[k] = (int64) ((uint64) it->prev_i[k] + (uint64) sift_unzigzag(x));
			it->cols[k].cell.i = it->prev_i[k];
		}
	}
	it->rows = p;
	it->left--;
	if (it->hdrs_only && !it->strs)
	{
		*h = it->bh;
		h->flags &= (uint8) ~SIFT_CDENT_FLAG_BATCH;
		h->row_ordinal = (uint16) it->prev_ord;
		h->tid_blk = (BlockNumber) (it->prev_tid >> 16);
		h->tid_off = (OffsetNumber) (it->prev_tid & 0xFFFF);
		*rec = NULL;
		return;
	}
	n = sift_col_delta_encode(it->buf, sizeof(it->buf), &t, it->bh.xmin, it->bh.cid, (uint8) (it->bh.flags & ~SIFT_CDENT_FLAG_BATCH), it->cols, it->ncols);
	SIFT_INVARIANT(n != 0, "a row of a column delta batch does not fit a record");
	memcpy(h, it->buf, sizeof(*h));
	*rec = it->buf;
}

bool
sift_col_delta_recs_next(SiftColDeltaRecs *it, const uint8 **rec, SiftColDeltaEntryHdr *h)
{
	if (it->left == 0)
	{
		if (it->p >= it->end)
			return false;
		memcpy(h, it->p, sizeof(*h));
		if ((h->flags & SIFT_CDENT_FLAG_BATCH) == 0)
		{
			*rec = it->p;
			it->p += h->rec_bytes;
			return true;
		}
		it->rows = recs_open_batch(it, it->p, h);
		it->p += h->rec_bytes;
	}
	recs_row(it, rec, h);
	return true;
}

void
sift_col_delta_recs_patch(const SiftColDeltaRecs *it, int k, SiftColDeltaPatch *out)
{
	memset(out, 0, sizeof(*out));
	out->row_ordinal = (uint16) it->prev_ord;
	out->attnum = it->cols[k].attnum;
	out->flags = it->cflags[k];
	if (it->cols[k].cell.isnull)
		return;
	if (it->cols[k].kind == SIFT_CELL_F8)
		out->val.f = it->cols[k].cell.f;
	else if (it->cols[k].kind == SIFT_CELL_STR)
	{
		out->val.s = it->cols[k].cell.s;
		out->vlen = (uint16) it->cols[k].cell.slen;
	}
	else
		out->val.i = it->cols[k].cell.i;
}

/* ---- checking ---- */

const char *
sift_col_delta_batch_check(const uint8 *rec, uint32 avail)
{
	SiftColDeltaEntryHdr h;
	const uint8 *p,
			   *end;
	uint32		nrows;
	int16		prev = -1;
	bool		leaf_addr;
	bool		seen_str[SIFT_COL_DELTA_MAX_COLS_PER_REC] = {false};
	int64		ord,
				tid;

	memcpy(&h, rec, sizeof(h));
	leaf_addr = (h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) != 0;
	if ((h.flags & ~(SIFT_CDENT_FLAG_BATCH | SIFT_CDENT_BATCH_FLAGS)) != 0 || h.ncols == 0 ||
		h.ncols > (leaf_addr ? SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR : SIFT_COL_DELTA_MAX_COLS_PER_REC))
		return "a batch has flags or a column count the format does not allow";
	if (avail < h.rec_bytes || h.rec_bytes < BATCH_FIXED_BYTES + h.ncols * sizeof(SiftColDeltaColHdr))
		return "a batch is shorter than its header and its columns";
	nrows = batch_nrows(rec);
	if (nrows < 2)
		return "a batch has fewer rows than the format makes a batch of";
	p = rec + BATCH_FIXED_BYTES;
	end = rec + h.rec_bytes;
	for (int k = 0; k < h.ncols; k++)
	{
		SiftColDeltaColHdr ch;

		memcpy(&ch, p, sizeof(ch));
		p += sizeof(ch);
		if (ch.attnum <= prev || ch.pad != 0 || ch.flags != SIFT_CDCOL_PACK_FLAGS((ch.flags & SIFT_CDCOL_IS_NULL) != 0, SIFT_CDCOL_KIND(ch.flags)) ||
			(SIFT_CDCOL_KIND(ch.flags) != SIFT_CELL_INT && SIFT_CDCOL_KIND(ch.flags) != SIFT_CELL_F8 && SIFT_CDCOL_KIND(ch.flags) != SIFT_CELL_STR) ||
			(ch.attnum == SIFT_CD_ATT_TID_REMAP && (!leaf_addr || SIFT_CDCOL_KIND(ch.flags) != SIFT_CELL_INT || (ch.flags & SIFT_CDCOL_IS_NULL) != 0)))
			return "a column of a batch is not one of the format";
		prev = ch.attnum;
	}
	ord = h.row_ordinal;
	tid = (int64) tid_of(&h);
	for (uint32 r = 0; r < nrows; r++)
	{
		uint64		x;

		p = sift_get_varint64_n(p, end, &x);
		if (p == NULL || (r == 0 && x != 0))
			return "the row of a batch is not one of the format";
		ord += sift_unzigzag(x);
		p = sift_get_varint64_n(p, end, &x);
		if (p == NULL || (r == 0 && x != 0))
			return "the row of a batch is not one of the format";
		tid += sift_unzigzag(x);
		if (ord < 0 || ord >= SIFT_COL_DELTA_MAX_ROW_ORDINAL || (tid & 0xFFFF) == 0)
			return "a batch names a row the format does not have";
		for (int k = 0; k < h.ncols; k++)
		{
			SiftColDeltaColHdr ch;

			memcpy(&ch, rec + BATCH_FIXED_BYTES + k * sizeof(ch), sizeof(ch));
			if ((ch.flags & SIFT_CDCOL_IS_NULL) != 0)
				continue;
			if (SIFT_CDCOL_KIND(ch.flags) == SIFT_CELL_F8)
			{
				if (end - p < (ptrdiff_t) sizeof(double))
					return "the row of a batch is not one of the format";
				p += sizeof(double);
			}
			else if (SIFT_CDCOL_KIND(ch.flags) == SIFT_CELL_STR)
			{
				p = sift_get_varint_n(p, end, &x);
				if (p == NULL || (x == STR_AGAIN ? !seen_str[k] : ((x & 1) != 0 || (x >> 1) > SIFT_COL_DELTA_MAX_PAYLOAD || (uint64) (end - p) < (x >> 1))))
					return "the row of a batch is not one of the format";
				if (x != STR_AGAIN)
					p += x >> 1;
				seen_str[k] = true;
			}
			else if ((p = sift_get_varint64_n(p, end, &x)) == NULL)
				return "the row of a batch is not one of the format";
		}
	}
	if (end - p >= 8)
		return "a batch has bytes past its rows";
	for (; p < end; p++)
		if (*p != 0)
			return "the padding of a batch is not zero";
	return NULL;
}

/* ---- writing ---- */

void
sift_col_delta_run_init(SiftColDeltaRun *r)
{
	r->nrows = 0;
	r->nbytes = 0;
}

/* A record as a row of a batch: its header and, for each column, its flags and the value as 8 bytes (the integer, or the bits of the double) or the bytes of the string; false when it cannot be one */
typedef struct Row
{
	SiftColDeltaEntryHdr h;
	int16		attnum[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	uint8		cflags[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	int64		v[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	const uint8 *s[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	uint16		slen[SIFT_COL_DELTA_MAX_COLS_PER_REC];
} Row;

static inline bool
col_is_null(uint8 cflags)
{
	return (cflags & SIFT_CDCOL_IS_NULL) != 0;
}

static bool
row_of(const uint8 *rec, Row *row)
{
	const uint8 *p = rec + sizeof(row->h),
			   *end;

	memcpy(&row->h, rec, sizeof(row->h));
	end = rec + row->h.rec_bytes;
	if ((row->h.flags & ~SIFT_CDENT_BATCH_FLAGS) != 0 || row->h.ncols == 0)
		return false;
	for (int k = 0; k < row->h.ncols; k++)
	{
		SiftColDeltaPatch col;

		p = sift_col_delta_decode_col(p, end, &col);
		SIFT_INVARIANT(p != NULL, "a column delta record was added to a run before it was checked");
		if ((col.flags & (SIFT_CDCOL_HAS_TAG | SIFT_CDCOL_KEY_ABSENT)) != 0)
			return false;
		row->attnum[k] = col.attnum;
		row->cflags[k] = col.flags;
		if (col_is_null(col.flags))
			continue;
		if (SIFT_CDCOL_KIND(col.flags) == SIFT_CELL_F8)
			memcpy(&row->v[k], &col.val.f, sizeof(double));
		else if (SIFT_CDCOL_KIND(col.flags) == SIFT_CELL_STR)
		{
			row->s[k] = (const uint8 *) col.val.s;
			row->slen[k] = col.vlen;
		}
		else
			row->v[k] = col.val.i;
	}
	return true;
}

/* Whether the row is alike the rows of the run: what a batch shares */
static bool
run_alike(const SiftColDeltaRun *r, const Row *row)
{
	const SiftColDeltaEntryHdr *f = &r->first;

	if (f->part_gen != row->h.part_gen || f->granule_id != row->h.granule_id || f->xmin != row->h.xmin || f->cid != row->h.cid || f->flags != row->h.flags || f->ncols != row->h.ncols)
		return false;
	for (int k = 0; k < f->ncols; k++)
		if (r->attnum[k] != row->attnum[k] || r->cflags[k] != row->cflags[k])
			return false;
	return true;
}

static uint32
batch_bytes(const SiftColDeltaRun *r, uint32 row_bytes)
{
	return (uint32) TYPEALIGN(8, BATCH_FIXED_BYTES + r->first.ncols * sizeof(SiftColDeltaColHdr) + r->nbytes + row_bytes);
}

/* Whether the string of the column is that of the row before */
static bool
str_again(const SiftColDeltaRun *r, const Row *row, int k)
{
	return r->prev_soff[k] >= 0 && r->prev_slen[k] == row->slen[k] && memcmp(r->rows + r->prev_soff[k], row->s[k], row->slen[k]) == 0;
}

/* The bytes the row adds to the run */
static uint32
row_bytes(const SiftColDeltaRun *r, const Row *row)
{
	uint32		n = varint_len(sift_zigzag(delta_of(row->h.row_ordinal, r->prev_ord))) + varint_len(sift_zigzag(delta_of((int64) tid_of(&row->h), (int64) r->prev_tid)));

	for (int k = 0; k < row->h.ncols; k++)
	{
		if (col_is_null(row->cflags[k]))
			continue;
		if (SIFT_CDCOL_KIND(row->cflags[k]) == SIFT_CELL_F8)
			n += (uint32) sizeof(double);
		else if (SIFT_CDCOL_KIND(row->cflags[k]) == SIFT_CELL_STR)
			n += str_again(r, row, k) ? 1 : varint_len((uint64) row->slen[k] << 1) + row->slen[k];
		else
			n += varint_len(sift_zigzag(delta_of(row->v[k], r->prev_i[k])));
	}
	return n;
}

static uint8 *
put_str(SiftColDeltaRun *r, uint8 *p, const Row *row, int k)
{
	if (str_again(r, row, k))
		return sift_put_varint(p, STR_AGAIN);
	p = sift_put_varint(p, (uint64) row->slen[k] << 1);
	r->prev_soff[k] = (int32) (p - r->rows);
	r->prev_slen[k] = row->slen[k];
	memcpy(p, row->s[k], row->slen[k]);
	return p + row->slen[k];
}

static void
run_put(SiftColDeltaRun *r, const uint8 *rec, const Row *row)
{
	uint8	   *p;

	if (r->nrows == 0)
	{
		r->nbytes = 0;
		r->first = row->h;
		memcpy(r->single, rec, row->h.rec_bytes);
		memcpy(r->attnum, row->attnum, sizeof(r->attnum));
		memcpy(r->cflags, row->cflags, sizeof(r->cflags));
		memset(r->prev_i, 0, sizeof(r->prev_i));
		memset(r->prev_soff, 0xFF, sizeof(r->prev_soff));
		memset(r->prev_slen, 0, sizeof(r->prev_slen));
		r->prev_ord = row->h.row_ordinal;
		r->prev_tid = tid_of(&row->h);
	}
	p = r->rows + r->nbytes;
	p = sift_put_varint(p, sift_zigzag(delta_of(row->h.row_ordinal, r->prev_ord)));
	p = sift_put_varint(p, sift_zigzag(delta_of((int64) tid_of(&row->h), (int64) r->prev_tid)));
	for (int k = 0; k < row->h.ncols; k++)
	{
		if (col_is_null(row->cflags[k]))
			continue;
		if (SIFT_CDCOL_KIND(row->cflags[k]) == SIFT_CELL_F8)
		{
			memcpy(p, &row->v[k], sizeof(double));
			p += sizeof(double);
		}
		else if (SIFT_CDCOL_KIND(row->cflags[k]) == SIFT_CELL_STR)
			p = put_str(r, p, row, k);
		else
		{
			p = sift_put_varint(p, sift_zigzag(delta_of(row->v[k], r->prev_i[k])));
			r->prev_i[k] = row->v[k];
		}
	}
	r->prev_ord = row->h.row_ordinal;
	r->prev_tid = tid_of(&row->h);
	r->nbytes = (uint32) (p - r->rows);
	r->nrows++;
}

/* The run as the record it is: a batch of its rows, or the record of the one row it holds */
static uint32
run_take(SiftColDeltaRun *r, uint8 *out)
{
	SiftColDeltaEntryHdr h = r->first;
	uint16		nrows = (uint16) r->nrows;
	uint16		pad = 0;
	uint8	   *p = out;

	if (r->nrows == 0)
		return 0;
	if (r->nrows == 1)
	{
		r->nrows = 0;
		memcpy(out, r->single, h.rec_bytes);
		return h.rec_bytes;
	}
	h.flags |= SIFT_CDENT_FLAG_BATCH;
	h.rec_bytes = (uint16) batch_bytes(r, 0);
	memset(out, 0, h.rec_bytes);
	memcpy(p, &h, sizeof(h));
	p += sizeof(h);
	memcpy(p, &nrows, sizeof(nrows));
	memcpy(p + sizeof(nrows), &pad, sizeof(pad));
	p += 2 * sizeof(uint16);
	for (int k = 0; k < h.ncols; k++)
	{
		SiftColDeltaColHdr ch = {.attnum = r->attnum[k],.flags = r->cflags[k]};

		memcpy(p, &ch, sizeof(ch));
		p += sizeof(ch);
	}
	memcpy(p, r->rows, r->nbytes);
	r->nrows = 0;
	return h.rec_bytes;
}

uint32
sift_col_delta_run_feed(SiftColDeltaRun *r, const uint8 *rec, uint8 *out)
{
	Row			row;
	uint32		n;
	bool		fits;

	if (rec == NULL)
		return run_take(r, out);
	fits = row_of(rec, &row);
	if (fits && r->nrows > 0 && run_alike(r, &row) && r->nrows < UINT16_MAX && batch_bytes(r, row_bytes(r, &row)) <= SIFT_COL_DELTA_MAX_REC_BYTES)
	{
		run_put(r, rec, &row);
		return 0;
	}
	n = run_take(r, out);
	if (fits)
		run_put(r, rec, &row);
	else
	{
		memcpy(out + n, rec, row.h.rec_bytes);
		n += row.h.rec_bytes;
	}
	return n;
}
