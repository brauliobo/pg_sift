/*
 * core/cmp.h - ordering helpers shared by the sources: three-way comparators of scalars for qsort and bsearch (ascending, no overflow),
 * of two terms as bytes then length (the order of the dictionary), the bisection of a sorted range and an in-line sort of an array. Portable (C library only).
 */
#ifndef SIFT_CMP_H
#define SIFT_CMP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* defines the qsort/bsearch comparator NAME of an array of TYPE */
#define SIFT_DEFINE_CMP(name, type) \
static inline int \
name(const void *a, const void *b) \
{ \
	type		x = *(const type *) a, \
				y = *(const type *) b; \
\
	return (x > y) - (x < y); \
}

/* -1, 0 or 1: the order of two integers or enums, as fields of a comparator (not for floats: a NaN is neither above nor below) */
#define SIFT_CMP3(a, b) (((a) > (b)) - ((a) < (b)))

SIFT_DEFINE_CMP(sift_cmp_i16, int16_t)
SIFT_DEFINE_CMP(sift_cmp_i32, int32_t)
SIFT_DEFINE_CMP(sift_cmp_i64, int64_t)
SIFT_DEFINE_CMP(sift_cmp_u64, uint64_t)
SIFT_DEFINE_CMP(sift_cmp_u32, uint32_t)
SIFT_DEFINE_CMP(sift_cmp_f64, double)

static inline int
sift_term_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, alen < blen ? alen : blen);

	return c ? c : (alen > blen) - (alen < blen);
}

/*
 * Bisects [lo, hi) to the first index at which `below` is false, a condition that holds for a prefix of the range and reads the
 * candidate through m (declared here as a type); lo and hi are variables, equal on exit.
 */
#define SIFT_BISECT(type, m, lo, hi, below) \
	do { \
		while ((lo) < (hi)) \
		{ \
			type		m = (lo) + ((hi) - (lo)) / 2; \
\
			if (below) \
				(lo) = m + 1; \
			else \
				(hi) = m; \
		} \
	} while (0)

/* does the sorted set hold a member in [lo, hi]? */
static inline bool
sift_set_meets_i64(const int64_t *set, uint32_t n, int64_t lo, int64_t hi)
{
	uint32_t	a = 0,
				b = n;

	SIFT_BISECT(uint32_t, m, a, b, set[m] < lo);
	return a < n && set[a] <= hi;
}

/*
 * defines the static function NAME(T *a, size_t n) that sorts an array in place with LESS(x, y) (pointers: does *x sort before *y?) compared in line, where qsort
 * calls a function for each comparison. Introsort: quicksort on the median of three, heapsort when it recurses too deep, insertion sort under 16 elements. Not stable;
 * the callers order by a key that no two elements share.
 */
#define SIFT_DEFINE_SORT(NAME, T, LESS) \
static void \
NAME##_sift(T *a, size_t i, size_t n) \
{ \
	T			x = a[i]; \
	size_t		c; \
\
	while ((c = 2 * i + 1) < n) \
	{ \
		if (c + 1 < n && LESS(&a[c], &a[c + 1])) \
			c++; \
		if (!LESS(&x, &a[c])) \
			break; \
		a[i] = a[c]; \
		i = c; \
	} \
	a[i] = x; \
} \
\
static void \
NAME##_rec(T *a, size_t n, int depth) \
{ \
	while (n > 16) \
	{ \
		T			p; \
		size_t		i = 0, \
					j = n - 1, \
					m = n / 2; \
\
		if (depth-- == 0) \
		{ \
			for (size_t k = n / 2; k-- > 0;) \
				NAME##_sift(a, k, n); \
			for (size_t k = n; k-- > 1;) \
			{ \
				T			t = a[0]; \
\
				a[0] = a[k]; \
				a[k] = t; \
				NAME##_sift(a, 0, k); \
			} \
			return; \
		} \
		p = LESS(&a[0], &a[m]) ? (LESS(&a[m], &a[j]) ? a[m] : (LESS(&a[0], &a[j]) ? a[j] : a[0])) : (LESS(&a[0], &a[j]) ? a[0] : (LESS(&a[m], &a[j]) ? a[j] : a[m])); \
		while (i <= j) \
		{ \
			while (LESS(&a[i], &p)) \
				i++; \
			while (LESS(&p, &a[j])) \
				j--; \
			if (i <= j) \
			{ \
				T			t = a[i]; \
\
				a[i++] = a[j]; \
				a[j] = t; \
				if (j-- == 0) \
					break; \
			} \
		} \
		if (j + 1 < n - i) \
		{ \
			NAME##_rec(a, j + 1, depth); \
			a += i; \
			n -= i; \
		} \
		else \
		{ \
			NAME##_rec(a + i, n - i, depth); \
			n = j + 1; \
		} \
	} \
	for (size_t k = 1; k < n; k++) \
	{ \
		T			x = a[k]; \
		size_t		h = k; \
\
		for (; h > 0 && LESS(&x, &a[h - 1]); h--) \
			a[h] = a[h - 1]; \
		a[h] = x; \
	} \
} \
\
static void \
NAME(T *a, size_t n) \
{ \
	int			depth = 0; \
\
	for (size_t m = n; m > 1; m >>= 1) \
		depth += 2; \
	NAME##_rec(a, n, depth); \
}

#endif							/* SIFT_CMP_H */
