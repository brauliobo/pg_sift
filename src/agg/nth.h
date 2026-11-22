/*
 * agg/nth.h - the k best of n elements without sorting all of them, and the sort of those k: introselect and introsort (a ninther pivot, a 3-way partition, so that a run of equal keys
 * costs one pass, and a heapsort of the range left when the partitions went wrong for 2 log2 n rounds). The elements are n records of sz bytes at base, put in order by
 * cmp(a, b, arg) < 0. It has no dependency but libc, so the code of the Vulkan engine can use it as the code of the backend does.
 *
 *   sift_nth(base, n, sz, k, cmp, arg);   the element at k is the one a sort would put there, those before it are not after it, and those after it are not before it
 *   sift_nth_sort(base, n, sz, cmp, arg); the elements in order
 */
#ifndef SIFT_AGG_NTH_H
#define SIFT_AGG_NTH_H

#include <stddef.h>
#include <string.h>

typedef int (*SiftNthCmp) (const void *a, const void *b, void *arg);

#define SIFT_NTH_INSERTION 16
#define SIFT_NTH_NINTHER 128

#define NTH_AT(i) (base + (size_t) (i) * sz)

static inline void
sift_nth_swap(char *a, char *b, size_t sz)
{
	char		t[32];

	for (size_t m; sz > 0; a += m, b += m, sz -= m)
	{
		m = sz < sizeof(t) ? sz : sizeof(t);
		memcpy(t, a, m);
		memcpy(a, b, m);
		memcpy(b, t, m);
	}
}

/* the index of the median of the elements a, b, c */
static inline size_t
sift_nth_med3(const char *base, size_t sz, size_t a, size_t b, size_t c, SiftNthCmp cmp, void *arg)
{
	if (cmp(NTH_AT(a), NTH_AT(b), arg) < 0)
		return cmp(NTH_AT(b), NTH_AT(c), arg) < 0 ? b : cmp(NTH_AT(a), NTH_AT(c), arg) < 0 ? c : a;
	return cmp(NTH_AT(b), NTH_AT(c), arg) > 0 ? b : cmp(NTH_AT(a), NTH_AT(c), arg) > 0 ? c : a;
}

static inline void
sift_nth_sift_down(char *base, size_t n, size_t sz, size_t at, SiftNthCmp cmp, void *arg)
{
	for (;;)
	{
		size_t		l = 2 * at + 1,
					w = l;

		if (l >= n)
			return;
		if (l + 1 < n && cmp(NTH_AT(l + 1), NTH_AT(l), arg) > 0)
			w = l + 1;
		if (cmp(NTH_AT(w), NTH_AT(at), arg) <= 0)
			return;
		sift_nth_swap(NTH_AT(w), NTH_AT(at), sz);
		at = w;
	}
}

static inline void
sift_nth_heapsort(char *base, size_t n, size_t sz, SiftNthCmp cmp, void *arg)
{
	for (size_t i = n / 2; i-- > 0;)
		sift_nth_sift_down(base, n, sz, i, cmp, arg);
	for (size_t end = n; end-- > 1;)
	{
		sift_nth_swap(NTH_AT(0), NTH_AT(end), sz);
		sift_nth_sift_down(base, end, sz, 0, cmp, arg);
	}
}

static inline void
sift_nth_insertion(char *base, size_t n, size_t sz, SiftNthCmp cmp, void *arg)
{
	for (size_t i = 1; i < n; i++)
		for (size_t j = i; j > 0 && cmp(NTH_AT(j - 1), NTH_AT(j), arg) > 0; j--)
			sift_nth_swap(NTH_AT(j - 1), NTH_AT(j), sz);
}

/* The elements [lo, hi) as [lo, *lt) before the pivot, [*lt, *gt] equal to it, (*gt, hi) after it */
static inline void
sift_nth_partition(char *base, size_t lo, size_t hi, size_t sz, size_t *lt_out, size_t *gt_out, SiftNthCmp cmp, void *arg)
{
	const size_t n = hi - lo,
				mid = lo + n / 2,
				last = hi - 1,
				s = n / 8;
	size_t		lt = lo,
				i = lo + 1,
				gt = last;
	size_t		pivot = n < SIFT_NTH_NINTHER ? sift_nth_med3(base, sz, lo, mid, last, cmp, arg)
		: sift_nth_med3(base, sz, sift_nth_med3(base, sz, lo, lo + s, lo + 2 * s, cmp, arg),
						sift_nth_med3(base, sz, mid - s, mid, mid + s, cmp, arg),
						sift_nth_med3(base, sz, last - 2 * s, last - s, last, cmp, arg), cmp, arg);

	sift_nth_swap(NTH_AT(lo), NTH_AT(pivot), sz);
	while (i <= gt)
	{
		const int	c = cmp(NTH_AT(i), NTH_AT(lt), arg);

		if (c < 0)
			sift_nth_swap(NTH_AT(lt++), NTH_AT(i++), sz);
		else if (c > 0)
			sift_nth_swap(NTH_AT(i), NTH_AT(gt--), sz);
		else
			i++;
	}
	*lt_out = lt;
	*gt_out = gt;
}

static inline int
sift_nth_depth(size_t n)
{
	int			depth = 0;

	for (; n > 1; n >>= 1)
		depth += 2;
	return depth;
}

static inline void
sift_nth(void *vbase, size_t n, size_t sz, size_t k, SiftNthCmp cmp, void *arg)
{
	char	   *base = vbase;
	size_t		lo = 0,
				hi = n;
	int			depth = sift_nth_depth(n);

	while (hi - lo > SIFT_NTH_INSERTION)
	{
		size_t		lt,
					gt;

		if (depth-- == 0)
		{
			sift_nth_heapsort(NTH_AT(lo), hi - lo, sz, cmp, arg);
			return;
		}
		sift_nth_partition(base, lo, hi, sz, &lt, &gt, cmp, arg);
		if (k < lt)
			hi = lt;
		else if (k > gt)
			lo = gt + 1;
		else
			return;
	}
	sift_nth_insertion(NTH_AT(lo), hi - lo, sz, cmp, arg);
}

static inline void
sift_nth_sort_range(char *base, size_t lo, size_t hi, size_t sz, int depth, SiftNthCmp cmp, void *arg)
{
	while (hi - lo > SIFT_NTH_INSERTION)
	{
		size_t		lt,
					gt;

		if (depth-- == 0)
		{
			sift_nth_heapsort(NTH_AT(lo), hi - lo, sz, cmp, arg);
			return;
		}
		sift_nth_partition(base, lo, hi, sz, &lt, &gt, cmp, arg);
		if (lt - lo < hi - gt - 1)
		{
			sift_nth_sort_range(base, lo, lt, sz, depth, cmp, arg);
			lo = gt + 1;
		}
		else
		{
			sift_nth_sort_range(base, gt + 1, hi, sz, depth, cmp, arg);
			hi = lt;
		}
	}
	sift_nth_insertion(NTH_AT(lo), hi - lo, sz, cmp, arg);
}

static inline void
sift_nth_sort(void *base, size_t n, size_t sz, SiftNthCmp cmp, void *arg)
{
	sift_nth_sort_range(base, 0, n, sz, sift_nth_depth(n), cmp, arg);
}

#undef NTH_AT

#endif							/* SIFT_AGG_NTH_H */
