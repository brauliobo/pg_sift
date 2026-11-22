/*
 * col/scope_v6/heap.c - a binary max-heap over the ordinals of keys, ordered by a size the owner keeps in an array: the key that holds the most
 * open bytes is the top, found in O(1), and a key whose size changed moves in O(log n). It replaces a scan of every key at each eviction.
 */
/* check-memory: allow-allocs the arrays follow the keys of the pair map the build holds by design (8 bytes a key) */
#include "core/host.h"

#include "col/scope_v6/int.h"

#define NOT_IN_HEAP UINT32_MAX

void
sift_mheap_init(SiftMaxHeap *h, uint32 nkeys, const Size *size)
{
	h->n = 0;
	h->cmps = 0;
	h->size = size;
	h->ord = palloc(sizeof(uint32) * Max(nkeys, 1));
	h->pos = palloc(sizeof(uint32) * Max(nkeys, 1));
	memset(h->pos, 0xFF, sizeof(uint32) * Max(nkeys, 1));
}

void
sift_mheap_free(SiftMaxHeap *h)
{
	pfree(h->ord);
	pfree(h->pos);
}

static inline bool
bigger(SiftMaxHeap *h, uint32 i, uint32 j)
{
	h->cmps++;
	return h->size[h->ord[i]] > h->size[h->ord[j]];
}

static void
swap(SiftMaxHeap *h, uint32 i, uint32 j)
{
	uint32		t = h->ord[i];

	h->ord[i] = h->ord[j];
	h->ord[j] = t;
	h->pos[h->ord[i]] = i;
	h->pos[h->ord[j]] = j;
}

static void
sift_up(SiftMaxHeap *h, uint32 i)
{
	while (i > 0 && bigger(h, i, (i - 1) / 2))
	{
		swap(h, i, (i - 1) / 2);
		i = (i - 1) / 2;
	}
}

static void
sift_down(SiftMaxHeap *h, uint32 i)
{
	for (;;)
	{
		uint32		l = 2 * i + 1,
					r = l + 1,
					m = i;

		if (l < h->n && bigger(h, l, m))
			m = l;
		if (r < h->n && bigger(h, r, m))
			m = r;
		if (m == i)
			return;
		swap(h, i, m);
		i = m;
	}
}

void
sift_mheap_set(SiftMaxHeap *h, uint32 k)
{
	if (h->pos[k] == NOT_IN_HEAP)
	{
		h->ord[h->n] = k;
		h->pos[k] = h->n++;
	}
	sift_up(h, h->pos[k]);
	sift_down(h, h->pos[k]);
}

void
sift_mheap_remove(SiftMaxHeap *h, uint32 k)
{
	uint32		i = h->pos[k];

	if (i == NOT_IN_HEAP)
		return;
	swap(h, i, h->n - 1);
	h->n--;
	h->pos[k] = NOT_IN_HEAP;
	if (i < h->n)
		sift_mheap_set(h, h->ord[i]);
}

uint32
sift_mheap_top(const SiftMaxHeap *h)
{
	return h->n == 0 ? UINT32_MAX : h->ord[0];
}
