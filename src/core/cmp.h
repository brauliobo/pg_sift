/*
 * core/cmp.h - three-way comparators shared by the sources: of scalars for qsort and bsearch (ascending, no overflow), and of
 * two terms as bytes then length, the order of the dictionary. Portable (C library only).
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

#endif							/* SIFT_CMP_H */
