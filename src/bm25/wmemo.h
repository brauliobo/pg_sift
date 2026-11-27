/*
 * bm25/wmemo.h - what a bind and a search learned about a wide leaf, kept for the next statement that finds the index as it was.
 */
#ifndef SIFT_BM25_WMEMO_H
#define SIFT_BM25_WMEMO_H

#include "bm25/eval/int.h"

/*
 * The blended df of the leaf and the largest bound of its terms in each segment, which a bind finds by merging the dictionaries of every segment over the
 * leaf's whole range, and the K-th score of the last search of K hits that completed on the leaf (theta is -infinity before one has). They are exact for the
 * state of the index named by the segments' generations and live counts and the document count of the plan, and for no other.
 */
typedef struct SiftWideMemo
{
	double		df_blend;
	float4		theta;
	int64		k;
} SiftWideMemo;

/* The leaf of a ranked plan of one leaf, the write buffers holding none of its terms, and a name to find it by (all_mb: [nsegs]) */
extern bool sift_wide_memo_find(const SiftPlan *p, const EvalLeaf *l, SiftWideMemo *out, uint8 *all_mb);
extern void sift_wide_memo_store(const SiftPlan *p, const EvalLeaf *l, double df_blend, const uint8 *all_mb);
extern void sift_wide_memo_hint(const SiftPlan *p, const EvalLeaf *l, int64 k, float4 theta);
/* The index file is built again in place: its generations start again, so the memos of the file name the contents before */
extern void sift_wide_memo_drop_file(const RelFileLocator *loc);

/* The leaf as the memo has it, when it has a K-th score for it: weighed without the dictionaries (EvalLeaf.warm) */
extern bool sift_wide_memo_warm(SiftPlan *p, EvalLeaf *l, double *df_blend);

#endif
