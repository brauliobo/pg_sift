/*
 * core/ring.h - the slot a cache of a few entries takes next, round robin (a ring of memos, of kept blocks, of tests). The count is unsigned and the number of slots a power of two
 * (checked when it is compiled), so the slot is the low bits of a count that wraps at 2^32 without a jump; a signed count passes INT_MAX into undefined behavior and then gives a negative
 * remainder, which is an index before the array. Every ring of the extension counts through this header (scripts/lint.sh, ring-counter).
 */
#ifndef SIFT_RING_H
#define SIFT_RING_H

#include <stdint.h>

typedef struct SiftRing
{
	uint32_t	count;			/* the slots handed out so far, modulo 2^32 */
} SiftRing;

#ifdef SIFT_TESTING
/* sift._test_ring_start: added to every count, so that a ring is a few slots from the wrap of its count at its first use (core/ring_test.c); sift._test_ring_sites tells which rings crossed it */
extern uint32_t sift_ring_test_start;
extern void sift_ring_test_note(const char *site, uint32_t at);
#endif

static inline uint32_t
sift_ring_take(SiftRing *ring, uint32_t mask, const char *site)
{
	uint32_t	at = ring->count++;

#ifdef SIFT_TESTING
	at += sift_ring_test_start;
	sift_ring_test_note(site, at);
#else
	(void) site;
#endif
	return at & mask;
}

/* The slot of RING (an lvalue of type SiftRing) among N (a constant power of two) that holds the oldest entry: the next to replace */
#define SIFT_RING_SLOT(ring, n) \
	((void) sizeof(char[((n) > 0 && ((n) & ((n) - 1)) == 0) ? 1 : -1]), sift_ring_take(&(ring), (uint32_t) (n) - 1, #ring))

#endif							/* SIFT_RING_H */
