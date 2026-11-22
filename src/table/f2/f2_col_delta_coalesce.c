/*
 * table/f2/f2_col_delta_coalesce.c - the records a transaction staged for one row become one, when the transaction is about to commit.
 *
 * A bulk UPDATE and an ON CONFLICT DO UPDATE touch many rows once, and a job that moves a row through its states touches one row many times: every record is 32 bytes and a page holds
 * about 250, so the pages of a chain (SIFT_COL_DELTA_MAX_PAGES) are spent on the same few rows. At the end of the transaction no reader can tell the commands of the writer apart (a reader of
 * another transaction sees all of them or none), so the records of one row fold into one that holds the last value of each column; a flush before that keeps every record, because a command
 * of the writer in between reads the row as it was then. Nothing here calls the server but the allocator.
 */
#include "postgres.h"

#include "access/xact.h"
#include "utils/memutils.h"

#include "table/f2/f2_col_delta.h"

static SiftColDeltaCol
col_of(const SiftColDeltaPatch *p)
{
	SiftColDeltaCol c = {.attnum = p->attnum,.kind = SIFT_CDCOL_KIND(p->flags)};

	c.cell.isnull = (p->flags & SIFT_CDCOL_IS_NULL) != 0;
	if (c.cell.isnull)
		return c;
	if (c.kind == SIFT_CELL_INT)
		c.cell.i = p->val.i;
	else if (c.kind == SIFT_CELL_F8)
		c.cell.f = p->val.f;
	else
	{
		c.cell.s = p->val.s;
		c.cell.slen = p->vlen;
	}
	return c;
}

/* The columns of a record appended to cols (a later value of a column replaces the earlier one); false when they are more than a record holds */
static bool
add_columns(const uint8 *rec, SiftColDeltaCol *cols, int *n)
{
	SiftColDeltaEntryHdr h;
	const uint8 *p = rec + sizeof(h),
			   *end;
	SiftColDeltaPatch patch;

	memcpy(&h, rec, sizeof(h));
	end = rec + h.rec_bytes;
	for (int k = 0; k < h.ncols; k++)
	{
		SiftColDeltaCol c;
		int			at = 0;

		p = sift_col_delta_decode_col(p, end, &patch);
		if (p == NULL)
			return false;
		c = col_of(&patch);
		while (at < *n && cols[at].attnum != c.attnum)
			at++;
		if (at == *n && *n == SIFT_COL_DELTA_MAX_COLS_PER_REC)
			return false;
		cols[at] = c;
		*n += at == *n;
	}
	return true;
}

uint32
sift_col_delta_merge_records(uint8 *dst, uint32 cap, const uint8 *first, const uint8 *second)
{
	SiftColDeltaEntryHdr a,
				b;
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC];
	int			n = 0;
	SiftColDeltaTarget t;
	uint8		flags;

	memcpy(&a, first, sizeof(a));
	memcpy(&b, second, sizeof(b));
	if (!add_columns(first, cols, &n) || !add_columns(second, cols, &n))
		return 0;
	t = (SiftColDeltaTarget) {.part_gen = a.part_gen,.granule_id = a.granule_id,.row_ordinal = a.row_ordinal,.tid = ((uint64) a.tid_blk << 16) | a.tid_off};
	flags = (uint8) (((a.flags | b.flags) & ~(SIFT_CDENT_FLAG_NOOP | SIFT_CDENT_FLAG_SUBXID)) | (n == 0 ? SIFT_CDENT_FLAG_NOOP : 0) | (b.flags & SIFT_CDENT_FLAG_SUBXID));
	return sift_col_delta_encode(dst, cap, &t, b.xmin, b.cid, flags, cols, n);
}

typedef struct Order
{
	uint64		part_id;
	uint64		part_gen;
	uint32		granule_id;
	uint16		row_ordinal;
	uint64		tid;
	TransactionId xmin;
	int			at;
} Order;

static int
order_cmp(const void *x, const void *y)
{
	const Order *a = x,
			   *b = y;

#define CMP(f) if (a->f != b->f) return a->f < b->f ? -1 : 1
	CMP(part_id);
	CMP(part_gen);
	CMP(granule_id);
	CMP(row_ordinal);
	CMP(tid);
	CMP(xmin);
	CMP(at);
#undef CMP
	return 0;
}

static bool
same_row(const Order *a, const Order *b)
{
	return a->part_id == b->part_id && a->part_gen == b->part_gen && a->granule_id == b->granule_id && a->row_ordinal == b->row_ordinal && a->tid == b->tid && a->xmin == b->xmin;
}

static Order
order_of(const SiftColDeltaStage *st, int at)
{
	SiftColDeltaEntryHdr h;

	memcpy(&h, st->items[at].rec, sizeof(h));
	return (Order) {st->items[at].part_id, h.part_gen, h.granule_id, h.row_ordinal, ((uint64) h.tid_blk << 16) | h.tid_off, h.xmin, at};
}

/* The item at i holds the merge of itself and the one at j, which is dropped (NULL rec); a pair that does not merge stays as it is */
static bool
fold_into(SiftColDeltaStage *st, int i, int j)
{
	uint8		rec[SIFT_COL_DELTA_MAX_REC_BYTES];
	SiftColDeltaEntryHdr was;
	uint32		n = sift_col_delta_merge_records(rec, sizeof(rec), st->items[i].rec, st->items[j].rec);
	uint8	   *copy;

	if (n == 0)
		return false;
	memcpy(&was, st->items[i].rec, sizeof(was));
	copy = MemoryContextAlloc(TopTransactionContext, n);
	memcpy(copy, rec, n);
	st->bytes = st->bytes - was.rec_bytes + n;
	pfree(st->items[i].rec);
	st->items[i].rec = copy;
	return true;
}

int
sift_col_delta_stage_coalesce(SiftColDeltaStage *st)
{
	Order	   *ord;
	int			kept = 0,
				folded = 0;

	if (st->n < 2)
		return 0;
	ord = palloc(sizeof(Order) * st->n);
	for (int i = 0; i < st->n; i++)
		ord[i] = order_of(st, i);
	qsort(ord, st->n, sizeof(Order), order_cmp);
	for (int k = 1; k < st->n; k++)
		if (same_row(&ord[k - 1], &ord[k]) && fold_into(st, ord[k - 1].at, ord[k].at))
		{
			SiftColDeltaEntryHdr gone;

			memcpy(&gone, st->items[ord[k].at].rec, sizeof(gone));
			st->bytes -= gone.rec_bytes;
			pfree(st->items[ord[k].at].rec);
			st->items[ord[k].at].rec = NULL;
			ord[k].at = ord[k - 1].at;
			ord[k].xmin = ord[k - 1].xmin;
			folded++;
		}
	for (int i = 0; i < st->n; i++)
		if (st->items[i].rec != NULL)
			st->items[kept++] = st->items[i];
	st->n = kept;
	pfree(ord);
	return folded;
}
