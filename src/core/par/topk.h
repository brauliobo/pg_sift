/*
 * core/par/topk.h - what a parallel Top-K shares, in pure C: a heap of the K best elements a thread has seen, with the worst of them at its root (an element is offered, and costs a copy only once it is
 * among the K best), the merge of the heaps of W threads into one order, and the bound that the threads raise as they learn what the K-th best is (SiftAtomicBound), so that each can skip what cannot
 * enter.
 *
 * The order is the caller's: a strict total order (the metric, then the key), so that the K best and their order are the same for any number of threads.
 */
#ifndef SIFT_CORE_PAR_TOPK_H
#define SIFT_CORE_PAR_TOPK_H

#include <stdatomic.h>

#include "core/par/arena.h"

/* Whether a ranks before b */
typedef bool (*SiftTopKBetter) (const void *a, const void *b, const void *ctx);
/* The candidate into the slot dst, once it is among the K best: the bytes that it points to (a text key) are copied into str. false when str has no room */
typedef bool (*SiftTopKStore) (void *dst, const void *cand, SiftArena *str, const void *ctx);

typedef struct SiftParTopK
{
	uint8	   *slot;			/* k elements of elem_size bytes: a binary heap, the worst at slot 0 */
	size_t		elem_size;
	uint32		k;
	uint32		n;
	SiftArena	str;			/* what store copies: the bytes behind the elements */
	SiftTopKBetter better;
	SiftTopKStore store;		/* NULL: the candidate is copied as it is */
	const void *ctx;
} SiftParTopK;

/* The heap, and str_bytes of the arena a for the bytes behind its elements: false when a does not hold them */
extern bool sift_par_topk_init(SiftParTopK *tk, SiftArena *a, uint32 k, size_t elem_size, size_t str_bytes, SiftTopKBetter better, SiftTopKStore store,
						   const void *ctx);
/* The candidate, when it ranks among the K best; false when store has no room (the heap is as it was) */
extern bool sift_par_topk_offer(SiftParTopK *tk, const void *cand);

static inline const void *
sift_par_topk_elem(const SiftParTopK *tk, uint32 i)
{
	return tk->slot + (size_t) i * tk->elem_size;
}

/* The worst of the K best; NULL until the heap holds K */
static inline const void *
sift_par_topk_worst(const SiftParTopK *tk)
{
	return tk->n == tk->k ? tk->slot : NULL;
}

/* The elements of the nw heaps in order, as pointers into them (kept in out): *sorted, and in *n the first K of them, or all there are; false when out has no room */
extern bool sift_par_topk_merge(SiftParTopK *const *heap, int nw, SiftArena *out, const void ***sorted, uint32 *n);

/*
 * The bound that threads share: a key that only grows (the larger it is, the more it rules out), raised by compare-and-swap, read before each unit of work. A value becomes a key by the functions
 * below, so that a bound on an integer in either order, or on a score, is one thing. The embedder places it alone on its cache line when threads read it often.
 */
typedef struct SiftAtomicBound
{
	_Atomic uint64 key;
} SiftAtomicBound;

extern void sift_bound_init(SiftAtomicBound *b, uint64 key);
extern uint64 sift_bound_load(const SiftAtomicBound *b);
extern void sift_bound_raise(SiftAtomicBound *b, uint64 key);

/* v as a key: desc, a larger value is the better, and the bound rises with the K-th best; else a smaller one is, and the bound falls */
extern uint64 sift_bound_key_i64(int64 v, bool desc);
/* A score as a key (the larger the better), and back */
extern uint64 sift_bound_key_f32(float v);
extern float sift_bound_f32(uint64 key);

#endif							/* SIFT_CORE_PAR_TOPK_H */
