/*
 * col/f2/cube/heap.c - the heaps of a summary build. A binary max-heap over the ordinals of keys, ordered by a size the owner keeps in an array: the
 * key that holds the most open bytes is the top, found in O(1), and a key whose size changed moves in O(log n); it replaces a scan of every key at
 * each eviction. And the min-heap of the newest rows of one ring of a key (the ring: topk.c), whose array grows with the rows the key has.
 */
/* check-memory: allow-allocs the arrays follow the keys of the pair map the build holds by design (8 bytes a key) */
#include "core/host.h"

#include "col/f2/cube/int.h"

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

int
sift_newest_cmp(const SiftScopeNewest *a, const SiftScopeNewest *b)
{
	if (a->time != b->time)
		return a->time < b->time ? -1 : 1;
	if (a->gidx != b->gidx)
		return a->gidx < b->gidx ? -1 : 1;
	return a->row_ord < b->row_ord ? -1 : a->row_ord > b->row_ord;
}

static inline uint32
depth_of(const SiftTopKHeap *h)
{
	return h->depth != 0 ? h->depth : SIFT_SCOPE_TOPK_CAP;
}

bool
sift_topk_truncated(const SiftTopKHeap *h)
{
	return h->dropped && depth_of(h) < SIFT_SCOPE_TOPK_CAP;
}

bool
sift_topk_wants(SiftTopKHeap *h, int64 time)
{
	const bool	wants = h->n < depth_of(h) || time >= h->e[0].time;

	h->dropped |= !wants;
	return wants;
}

static void
newest_swap(SiftScopeNewest *a, SiftScopeNewest *b)
{
	const SiftScopeNewest t = *a;

	*a = *b;
	*b = t;
}

static void
heap_sift_down(SiftTopKHeap *h, uint32 i)
{
	for (;;)
	{
		uint32		l = 2 * i + 1,
					r = l + 1,
					m = i;

		if (l < h->n && sift_newest_cmp(&h->e[l], &h->e[m]) < 0)
			m = l;
		if (r < h->n && sift_newest_cmp(&h->e[r], &h->e[m]) < 0)
			m = r;
		if (m == i)
			return;
		newest_swap(&h->e[i], &h->e[m]);
		i = m;
	}
}

void
sift_topk_offer(SiftTopKHeap *h, int64 time, uint32 gidx, uint32 row_ord)
{
	const SiftScopeNewest c = {time, gidx, row_ord};

	if (h->n < depth_of(h))
	{
		uint32		i = h->n++;

		if (i == h->cap)
		{
			/* MEM[MEM-SCOPE-09]: key ring heap array grown with rows (4, 16, depth * 16 B; was 1,040 B/ring) */
			h->cap = Min(Max(h->cap * 4, 4), depth_of(h));
			h->e = h->e == NULL ? palloc(h->cap * sizeof(*h->e)) : repalloc(h->e, h->cap * sizeof(*h->e));
		}
		h->e[i] = c;
		for (; i > 0 && sift_newest_cmp(&h->e[i], &h->e[(i - 1) / 2]) < 0; i = (i - 1) / 2)
			newest_swap(&h->e[i], &h->e[(i - 1) / 2]);
	}
	else
	{
		h->dropped = true;
		if (sift_newest_cmp(&c, &h->e[0]) > 0)
		{
			h->e[0] = c;
			heap_sift_down(h, 0);
		}
	}
}

void
sift_topk_release(SiftTopKHeap *h)
{
	if (h->e != NULL)
		pfree(h->e);
	h->e = NULL;
	h->n = h->cap = 0;
}

static int
newest_desc(const void *a, const void *b)
{
	return -sift_newest_cmp(a, b);
}

static inline uint32
loc_pack(uint32 gidx, uint32 row_ord)
{
	return (gidx << SIFT_GRANULE_ROW_BITS) | (row_ord & SIFT_GRANULE_ROW_MASK);
}

uint32
sift_topk_write(const SiftTopKHeap *h, bool wide, StringInfo out)
{
	SiftScopeNewest s[SIFT_SCOPE_TOPK_CAP];

	for (uint32 i = 0; i < h->n; i++)
		s[i] = h->e[i];
	qsort(s, h->n, sizeof(SiftScopeNewest), newest_desc);
	for (uint32 i = 0; i < h->n; i++)
	{
		const int64 time = i == 0 ? s[0].time : (int64) ((uint64) s[i - 1].time - (uint64) s[i].time);

		SIFT_INVARIANT(wide || s[i].gidx < SIFT_TOPK_COMPACT_GRANS, "scope ring: granule %u does not fit a compact entry", s[i].gidx);
		/* STOR[STOR-SCOPE-07]: ring entry wire, fixed 12 B compact or 16 B wide (varint deferred 6.1+) */
		if (wide)
		{
			SiftScopeTopKEntryWide e = {time, s[i].gidx, (uint16) s[i].row_ord, 0};

			appendBinaryStringInfo(out, (char *) &e, sizeof(e));
		}
		else
		{
			SiftScopeTopKEntry e = {time, loc_pack(s[i].gidx, s[i].row_ord)};

			appendBinaryStringInfo(out, (char *) &e, sizeof(e));
		}
	}
	return h->n;
}
