/*
 * col/f2/preagg/elem.h - the cubes of the elements of the maps and arrays of a part (SIFT_PCUBE_ELEMENTS, SIFT_PCUBE_LIST: col/f2/preagg/v2.h), which the plan of its cubes (v2.c) ends with.
 */
#ifndef SIFT_PREAGG_ELEM_H
#define SIFT_PREAGG_ELEM_H

#include "col/f2/preagg/v2.h"

/* v2.c: the distinct values of a sample of the n values of an integer column, at most cap + 1; the codes of a string column in the dictionary of the part are kept */
extern uint32 sift_pag_sample_distinct(const SiftColIn *col, int n, uint32 cap);
extern bool sift_pag_dict_codes_kept(const SiftColIn *c, const struct SiftDictEnc *d);

/*
 * A cube of the elements of each map (its `#key` leaf) and each array of objects or strings (each of its string leaves whose codes the part keeps, and each of its integer leaves of few values),
 * and of an array the cube of all those leaves together when the groups of its first granule fit; the numeric leaves of the same elements are the measures of each
 */
extern void sift_pag_add_elem_cubes(const SiftRows *rows, const struct SiftDictEnc *const *dicts, SiftPreaggPlan *plan);

#endif							/* SIFT_PREAGG_ELEM_H */
