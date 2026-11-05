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
static inline void \
NAME(T *a, size_t n) \
{ \
	int			depth = 0; \
\
	for (size_t m = n; m > 1; m >>= 1) \
		depth += 2; \
	NAME##_rec(a, n, depth); \
}


/*
 * defines the static function NAME(T *a, size_t n) that orders an array by the signed 64-bit KEY(x), then KEY2(x) (x: a pointer; 0 when there is one key) and, among
 * equal keys, by TIE(x) (an unsigned integer): the order SIFT_DEFINE_SORT gives with the same fields, for an array whose TIE never goes down along it (entries added
 * in the order of their rows). A least significant digit radix sort, stable, of the bits each key spans above its smallest value, in as few digits of equal width as
 * the width allows (8 bits under 4,096 entries, 10 under 32,768, 12 above: a column of a few thousand values takes one or two passes of one load and one store an
 * entry, where the comparison sort takes 20 steps of two fields). A digit all entries share is not moved. The other arrays, those under 512 entries and those that
 * ALLOC (the bytes of a copy and of the counters of a digit; NULL: none) does not give go to FALLBACK(a, n). Same result as the comparison sort whatever the path:
 * two entries equal in all three fields are alike.
 */
#define SIFT_RADIX_MAX_DIGIT 12

#define SIFT_DEFINE_RADIX_SORT(NAME, T, KEY, KEY2, TIE, ALLOC, FREE, FALLBACK) \
static void \
NAME(T *a, size_t n) \
{ \
	int64_t		mn[2], \
				mx[2]; \
	int			bits[2], \
				dw[2], \
				npass[2], \
				maxd; \
	T		   *tmp, \
			   *src = a, \
			   *dst; \
	uint32_t   *cnt; \
\
	if (n < 512 || n > UINT32_MAX) \
	{ \
		FALLBACK(a, n); \
		return; \
	} \
	mn[0] = mx[0] = KEY(&a[0]); \
	mn[1] = mx[1] = KEY2(&a[0]); \
	for (size_t i = 1; i < n; i++) \
	{ \
		int64_t		k0 = KEY(&a[i]), \
					k1 = KEY2(&a[i]); \
\
		if (TIE(&a[i]) < TIE(&a[i - 1])) \
		{ \
			FALLBACK(a, n); \
			return; \
		} \
		mn[0] = k0 < mn[0] ? k0 : mn[0]; \
		mx[0] = k0 > mx[0] ? k0 : mx[0]; \
		mn[1] = k1 < mn[1] ? k1 : mn[1]; \
		mx[1] = k1 > mx[1] ? k1 : mx[1]; \
	} \
	if (mn[0] == mx[0] && mn[1] == mx[1]) \
		return; \
	maxd = n >= 32768 ? SIFT_RADIX_MAX_DIGIT : n >= 4096 ? 10 : 8; \
	for (int c = 0; c < 2; c++) \
	{ \
		uint64_t	range = (uint64_t) mx[c] - (uint64_t) mn[c]; \
\
		for (bits[c] = 0; bits[c] < 64 && (range >> bits[c]) != 0; bits[c]++) \
			; \
		npass[c] = (bits[c] + maxd - 1) / maxd; \
		dw[c] = npass[c] == 0 ? 0 : (bits[c] + npass[c] - 1) / npass[c]; \
	} \
	if ((tmp = (T *) ALLOC(n * sizeof(T) + ((size_t) 1 << maxd) * sizeof(uint32_t))) == NULL) \
	{ \
		FALLBACK(a, n); \
		return; \
	} \
	cnt = (uint32_t *) (tmp + n); \
	dst = tmp; \
	for (int c = 1; c >= 0; c--) \
	{ \
		for (int k = 0, shift = 0; k < npass[c]; k++, shift += dw[c]) \
		{ \
			uint64_t	mask = ((uint64_t) 1 << dw[c]) - 1, \
						base = (uint64_t) mn[c]; \
			size_t		sum = 0; \
			bool		shared = false; \
			T		   *t; \
\
			memset(cnt, 0, ((size_t) 1 << dw[c]) * sizeof(uint32_t)); \
			if (c == 0) \
				for (size_t i = 0; i < n; i++) \
					cnt[(((uint64_t) KEY(&src[i]) - base) >> shift) & mask]++; \
			else \
				for (size_t i = 0; i < n; i++) \
					cnt[(((uint64_t) KEY2(&src[i]) - base) >> shift) & mask]++; \
			for (size_t b = 0; b <= mask; b++) \
			{ \
				size_t		x = cnt[b]; \
\
				shared |= x == n; \
				cnt[b] = sum; \
				sum += x; \
			} \
			if (shared) \
				continue; \
			if (c == 0) \
				for (size_t i = 0; i < n; i++) \
					dst[cnt[(((uint64_t) KEY(&src[i]) - base) >> shift) & mask]++] = src[i]; \
			else \
				for (size_t i = 0; i < n; i++) \
					dst[cnt[(((uint64_t) KEY2(&src[i]) - base) >> shift) & mask]++] = src[i]; \
			t = src; \
			src = dst; \
			dst = t; \
		} \
	} \
	if (src != a) \
		memcpy(a, src, n * sizeof(T)); \
	FREE(tmp); \
}

/* sift_sort_i16, _i32, _i64, _u32 and _u64 (a, n): sort an array of that scalar ascending in line, where qsort(a, n, sizeof(T), sift_cmp_*) calls a function for each comparison; a is void * as qsort's is */
#define SIFT_DEFINE_SORT_SCALAR(name, T) \
SIFT_DEFINE_SORT(name##_typed, T, SIFT_SORT_LESS_SCALAR) \
static inline void \
name(void *a, size_t n) \
{ \
	name##_typed((T *) a, n); \
}

#define SIFT_SORT_LESS_SCALAR(x, y) (*(x) < *(y))
SIFT_DEFINE_SORT_SCALAR(sift_sort_i16, int16_t)
SIFT_DEFINE_SORT_SCALAR(sift_sort_i32, int32_t)
SIFT_DEFINE_SORT_SCALAR(sift_sort_i64, int64_t)
SIFT_DEFINE_SORT_SCALAR(sift_sort_u32, uint32_t)
SIFT_DEFINE_SORT_SCALAR(sift_sort_u64, uint64_t)

#endif							/* SIFT_CMP_H */
