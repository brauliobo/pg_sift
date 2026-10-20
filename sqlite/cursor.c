/*
 * cursor.c - xBestIndex and the cursors of the SQLite table: MATCH scans (ranked, or in rowid order) and scans of the content
 * rows.
 */
#include "vtab_int.h"

SQLITE_EXTENSION_INIT3

/* ---------- best index ---------- */

#define IDX_MATCH	0x01			/* MATCH constraints, count in the bits above IDX_NSHIFT */
#define IDX_RANK	0x02			/* ranked: top-k in rank order */
#define IDX_LIMIT	0x04
#define IDX_ROWID	0x08
#define IDX_OFFSET	0x10
#define IDX_NSHIFT	8

int
sqBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *info)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	int			idx = 0,
				nmatch = 0,
				argc = 0,
				limit = -1,
				offset = -1,
				rowid = -1;

	for (int i = 0; i < info->nConstraint; i++)
	{
		const struct sqlite3_index_constraint *c = &info->aConstraint[i];

		if (!c->usable)
			continue;
		if (c->op == SQLITE_INDEX_CONSTRAINT_MATCH && c->iColumn == vt->ncols)
		{
			info->aConstraintUsage[i].argvIndex = ++argc;
			info->aConstraintUsage[i].omit = 1;
			nmatch++;
		}
		else if (c->op == SQLITE_INDEX_CONSTRAINT_LIMIT)
			limit = i;
		else if (c->op == SQLITE_INDEX_CONSTRAINT_OFFSET)
			offset = i;
		else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn == -1)
			rowid = i;
	}
	if (nmatch > 0)
	{
		bool		rank_used = (info->colUsed >> (vt->ncols + 1)) & 1,
					by_rank = info->nOrderBy == 1 && info->aOrderBy[0].iColumn == vt->ncols + 1 && !info->aOrderBy[0].desc,
					by_rowid = info->nOrderBy == 1 && info->aOrderBy[0].iColumn == -1 && !info->aOrderBy[0].desc;

		idx |= IDX_MATCH | (nmatch << IDX_NSHIFT);
		/* the matches arrive in rank order when rank is read or asked for, in rowid order otherwise */
		if (rank_used || by_rank)
			idx |= IDX_RANK;
		info->orderByConsumed = (idx & IDX_RANK) ? by_rank : by_rowid;
		if ((idx & IDX_RANK) && limit >= 0)
		{
			info->aConstraintUsage[limit].argvIndex = ++argc;
			idx |= IDX_LIMIT;
			if (offset >= 0)
			{
				info->aConstraintUsage[offset].argvIndex = ++argc;
				idx |= IDX_OFFSET;
			}
		}
		info->estimatedCost = (idx & IDX_LIMIT) ? 50 : 1000;
		info->estimatedRows = (idx & IDX_LIMIT) ? 10 : 10000;
	}
	else if (rowid >= 0)
	{
		info->aConstraintUsage[rowid].argvIndex = ++argc;
		info->aConstraintUsage[rowid].omit = 1;
		idx |= IDX_ROWID;
		info->estimatedCost = 2;
		info->estimatedRows = 1;
		info->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
	}
	else
	{
		info->estimatedCost = 1e6;
		info->estimatedRows = 1000000;
		if (info->nOrderBy == 1 && info->aOrderBy[0].iColumn == -1 && !info->aOrderBy[0].desc)
			info->orderByConsumed = 1;
	}
	info->idxNum = idx;
	return SQLITE_OK;
}

/* ---------- cursors ---------- */

int
sqOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	SqCursor   *c = sqlite3_malloc(sizeof(SqCursor));

	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof(*c));
	c->vt = vt;
	c->mode = SC_EOF;
	vt->ncursors++;
	sq_store_hold(vt->st, 1);
	*ppCursor = &c->base;
	return SQLITE_OK;
}

static void
cursor_reset(SqCursor *c)
{
	if (c->cx)
		MemoryContextDelete(c->cx);
	c->cx = NULL;
	c->state = NULL;
	c->plan = NULL;
	c->ranker = NULL;
	c->iter = NULL;
	c->qs = NULL;
	c->nqs = 0;
	c->hits = NULL;
	c->nhits = c->hitpos = 0;
	c->have_pm = false;
	sqlite3_finalize(c->rows);
	c->rows = NULL;
	if (c->row)
		row_stmt_give_back(c->vt, c->row);
	c->row = NULL;
	c->mode = SC_EOF;
}

int
sqClose(sqlite3_vtab_cursor *cur)
{
	SqCursor   *c = (SqCursor *) cur;

	cursor_reset(c);
	c->vt->ncursors--;
	sq_store_hold(c->vt->st, -1);
	sqlite3_free(c);
	return SQLITE_OK;
}

int
sqEof(sqlite3_vtab_cursor *cur)
{
	return ((SqCursor *) cur)->eof;
}

static void
ranked_next(SqCursor *c)
{
	ItemPointerData tid;
	float4		score;

	if (c->hits)
	{
		c->eof = c->hitpos >= c->nhits;
		if (!c->eof)
		{
			c->rowid = c->hits[c->hitpos].rowid;
			c->score = c->hits[c->hitpos++].score;
		}
		return;
	}
	c->eof = !sift_rank_next(c->ranker, &tid, &score);
	if (!c->eof)
	{
		c->rowid = sq_rowid(tid.blk, tid.off);
		c->score = score;
	}
}

static void
iter_next(SqCursor *c)
{
	for (;;)
	{
		while (c->have_pm)
		{
			int			w = 0,
						off;

			while (w < SIFT_OFFMASK_WORDS && c->left[w] == 0)
				w++;
			if (w == SIFT_OFFMASK_WORDS)
				break;
			off = w * 64 + pg_rightmost_one_pos64(c->left[w]) + 1;
			c->left[w] &= c->left[w] - 1;
			c->rowid = sq_rowid(c->pm.blk, off);
			if (c->pm.recheck && !sift_page_sure(&c->pm, off) &&
				!sift_plan_recheck(c->plan, fetch_text(c->vt, c->rowid)))
				continue;
			return;
		}
		c->have_pm = sift_iter_next(c->iter, &c->pm);
		if (!c->have_pm)
		{
			c->eof = true;
			return;
		}
		memcpy(c->left, c->pm.offmask, sizeof(c->left));
	}
}

static void
rows_next(SqCursor *c)
{
	int			rc = sqlite3_step(c->rows);

	c->eof = rc != SQLITE_ROW;
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		elog(ERROR, "sift: %s", sqlite3_errmsg(c->vt->db));
	if (!c->eof)
		c->rowid = sqlite3_column_int64(c->rows, 0);
}

/* The rows the scan has to produce: LIMIT plus OFFSET, -1 for all */
static int64_t
ranked_limit(int idx, int *a, sqlite3_value **argv)
{
	int64_t		k = -1;

	if (idx & IDX_LIMIT)
	{
		k = sqlite3_value_int64(argv[(*a)++]);
		if (idx & IDX_OFFSET)
		{
			int64_t		skip = sqlite3_value_int64(argv[(*a)++]);

			k += Max(skip, 0);
		}
	}
	return k > 0 ? k : (k == 0 ? 1 : -1);
}

static void
start_match(SqCursor *c, int idx, sqlite3_value **argv)
{
	SqVtab	   *vt = c->vt;
	int			nmatch = idx >> IDX_NSHIFT,
				a = nmatch;

	SqQuery    *texts = palloc(nmatch * sizeof(SqQuery));

	c->state = sift_state_open(sq_store_rel(vt->st), c->cx);
	c->qs = palloc(nmatch * sizeof(SiftQuery *));
	c->nqs = nmatch;
	for (int i = 0; i < nmatch; i++)
	{
		const char *q = (const char *) sqlite3_value_text(argv[i]);

		texts[i] = (SqQuery) {q ? q : "", sqlite3_value_bytes(argv[i])};
		c->qs[i] = sift_state_query(c->state, texts[i].text, texts[i].len);
	}
	c->plan = sift_plan_bind(c->state, c->qs, nmatch, (idx & IDX_RANK) ? SIFT_BIND_SCORE : 0);
	if (idx & IDX_RANK)
	{
		int64_t		k = ranked_limit(idx, &a, argv);

		c->mode = SC_RANK;
		if (k > 0 && sq_par_available(vt, c->plan, texts, nmatch))
			c->hits = sq_par_rank(vt, texts, nmatch, k, &c->nhits);
		if (c->hits == NULL)
			c->ranker = sift_rank_begin(c->plan, k, sq_vt_verify, vt);
		ranked_next(c);
	}
	else
	{
		c->iter = sift_iter_begin(c->plan);
		c->mode = SC_ITER;
		iter_next(c);
	}
}

static void
start_rows(SqCursor *c, int idx, sqlite3_value **argv)
{
	c->rows = source_stmt(c->vt, (idx & IDX_ROWID) ? SRC_ID : SRC_IDS, 0);
	if (idx & IDX_ROWID)
		sqlite3_bind_value(c->rows, 1, argv[0]);
	c->mode = SC_ROWS;
	rows_next(c);
}

int
sqFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;
	TRY_BEGIN(vt)
	{
		cursor_reset(c);
		c->eof = false;
		c->cx = AllocSetContextCreate(NULL, "sift scan", ALLOCSET_DEFAULT_SIZES);
		MemoryContextSwitchTo(c->cx);
		if (!vt->in_txn && sq_store_refresh(vt->st, true))
			load_settings(vt);
		if (idxNum & IDX_MATCH)
		{
			sq_vt_flush(vt, false);
			start_match(c, idxNum, argv);
		}
		else
			start_rows(c, idxNum, argv);
	}
	SQ_CATCH();
	cursor_reset(c);
	c->eof = true;
	rc = fail(vt);
	SQ_END();
	MemoryContextSwitchTo(call_);
	sq_store_leave(prev_);
	sq_leave(old_);
	return rc;
}

int
sqNext(sqlite3_vtab_cursor *cur)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;
	TRY_BEGIN(vt)
	{
		MemoryContextSwitchTo(c->cx);
		if (c->row)
		{
			row_stmt_give_back(vt, c->row);
			c->row = NULL;
		}
		if (c->mode == SC_RANK)
			ranked_next(c);
		else if (c->mode == SC_ITER)
			iter_next(c);
		else if (c->mode == SC_ROWS)
			rows_next(c);
	}
	TRY_END(vt)
}

int
sqRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid)
{
	*pRowid = ((SqCursor *) cur)->rowid;
	return SQLITE_OK;
}

/* Steps the content row of the current document; false on an error, which is the SQLite error of ctx */
static bool
load_row(SqCursor *c, sqlite3_context *ctx)
{
	SqVtab	   *vt = c->vt;
	MemoryContext old = sq_enter();
	SqStore    *prev = sq_store_enter(vt->st);
	bool		failed = false;

	SQ_TRY();
	c->row = row_stmt(vt);
	sqlite3_bind_int64(c->row, 1, c->rowid);
	if (sqlite3_step(c->row) != SQLITE_ROW)
	{
		row_stmt_give_back(vt, c->row);
		c->row = NULL;
	}
	SQ_CATCH();
	failed = true;
	SQ_END();
	sq_store_leave(prev);
	sq_leave(old);
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	return !failed;
}

int
sqColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;

	if (i == vt->ncols)
	{
		sqlite3_result_pointer(ctx, c, "sift_cursor", NULL);
		return SQLITE_OK;
	}
	if (i == vt->ncols + 1)
	{
		if (c->mode == SC_RANK)
			sqlite3_result_double(ctx, -c->score);
		return SQLITE_OK;
	}
	if (vt->content == SQ_CONTENT_NONE)
		return SQLITE_OK;
	if (c->row == NULL && !load_row(c, ctx))
		return SQLITE_ERROR;
	if (c->row)
		sqlite3_result_value(ctx, sqlite3_column_value(c->row, i));
	return SQLITE_OK;
}
