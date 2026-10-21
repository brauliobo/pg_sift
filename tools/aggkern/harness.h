/* Shared by the standalone aggregation operator test and benchmark (no backend linked): tools/kernels' harness and the exact-size allocator of the case runner. */
#ifndef AK_HARNESS_H
#define AK_HARNESS_H

#include "../kernels/harness.h"
#include "agg/kern_int.h"

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

#endif
