/*
 * col/f2/rowbuf_sort.c - the order of the rows of a part (col/f2/rowbuf.h, sift_rowbuf_sort): the sort columns in turn, then the address.
 */
#include "core/host.h"

#include "col/f2/rowbuf.h"

typedef struct SortCtx
{
	const SiftRowBuf *rb;
	const uint16 *cols;
	const uint8 *flags;
	int			ncols;
} SortCtx;

int
sift_rowbuf_cmp_val(const SiftRowBuf *rb, int c, int r1, int r2)
{
	if (rb->kinds[c] == SIFT_CELL_INT)
		return rb->i[c][r1] < rb->i[c][r2] ? -1 : rb->i[c][r1] > rb->i[c][r2];
	if (rb->kinds[c] == SIFT_CELL_F8)
		return rb->f[c][r1] < rb->f[c][r2] ? -1 : rb->f[c][r1] > rb->f[c][r2];
	{
		int			m = memcmp(rb->s[c][r1], rb->s[c][r2], Min(rb->slen[c][r1], rb->slen[c][r2]));

		return m != 0 ? (m < 0 ? -1 : 1) : (rb->slen[c][r1] < rb->slen[c][r2] ? -1 : rb->slen[c][r1] > rb->slen[c][r2]);
	}
}

static int
row_cmp(const void *a, const void *b, void *arg)
{
	const SortCtx *x = arg;
	const SiftRowBuf *rb = x->rb;
	int			r1 = *(const int *) a,
				r2 = *(const int *) b;

	for (int k = 0; k < x->ncols; k++)
	{
		int			c = x->cols[k];
		bool		n1 = rb->isnull[c][r1],
					n2 = rb->isnull[c][r2];
		int			cmp = 0;

		if (n1 || n2)
			cmp = n1 == n2 ? 0 : ((n1 ? 1 : -1) * ((x->flags[k] & SIFT_KEY_NULLS_FIRST) ? -1 : 1));
		else
			cmp = sift_rowbuf_cmp_val(rb, c, r1, r2) * ((x->flags[k] & SIFT_KEY_DESC) ? -1 : 1);
		if (cmp != 0)
			return cmp;
	}
	return rb->tid[r1] < rb->tid[r2] ? -1 : rb->tid[r1] > rb->tid[r2];
}

/* Rows that arrive in the order of a part (a load in key order, rows keyed by their address) need no sort and no permutation */
static bool
rows_ordered(const SiftRowBuf *rb, const SortCtx *ctx)
{
	for (int r = 1; r < rb->n; r++)
	{
		int			prev = r - 1;

		if (row_cmp(&prev, &r, (void *) ctx) > 0)
			return false;
	}
	return true;
}

/* A row with the order image of its first sort column when that is an integer: rows are compared by it first and by every sort column (row_cmp) only when it ties */
typedef struct KeyedRow
{
	int64		key;
	int			row;
} KeyedRow;

static int
keyed_cmp(const KeyedRow *a, const KeyedRow *b, const SortCtx *ctx)
{
	if (a->key != b->key)
		return a->key < b->key ? -1 : 1;
	return row_cmp(&a->row, &b->row, (void *) ctx);
}

#define ST_SORT sort_keyed
#define ST_ELEMENT_TYPE KeyedRow
#define ST_COMPARE_ARG_TYPE SortCtx
#define ST_COMPARE(a, b, ctx) keyed_cmp((a), (b), (ctx))
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

static int64
first_key_image(const SortCtx *ctx, int r)
{
	const SiftRowBuf *rb = ctx->rb;
	int			c = ctx->cols[0];

	if (rb->isnull[c][r])
		return (ctx->flags[0] & SIFT_KEY_NULLS_FIRST) ? PG_INT64_MIN : PG_INT64_MAX;
	return (ctx->flags[0] & SIFT_KEY_DESC) ? ~rb->i[c][r] : rb->i[c][r];
}

/* The rows in the order of a part as the permutation that gets them there */
static int *
sort_permutation(const SiftRowBuf *rb, const SortCtx *ctx)
{
	int		   *perm = palloc(Max(rb->n, 1) * sizeof(int));

	if (ctx->ncols > 0 && rb->kinds[ctx->cols[0]] == SIFT_CELL_INT)
	{
		KeyedRow   *keyed = palloc(Max(rb->n, 1) * sizeof(KeyedRow));

		for (int r = 0; r < rb->n; r++)
			keyed[r] = (KeyedRow) {first_key_image(ctx, r), r};
		sort_keyed(keyed, rb->n, (SortCtx *) ctx);
		for (int r = 0; r < rb->n; r++)
			perm[r] = keyed[r].row;
		pfree(keyed);
		return perm;
	}
	for (int r = 0; r < rb->n; r++)
		perm[r] = r;
	qsort_arg(perm, rb->n, sizeof(int), row_cmp, (void *) ctx);
	return perm;
}

void
sift_rowbuf_sort(SiftRowBuf *rb, const SiftPartSpec *spec)
{
	SortCtx		ctx = {rb, spec->sort_cols, spec->sort_flags, spec->nsort};
	int		   *perm;

	if (rows_ordered(rb, &ctx))
		return;
	perm = sort_permutation(rb, &ctx);
	sift_rowbuf_permute(rb, perm);
	pfree(perm);
}
