/*
 * agg/v6/v6_gather.h - the rows of a vector picked by an index list: the pairs of a join or an unnest each name the row of the fact side they came from, and the columns of the fact side
 * follow them into the batch of pairs.
 *
 * Portable: only core/host.h and agg/vec.h.
 */
#ifndef SIFT_AGG_V6_GATHER_H
#define SIFT_AGG_V6_GATHER_H

#include "core/host.h"

#include "agg/vec.h"

#define SIFT_GATHER_NONE	UINT32_MAX

/* The bytes of one value of a vector of the types of a column of the scan: SVEC_I64 and SVEC_F64 8, SVEC_STRVIEW 16 */
extern size_t sift_gather_width(SiftVecType t);
/*
 * out_data[i] = src value idx[i] and the validity into out_valid (SIFT_VEC_WORDS words) for i < n; an idx of SIFT_GATHER_NONE is NULL. out becomes the vector of the n rows: dense, with the type
 * of src. src->valid is read when src->all_valid is false.
 */
extern void sift_vec_gather(SiftVec *out, uint64 *out_valid, void *out_data, const SiftVec *src, const uint32 *idx, uint32 n);

#endif							/* SIFT_AGG_V6_GATHER_H */
