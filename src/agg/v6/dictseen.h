/*
 * agg/v6/dictseen.h - the distinct strings of a column that the parts code by a dictionary, counted without expanding a row: a row of a batch is one bit of the dictionary of its part, and when the
 * scan leaves the part the strings of the bits that are set join the set of the strings seen so far (a hash table, the bytes in an arena), which holds each string once
 * whatever the number of parts that name it and the order of their dictionaries. What reaches the exact set of v6_distinct.h is that set, a string once, not a row of the scan.
 */
#ifndef SIFT_AGG_V6_DICTSEEN_H
#define SIFT_AGG_V6_DICTSEEN_H

#include "core/host.h"

/* A dictionary of a part: gen names the part (a generation is never reused), code c is the string bytes[off[c] .. off[c + 1]); it is valid for the call that gives it */
typedef struct SiftDictSrc
{
	uint64		gen;
	uint32		n;
	const uint32 *off;
	const char *bytes;
} SiftDictSrc;

/* A dictionary of more codes, or of more bytes, is not taken: the caller reads its rows as strings */
#define SIFT_DICTSEEN_MAX_CODES		(UINT32_C(1) << 20)
#define SIFT_DICTSEEN_MAX_BYTES		(UINT32_C(32) << 20)

typedef struct SiftDictSeen SiftDictSeen;
typedef void (*SiftDictSeenEmit) (void *arg, const void *bytes, uint32 len);

/* cap_bytes: the bytes of strings the set holds before it hands them to emit and starts again (the set it feeds spills, this one does not) */
extern SiftDictSeen *sift_dictseen_create(MemoryContext cx, uint64 cap_bytes);

/*
 * The rows of a batch: code[r] of every row r of take (a bitmap of n_raw rows) whose bit is set in valid (NULL: all) is seen in the dictionary src. When the dictionary is another part's, the part the set
 * was reading is folded in first, and what overflows the cap goes to emit. False, with nothing done, when the dictionary is too large to take.
 */
extern bool sift_dictseen_batch(SiftDictSeen *d, const SiftDictSrc *src, const uint32 *code, const uint64 *take, const uint64 *valid, uint16 n_raw, SiftDictSeenEmit emit, void *arg);

/*
 * Whether the dictionary is one the set takes; a part with no predicate, no deletion and no NULL-hiding column patch has a row of each string of its dictionary, so that sift_dictseen_part says them all seen
 * without a code of a row being read. The part being read is folded in first, as for a batch of another part.
 */
extern bool sift_dictseen_fits(const SiftDictSrc *src);
extern void sift_dictseen_part(SiftDictSeen *d, const SiftDictSrc *src, SiftDictSeenEmit emit, void *arg);

/* The number of distinct strings seen, with the part being read folded in, when none has gone to emit (so that the set holds them all): false otherwise */
extern bool sift_dictseen_exact(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg, uint64 *count);

/* Folds the part being read and hands every string seen to emit, once each, in no order; the set is empty afterwards */
extern void sift_dictseen_drain(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg);

/* The strings of the parts folded so far (the part being read holds at most the codes of its dictionary more) */
extern uint64 sift_dictseen_count(const SiftDictSeen *d);
extern void sift_dictseen_free(SiftDictSeen *d);

#endif							/* SIFT_AGG_V6_DICTSEEN_H */
