/*
 * col/granule/cfilt_cc.h - the predicates of a scan run by the kernels of col/granule/cfilt_kern.h on the nodes of a column chunk of the codec (col/codec.h) as they are stored: the frame of reference
 * (FORBIT in the vertical layout, FORBYTE as an array), the delta, the runs, the dictionary with its codes and the constant, under any wrapper. No value is built: a range or a set of the keys
 * comes out as a bit for each value of the chunk.
 *
 * A node the kernels do not read (the decimal floats of ALP, a scale over a set) is not handled: the caller tests the chunk with the filters of the codec, which also run on the stored form.
 */
#ifndef SIFT_CFILT_CC_H
#define SIFT_CFILT_CC_H

#include "col/codec.h"
#include "col/granule/cfilt_kern.h"
#include "core/par/arena.h"

/* What the kernels did on this thread, for the tests and the counters of a scan: the chunks they tested, by the node they read, and those they left to the filters of the codec */
typedef struct SiftCfCcStats
{
	uint64		node[10];		/* by node tag of the codec (SIFT_CCN_*), the root under its wrappers */
	uint64		declined;
} SiftCfCcStats;

extern _Thread_local SiftCfCcStats sift_cf_cc_stats;

/*
 * Tests the keys of the chunk with t by the kernels: true when they read the chunk, with dense holding a bit for each of its sift_cc_nrows keys (the words past them 0); false: the chunk is not one
 * they read and dense is not written. dense has SIFT_COL_NWORDS(n) words.
 *
 * The scan uses it for the sets of more keys than a few ranges can take (granule_filter.c), where the codec decodes the chunk and searches each key: the kernels test a set as a bitmap of the offsets
 * (or of the entries of a dictionary, or the runs), 3 to 200 times faster on every node (tests/cfilt_cc_test.c prints both). The ranges stay with the filters of the codec: they run in vector registers
 * on the vertical packing and the arrays, and are 2.5 to 4 times faster than these kernels, which read the same nodes (also printed).
 */
extern bool sift_cf_cc_filter(const SiftCc *cc, const SiftCfTest *t, uint64 *dense);

/* On a worker (core/par/arena.h): *handled is what sift_cf_cc_filter returns; false, with err filled, when the arena is full */
extern bool sift_cf_cc_filter_a(SiftArena *a, SiftErr *err, const SiftCc *cc, const SiftCfTest *t, uint64 *dense, bool *handled);

#endif							/* SIFT_CFILT_CC_H */
