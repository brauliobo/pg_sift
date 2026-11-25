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

/* The bytes of one value of a vector of the type t, as vec.h lays it out: SVEC_BOOL 1, SVEC_I32 and SVEC_DICT32 4, SVEC_I64, SVEC_F64, SVEC_NUM64 and SVEC_DATUM 8, SVEC_NUM128, SVEC_UUID, SVEC_INTERVAL and SVEC_STRVIEW 16, SVEC_NUM256 32 */
extern size_t sift_gather_width(SiftVecType t);
/*
 * out_data[i] = src value idx[i] and the validity into out_valid (SIFT_VEC_WORDS words) for i < n; an idx of SIFT_GATHER_NONE is NULL. out becomes the vector of the n rows: dense, with the type
 * of src. src->valid is read when src->all_valid is false.
 */
extern void sift_vec_gather(SiftVec *out, uint64 *out_valid, void *out_data, const SiftVec *src, const uint32 *idx, uint32 n);
/* The NaN rows of src (a numeric) for the rows gather took: out->nan is out_nan (SIFT_VEC_WORDS words) when src has any, else NULL */
extern void sift_vec_gather_nan(SiftVec *out, uint64 *out_nan, const SiftVec *src, const uint32 *idx, uint32 n);

#endif							/* SIFT_AGG_V6_GATHER_H */
