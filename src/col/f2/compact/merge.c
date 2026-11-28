/*
 * col/f2/compact/merge.c - the order in which a compaction reads the granules of its inputs (merge.h).
 */
#include "core/host.h"

#include "col/f2/compact/merge.h"

SiftCompactMerge *
sift_compact_merge_begin(const SiftPartDesc *const *at, int n)
{
	SiftCompactMerge *m = palloc(sizeof(SiftCompactMerge));

	m->n = n;
	m->bound = palloc(Max(n, 1) * SIFT_BOUND_BYTES);
	m->done = palloc0(Max(n, 1) * sizeof(bool));
	for (int i = 0; i < n; i++)
		memcpy(m->bound[i], at[i]->key_lo, SIFT_BOUND_BYTES);
	return m;
}

int
sift_compact_merge_next(const SiftCompactMerge *m)
{
	int			least = -1;

	for (int i = 0; i < m->n; i++)
		if (!m->done[i] && (least < 0 || memcmp(m->bound[i], m->bound[least], SIFT_BOUND_BYTES) < 0))
			least = i;
	return least;
}

void
sift_compact_merge_read(SiftCompactMerge *m, int i, const SiftRowBuf *rb)
{
	for (int r = 0; r < rb->n; r++)
		if (memcmp(rb->keys[r], m->bound[i], SIFT_BOUND_BYTES) > 0)
			memcpy(m->bound[i], rb->keys[r], SIFT_BOUND_BYTES);
}

static int
by_key_then_place(const void *a, const void *b, void *arg)
{
	const SiftPartDesc *const *at = arg;
	const int	x = *(const int *) a,
				y = *(const int *) b,
				c = memcmp(at[x]->key_lo, at[y]->key_lo, SIFT_BOUND_BYTES);

	return c != 0 ? c : (x > y) - (x < y);
}

int *
sift_compact_key_order(const SiftPartDesc *const *at, int n)
{
	int		   *order = palloc(Max(n, 1) * sizeof(int));

	for (int i = 0; i < n; i++)
		order[i] = i;
	qsort_arg(order, n, sizeof(int), by_key_then_place, (void *) at);
	return order;
}
