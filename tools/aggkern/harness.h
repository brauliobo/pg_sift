/* Shared by the standalone aggregation operator test and benchmark (no backend linked). */
#ifndef AK_HARNESS_H
#define AK_HARNESS_H

#include "postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sift_aggkern_int.h"

/* libc, not libpgport */
#undef printf
#undef fprintf
#undef qsort

/* Exactly n bytes, so that a sanitizer sees an access past them */
static inline void *
ak_alloc(size_t n)
{
	void	   *p;

	if (posix_memalign(&p, SIFT_SIMD_ALIGN, n ? n : 1))
		abort();
	return p;
}

#define AGK_ALLOC(n)	ak_alloc(n)
#define AGK_FREE(p)		free(p)

extern void ak_cpu_init(void);
extern const char *const ak_level_name[];

#endif
