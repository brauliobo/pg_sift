/*
 * col/batch_test.c - L1 tests: the rows of a batch, copies, merges, versions and refused rows.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/ver.h"
#include "col/enc.h"
#include "col/enc_test_int.h"

/* Two stored values are equal: the same NULL/overflow state, the same key (floats: the bits, but every NaN is one value) or bytes */
static bool
value_eq(SiftColStore store, const SiftColValue *a, const SiftColValue *b)
{
	if ((a->isnull || a->overflow) != (b->isnull || b->overflow) || (a->overflow != b->overflow) || a->tag != b->tag)
		return false;
	if (a->isnull || a->overflow)
		return true;
	switch (store)
	{
		case SIFT_COLS_INT:
			return a->v.i == b->v.i;
		case SIFT_COLS_STR:
		case SIFT_COLS_VAR:
			return a->len == b->len && memcmp(a->v.s, b->v.s, a->len) == 0;
		default:
			return sift_col_f8_key(a->v.f) == sift_col_f8_key(b->v.f);
	}
}

static bool
row_eq(const SiftColSchema *s, const SiftColRow *a, const SiftColRow *b)
{
	if (!ItemPointerEquals((ItemPointer) &a->tid, (ItemPointer) &b->tid))
		return false;
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
			continue;
		if (!value_eq(d->store, &a->val[id], &b->val[id]))
			return false;
		if (d->kind == SIFT_COLK_GROUP && !a->val[id].isnull && !a->val[id].overflow)
			for (int ch = id + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == id)
					for (int64 j = 0; j < a->val[id].v.i; j++)
						if (!value_eq(s->cols[ch].store, &a->elem[ch][j], &b->elem[ch][j]))
							return false;
	}
	return true;
}

/* The batch holds exactly the rows (in order) */
void
check_batch_rows(TCtx *t, const char *what, const Case *c, const SiftColBatch *b, const SiftColRow *rows, uint32 n)
{
	SiftColRow	got = {0};
	uint32		bad = 0;

	CHECK(t, b->nrows == n, "%s: %u rows, expected %u", what, b->nrows, n);
	for (uint32 r = 0; r < Min(n, b->nrows); r++)
	{
		sift_cbatch_row(b, r, &got);
		bad += !row_eq(c->schema, &got, &rows[r]);
	}
	CHECK(t, bad == 0, "%s: %u rows differ", what, bad);
	sift_cbatch_row_free(&got);
}

void
check_merge(TCtx *t, const Case *c, const SiftKey *keys, uint32 n)
{
	SiftKey    *ka = palloc(Max(n, 1) * sizeof(SiftKey)),
			   *kb = palloc(Max(n, 1) * sizeof(SiftKey));
	SiftColRow *want = palloc(Max(n, 1) * sizeof(SiftColRow));
	uint32		na = 0,
				nb = 0;
	SiftColBatch *src[2],
			   *dst = sift_cbatch_create(CurrentMemoryContext, c->schema);
	SiftColRow *ra,
			   *rb;

	for (uint32 i = 0; i < n; i++)
	{
		bool		a = RAND(t, 5) < 3,
					b = RAND(t, 5) < 3 || !a;

		if (a)
			ka[na++] = keys[i];
		if (b)
			kb[nb++] = keys[i];
	}
	ra = gen_rows(t, c, ka, na);
	rb = gen_rows(t, c, kb, nb);
	for (uint32 i = 0, ia = 0, ib = 0; i < n; i++)
	{
		if (ia < na && ka[ia] == keys[i])
			want[i] = ra[ia++];
		else
			want[i] = rb[ib];
		if (ib < nb && kb[ib] == keys[i])
			ib++;
	}
	src[0] = batch_of(c, ra, na);
	src[1] = batch_of(c, rb, nb);
	sift_cbatch_reset(dst, c->sb);
	sift_cbatch_merge(dst, src, 2);
	check_batch_rows(t, "merge", c, dst, want, n);
}

/* The vectors of the versions of n rows: all frozen, one writer, a run of single row inserts, a few writers, any values at the edges */
static void
gen_vers(TCtx *t, uint32 n, uint32 *xm, uint32 *cm)
{
	uint32		shape = RAND(t, 5),
				x = 3 + RAND(t, 1000000);

	for (uint32 i = 0; i < n; i++)
	{
		switch (shape)
		{
			case 0:
				xm[i] = cm[i] = 0;
				break;
			case 1:
				xm[i] = x;
				cm[i] = 7;
				break;
			case 2:
				xm[i] = x;
				cm[i] = 1 + i;
				break;
			case 3:
				xm[i] = i == 0 ? x : xm[i - 1] + (CHANCE(t, 0.01) ? 1 + RAND(t, 5) : 0);
				cm[i] = xm[i] == x ? 1 + i : 1 + RAND(t, 4);
				break;
			default:
				xm[i] = CHANCE(t, 0.3) ? 0 : (CHANCE(t, 0.5) ? PG_UINT32_MAX - RAND(t, 3) : RAND(t, 4));
				cm[i] = xm[i] == 0 ? 0 : (CHANCE(t, 0.5) ? PG_UINT32_MAX : RAND(t, 1000));
		}
		if (xm[i] == 0)
			cm[i] = 0;
	}
}

static void
check_vec(TCtx *t, const uint32 *v, uint32 n)
{
	StringInfoData buf;
	uint32	   *out = palloc(n * sizeof(uint32));
	uint8	   *bad;

	initStringInfo(&buf);
	sift_cver_encode(v, n, &buf);
	CHECK(t, sift_cver_decode((uint8 *) buf.data, buf.len, n, out) && memcmp(out, v, n * sizeof(uint32)) == 0, "a version vector of %u rows does not round trip", n);
	CHECK(t, !sift_cver_decode((uint8 *) buf.data, buf.len - 1, n, out), "a vector one byte short decoded");
	bad = palloc(buf.len);
	for (int k = 0; k < 20; k++)
	{
		memcpy(bad, buf.data, buf.len);
		bad[RAND(t, buf.len)] ^= 1 << RAND(t, 8);
		(void) sift_cver_decode(bad, buf.len, n, out);	/* any answer, no read outside the bytes */
	}
	pfree(out);
	pfree(bad);
	pfree(buf.data);
}

/* The versions of the rows are kept by the vector codec, and by copies and merges of batches */
void
check_vers(TCtx *t, const Case *c, const SiftColRow *rows, uint32 n)
{
	uint32	   *xm = palloc(n * sizeof(uint32)),
			   *cm = palloc(n * sizeof(uint32));
	SiftColBatch *b = sift_cbatch_create(CurrentMemoryContext, c->schema),
			   *copy = sift_cbatch_create(CurrentMemoryContext, c->schema),
			   *odd = sift_cbatch_create(CurrentMemoryContext, c->schema),
			   *even = sift_cbatch_create(CurrentMemoryContext, c->schema),
			   *merged = sift_cbatch_create(CurrentMemoryContext, c->schema),
			   *src[2];
	bool		any = false;

	gen_vers(t, n, xm, cm);
	check_vec(t, xm, n);
	check_vec(t, cm, n);
	sift_cbatch_reset(b, c->sb);
	sift_cbatch_reset(copy, c->sb);
	sift_cbatch_reset(odd, c->sb);
	sift_cbatch_reset(even, c->sb);
	sift_cbatch_reset(merged, c->sb);
	for (uint32 i = 0; i < n; i++)
	{
		any |= xm[i] != 0;
		sift_cbatch_add_ver(b, &rows[i], xm[i], cm[i]);
		sift_cbatch_add_ver(i % 2 ? odd : even, &rows[i], xm[i], cm[i]);
	}
	CHECK(t, b->hasver == any, "the batch says versions %d for rows with versions %d", b->hasver, any);
	for (uint32 r = 0; r < n; r++)
		sift_cbatch_add_from(copy, b, r);
	src[0] = odd;
	src[1] = even;
	sift_cbatch_merge(merged, src, 2);
	for (uint32 r = 0; r < n && any; r++)
		CHECK(t, b->xmin[r] == xm[r] && b->cmin[r] == cm[r] && copy->xmin[r] == xm[r] && copy->cmin[r] == cm[r] && merged->xmin[r] == xm[r] &&
			  merged->cmin[r] == cm[r], "row %u lost its version (%u, %u) in a batch, a copy or a merge", r, xm[r], cm[r]);
	sift_cbatch_reset(b, c->sb);
	CHECK(t, !b->hasver, "reset leaves versions");
}

/* Rows out of order, outside the superblock or with a bad offset are refused; a string over the dictionary limit is not */
void
check_errors(TCtx *t, const Case *c, const SiftColRow *rows, uint32 n)
{
	SiftColBatch *b = batch_of(c, rows, n);
	SiftColRow	row = rows[n - 1];
	SiftKey		last = sift_key_from_tid((ItemPointer) &rows[n - 1].tid);
	int			str = -1;

	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a repeated TID was accepted");
	row = rows[0];
	CHECK(t, n < 2 || ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a TID out of order was accepted");
	ItemPointerSet(&row.tid, (c->sb + 1) * SIFT_SB_PAGES, 1);
	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "a TID of the next superblock was accepted");
	ItemPointerSet(&row.tid, sift_key_block(last), 1);
	row.tid.ip_posid = 0;
	CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == ERRCODE_INTERNAL_ERROR, "offset 0 was accepted");
	CHECK(t, b->nrows == n, "refused rows were counted");

	for (int col = 0; col < c->schema->ncols && str < 0; col++)
		if (c->schema->cols[col].store == SIFT_COLS_STR && c->schema->cols[col].parent < 0)
			str = col;
	if (str >= 0 && (last & 0xFFFF) < MaxHeapTuplesPerPage)
	{
		char		big[SIFT_COL_DICT_MAX_ENTRY + 1];

		memset(big, 'x', sizeof(big));
		row = rows[n - 1];
		row.val = palloc(c->schema->ncols * sizeof(SiftColValue));
		memcpy(row.val, rows[n - 1].val, c->schema->ncols * sizeof(SiftColValue));
		row.val[str] = (SiftColValue) {.v.s = big, .len = sizeof(big)};
		ItemPointerSet(&row.tid, sift_key_block(last), (last & 0xFFFF) + 1);
		CHECK(t, ERRCODE_OF(sift_cbatch_add(b, &row)) == 0, "a value of 256 bytes was refused");
	}
}

#endif							/* SIFT_TESTING */
