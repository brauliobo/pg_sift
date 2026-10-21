/*
 * core/cmp.h - ordering helpers shared by the sources: three-way comparators of scalars for qsort and bsearch (ascending, no overflow),
 * of two terms as bytes then length (the order of the dictionary), and the bisection of a sorted range. Portable (C library only).
 */
#ifndef SIFT_CMP_H
#define SIFT_CMP_H

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

#endif							/* SIFT_CMP_H */
