/*
 * sift_kernels.h - scoring kernels.
 *
 * One dispatch table per SIMD level, selected through sift_cpu_register().
 * The scalar table is the specification: every level produces bit-identical
 * results (IEEE float32, round to nearest, no FMA: -ffp-contract=off).
 *
 * Exact BM25, every operation rounded to float32 in this order:
 *   c0  = k1 * (1 - b)                     per query (sift_kern_bm25_init)
 *   c1  = (k1 * b) / avgdl
 *   K   = c0 + c1 * (float) dl             per document (knorm)
 *   x   = (float) tf / ((float) tf + K)    per (document, term) (bm25_accum)
 *   s   = w * x,  w = (boost * idf) * (k1 + 1)  (sift_kern_term_weight)
 *   score += s                             terms in the caller's order
 * For tf >= 1, k1 >= 0, 0 <= b <= 1, avgdl > 0 and finite w, no NaN or Inf
 * is produced (tf + K >= 1).
 *
 * Page-run layout: the mask-driven kernels take npages consecutive pages.
 * mask holds SIFT_OFFMASK_WORDS words per page (bit off-1 = heap offset off);
 * float arrays hold SIFT_KERN_SLOTS slots per page (slot off-1). Slots
 * outside the mask keep their values. Masks and float arrays are
 * SIFT_SIMD_ALIGN aligned. One call per block rather than per page is
 * 1.2x to 2.8x faster.
 *
 * Padding: SIMD levels may read up to SIFT_KERN_PAD bytes past the last
 * used byte of tf[] and of a tf stream, and tf_decode may write that far
 * past tf[count]. Callers allocate those bytes (contents irrelevant).
 */
#ifndef SIFT_KERNELS_H
#define SIFT_KERNELS_H

#include "sift.h"
#include "sift_cpu.h"

#define SIFT_KERN_SLOTS		(SIFT_OFFMASK_WORDS * 64)
#define SIFT_KERN_PAD		64

/* Per-query length normalization constants; scalar, computed once */
typedef struct SiftBm25
{
	float4		k1;
	float4		b;
	float4		avgdl;
	float4		c0;				/* k1 * (1 - b) */
	float4		c1;				/* (k1 * b) / avgdl */
} SiftBm25;

/* Position in a term's tf stream; tf_decode and sift_kern_tf_encode advance it */
typedef struct SiftTfPos
{
	uint32		rank;			/* next posting = next L1 bit */
	uint32		nib;			/* next nibble */
	uint32		esc;			/* next escape byte */
} SiftTfPos;

typedef struct SiftKernels
{
	/*
	 * Rank -> offset space: out = mask & pdep(rankbits, base) over nwords words;
	 * rankbits (a word array, read from bit pos) holds one bit per set bit of
	 * base, in order. out may be mask or base. Returns popcount(out).
	 */
	int			(*expand) (uint64 *out, const uint64 *mask, const uint64 *base, const uint64 *rankbits,
						   uint32 pos, int nwords);

	/*
	 * Decodes count postings of a term's tf stream into
	 * tf[0..count): posting r has L1 bit r (tf > 1); L1 members take the
	 * next nibble n (low nibble first): tf = n + 2 if n < 15, else the next
	 * escape byte e gives tf = min(e + 17, 255). Advances pos.
	 */
	void		(*tf_decode) (uint8 *tf, const uint8 *l1, const uint8 *nib, const uint8 *esc,
							  SiftTfPos *pos, uint32 count);

	/* knorm[o] = K(dl[p][o]) for slots in mask; dl[p] is page p's offset-indexed row */
	void		(*knorm) (float4 *knorm, const uint16 *const *dl, const uint64 *mask, int npages,
						  const SiftBm25 *bm);

	/*
	 * scores[o] += w * x(tf, knorm[o]) for slots in mask, where tf is taken
	 * in rank order from tf[] (rank = position of o within mask, counted
	 * across the pages). Returns popcount(mask), the tf entries consumed.
	 */
	int			(*bm25_accum) (float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
							   const float4 *knorm, float4 w);

	/*
	 * bm25_accum restricted to the slots of cand: tf is still taken in rank
	 * order over mask, only the slots in mask & cand are updated. Returns
	 * popcount(mask).
	 */
	int			(*bm25_sel) (float4 *scores, const uint64 *mask, const uint64 *cand, int npages,
							 const uint8 *tf, const float4 *knorm, float4 w);

	/*
	 * cand &= (scores[o] > thr) for the slots in cand; returns the number of
	 * slots left in cand.
	 */
	int			(*prune_gt) (uint64 *cand, const float4 *scores, int npages, float4 thr);

	/* scores[o] = scores[o] > other[o] ? scores[o] : other[o] for slots in mask (dismax) */
	void		(*max_merge) (float4 *scores, const float4 *other, const uint64 *mask, int npages);

	/* ub[i] += w * (float) bounds[i] for i < n (product rounded, then sum) */
	void		(*ub_accum) (float4 *ub, const uint8 *bounds, float4 w, int n);

	/*
	 * Compresses the slots in mask with scores[slot] > theta, in slot order,
	 * into slot/vals. Slot numbers are page * SIFT_KERN_SLOTS + off - 1.
	 * Returns the number written; slot/vals need npages * SIFT_KERN_SLOTS
	 * entries (entries past the result are clobbered).
	 */
	int			(*select_gt) (uint16 *slot, float4 *vals, const float4 *scores,
							  const uint64 *mask, int npages, float4 theta);

	/*
	 * Visibility map bytes for one SB (64 bytes, 2 bits per heap page,
	 * VISIBILITYMAP_ALL_VISIBLE = low bit) to a 256-bit all-visible page mask.
	 */
	void		(*vm_allvisible) (uint64 *pagemask, const uint8 *vmbytes);
} SiftKernels;

/* Active table; never NULL after _PG_init */
extern PGDLLIMPORT const SiftKernels *sift_kern;

/* Per-level tables, for equivalence tests; NULL when the CPU lacks the level */
extern const SiftKernels *sift_kernels_for(SiftSimdLevel level);

/* Registers the dispatch rebind; called from _PG_init */
extern void sift_kernels_init(void);

/* Requires k1 >= 0, 0 <= b <= 1, avgdl > 0 */
extern void sift_kern_bm25_init(SiftBm25 *bm, float4 k1, float4 b, float4 avgdl);

/* idf = (float) ln(1 + (N - df + 0.5) / (df + 0.5)) in double; w = (boost * idf) * (k1 + 1) */
extern float4 sift_kern_idf(double ndocs, double df);
extern float4 sift_kern_term_weight(float4 boost, float4 idf, float4 k1);

/* Single-pair definitions every kernel matches bit for bit */
static inline float4
sift_kern_knorm1(const SiftBm25 *bm, uint16 dl)
{
	return bm->c0 + bm->c1 * (float4) dl;
}

static inline float4
sift_kern_bm25(float4 w, uint8 tf, float4 knorm)
{
	return w * ((float4) tf / ((float4) tf + knorm));
}

/*
 * Score bounds. sift_kern_bound_u8 = ceil(255 * x) with
 * x = tf / (tf + K_ref(dl)) evaluated in double from ref's k1, b, avgdl,
 * clamped to [1, 255]. sift_kern_ub_weight scales a term weight w for
 * ub_accum: w * c / 255, c = max(1, avgdl_now / avgdl_ref), raised by
 * SIFT_KERN_UB_SLACK and rounded up, so the float32 block UB is >= every
 * float32 score it covers for up to SIFT_MAX_LEAVES terms.
 */
#define SIFT_KERN_UB_SLACK	(1.0 / 4096)

extern uint8 sift_kern_bound_u8(const SiftBm25 *ref, uint32 tf, uint32 dl);
extern float4 sift_kern_ub_weight(float4 w, double avgdl_now, double avgdl_ref);

/*
 * Writes count tf values (1..255) at pos into a term's tf stream; l1 and
 * nib must be zeroed beforehand. Advances pos.
 */
extern void sift_kern_tf_encode(uint8 *l1, uint8 *nib, uint8 *esc, SiftTfPos *pos,
								const uint8 *tf, uint32 count);

/*
 * Top-k by score descending, ties by ascending key: a min-heap whose root is
 * the worst kept hit. Caller-owned storage of k hits.
 */
typedef struct SiftHit
{
	SiftKey		key;
	float4		score;
} SiftHit;

typedef struct SiftTopK
{
	SiftHit    *hit;
	int			k;
	int			n;
} SiftTopK;

static inline bool
sift_hit_better(float4 s, SiftKey key, const SiftHit *h)
{
	return s > h->score || (s == h->score && key < h->key);
}

extern void sift_kern_topk_init(SiftTopK *t, SiftHit *buf, int k);

/*
 * select_gt threshold: exactly the scores >= the worst kept hit pass (equal
 * scores may still win on key); -Inf until the heap is full.
 */
extern float4 sift_kern_topk_theta(const SiftTopK *t);

static inline bool
sift_kern_topk_admits(const SiftTopK *t, float4 s, SiftKey key)
{
	return t->n < t->k || sift_hit_better(s, key, &t->hit[0]);
}

/* Inserts a hit; it must be admitted */
extern void sift_kern_topk_push(SiftTopK *t, float4 s, SiftKey key);

/* Sorts the kept hits best first, in place (the heap is consumed); returns n */
extern int	sift_kern_topk_sort(SiftTopK *t);

#endif							/* SIFT_KERNELS_H */
