/*
 * core/cmp.h - three-way comparators shared by the sources: of int64 and double for qsort and bsearch (ascending, no overflow), and of
 * two terms as bytes then length, the order of the dictionary. Portable (C library only).
 */
#ifndef SIFT_CMP_H
#define SIFT_CMP_H

#include <stdint.h>
#include <string.h>

static inline int
sift_cmp_i64(const void *a, const void *b)
{
	int64_t		x = *(const int64_t *) a,
				y = *(const int64_t *) b;

	return (x > y) - (x < y);
}

static inline int
sift_cmp_f64(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

static inline int
sift_term_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, alen < blen ? alen : blen);

	return c ? c : (alen > blen) - (alen < blen);
}

#endif							/* SIFT_CMP_H */
