/*
 * col/f2/pred/ext.h - the predicates of a faceted search that a leaf of format major 2 decides on its stored bytes: lower(col) LIKE 'prefix%' / lower(col) = 'lit' (the folded
 * prefix and equality of a string leaf) and && / @> of an int4[], int8[] or text[] leaf, read in place over the stored image of the array (no allocation per row).
 *
 * Every test answers SiftLikeVerdict: NO and YES are exact, MAYBE leaves the row to the heap (a string whose lower case is ASCII only in Unicode, an array image the reader cannot walk, an
 * @> with a NULL element on the right). Portable (core/host.h).
 */
#ifndef SIFT_F2_PRED_EXT_H
#define SIFT_F2_PRED_EXT_H

#include "core/host.h"
#include "col/col.h"
#include "col/like/simd.h"

/* ---- lower(col) LIKE 'prefix%', lower(col) = 'lit': the pattern is ASCII and lower case already ---- */

/* Whether the pattern has an i or a k: the two ASCII letters that the lower case of U+0130 and U+212A makes, so that a string with bytes above 0x7F may match it */
extern bool sift_f2_iprefix_has_ik(const uint8 *pat, uint32 plen);

/* exact_eq: lower(s) = pat, else lower(s) LIKE pat || '%' */
extern SiftLikeVerdict sift_f2_iprefix_test(const char *s, uint32 len, const uint8 *pat, uint32 plen, bool exact_eq, bool has_ik);

/* The entries of a dictionary of n strings (n + 1 offsets into bytes) that pass the test, as bits of yes, and those that may, as bits of maybe: ceil(n / 64) words each, written whole; returns yes + maybe */
extern uint32 sift_f2_iprefix_dict(uint32 n, const uint32 *off, const uint8 *bytes, const uint8 *pat, uint32 plen, bool exact_eq, uint64 *yes, uint64 *maybe);

/* ---- && and @> over the stored image of an array ---- */

/*
 * Whether the array of the stored image (a detoasted ArrayType, img_len bytes, any alignment) overlaps the query (contains false) or contains it. An empty query overlaps nothing and is
 * contained in every array.
 */
extern SiftLikeVerdict sift_f2_arr_match(SiftArrQuery *q, const uint8 *img, uint32 img_len, bool contains);

#endif							/* SIFT_F2_PRED_EXT_H */
