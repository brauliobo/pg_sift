/*
 * col/like_simd.h - LIKE '%needle%' and ILIKE '%needle%' on the bytes of a string column: a substring search over 32 bytes (AVX2) or 16 (NEON) at a time, with a scalar body that gives the same
 * answers, and a scan of the strings of a dictionary or of a decoded block of strings in one pass over their bytes.
 *
 * The search compares the first and the last byte of the needle with every position of a block at once and verifies only the positions where both match. The case folded form (ILIKE) folds the
 * ASCII letters of both sides; it keeps the bytes of other characters as they are, which is exact for every row but those that hold a character whose lower case is ASCII only in Unicode: U+0130
 * and U+212A (the dotted capital I and the Kelvin sign), whose lower case is `i` and `k`. A needle that has an `i` or a `k` leaves such a row it did not find the needle in as SIFT_LIKE_MAYBE for
 * the decoder to decide (a row with no byte above 0x7F is always decided here).
 *
 * Strings are bytes: the collation is the caller's to check (a nondeterministic collation, or a case folding that is not ASCII, is the heap's).
 */
#ifndef SIFT_LIKE_SIMD_H
#define SIFT_LIKE_SIMD_H

#include "core/host.h"

typedef enum SiftLikeVerdict
{
	SIFT_LIKE_NO,
	SIFT_LIKE_YES,
	SIFT_LIKE_MAYBE				/* only for a case folded needle with an i or a k, on a row with bytes above 0x7F that does not hold it as ASCII */
} SiftLikeVerdict;

typedef struct SiftLikeNeedle
{
	uint8	   *bytes;			/* the needle, with the ASCII capitals made lower case when folded */
	uint32		len;
	bool		fold;
	bool		has_ik;			/* folded, and the needle has an i or a k */
	uint8		first;
	uint8		last;
	uint8		first_or;		/* 0x20 where the byte is a letter and the compare must fold it, else 0 */
	uint8		last_or;
} SiftLikeNeedle;

extern SiftLikeNeedle *sift_like_needle_new(const uint8 *s, uint32 len, bool fold);
extern void sift_like_needle_free(SiftLikeNeedle *nd);

/* A needle over bytes that are folded already (or not to be): nothing is allocated, and the bytes stay the caller's while the needle is used */
extern void sift_like_needle_borrow(SiftLikeNeedle *nd, const uint8 *folded, uint32 len, bool fold);

/* The offset of the first occurrence of the needle in hay (the empty needle is at 0), or -1 */
extern int64 sift_like_find(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen);

/* The same, with a scalar loop, for the tests and for the platforms with no vector body */
extern int64 sift_like_find_scalar(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen);

/* Whether a string holds the needle */
extern SiftLikeVerdict sift_like_test(const SiftLikeNeedle *nd, const uint8 *s, uint32 len);

/*
 * The entries of a dictionary of n strings (off: n + 1 offsets into bytes) that hold the needle, as the bits of match, and those that may (maybe; NULL when none is wanted): ceil(n / 64) words
 * each, written whole. The bytes of all the entries are scanned once, and an occurrence that lies across two entries is not one. Returns how many entries match or may.
 */
extern uint32 sift_like_dict(const SiftLikeNeedle *nd, uint32 n, const uint32 *off, const uint8 *bytes, uint64 *match, uint64 *maybe);

/* Whether a block has a byte above 0x7F */
extern bool sift_like_has_high(const uint8 *p, size_t len);

/*
 * The shape of the pattern of a LIKE: the literal of '%lit%' (contains), 'lit%' (prefix), '%lit' (suffix) and 'lit' (equal), with the escapes taken out (`\%`, `\_`, `\\`) and the runs of `%`
 * at an end taken as one; any other pattern (an `_`, a `%` inside) is OTHER. lit is room for len bytes; *litlen is the literal's length.
 */
typedef enum SiftLikeShape
{
	SIFT_LIKE_OTHER,
	SIFT_LIKE_CONTAINS,
	SIFT_LIKE_PREFIX,
	SIFT_LIKE_SUFFIX,
	SIFT_LIKE_EQUAL
} SiftLikeShape;

extern SiftLikeShape sift_like_parse(const uint8 *pat, uint32 len, uint8 *lit, uint32 *litlen);

/* The kernels of an instruction set (like_simd_x86.c, like_simd_neon.c) */
extern int64 sift_like_find_avx2(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen);
extern bool sift_like_has_high_avx2(const uint8 *p, size_t len);
extern int64 sift_like_find_neon(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen);
extern bool sift_like_has_high_neon(const uint8 *p, size_t len);

/* Whether the needle verifies at hay[pos .. pos + len) (the bytes between the first and the last): shared by the kernels */
extern bool sift_like_verify(const SiftLikeNeedle *nd, const uint8 *at);

#endif							/* SIFT_LIKE_SIMD_H */
