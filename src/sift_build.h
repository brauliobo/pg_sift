/*
 * sift_build.h - in-memory inverted buffer and sorted runs, shared by build
 * and promotion (DESIGN.md (i) Build, (c) Promotion).
 *
 * The buffer holds docs (key, dl) and per-term posting slices (doc ordinal
 * deltas + tf, about 1.3 bytes per posting) within a memory budget. A full
 * buffer is either written as a segment or spilled to a sorted run (a
 * temporary file); runs with ascending, disjoint key ranges merge into one
 * segment with memory bounded by the largest term (about 10 bytes per
 * posting). Parallel build participants produce runs the same way.
 */
#ifndef SIFT_BUILD_H
#define SIFT_BUILD_H

#include "sift_analyze.h"
#include "sift_segment.h"

typedef struct SiftInvBuf SiftInvBuf;
typedef struct SiftRun SiftRun;

/* budget: bytes of maintenance memory the buffer may use before it is full */
extern SiftInvBuf *sift_inv_create(Size budget, MemoryContext mcxt);

/*
 * Adds a document with nterms >= 1 distinct terms; keys must be strictly
 * ascending (the heap build sorts each page, promotion sorts its records).
 * Term bytes are copied.
 */
extern void sift_inv_add(SiftInvBuf *buf, ItemPointer tid, const SiftTermFreq *terms,
						 int nterms, uint32 dl);
extern bool sift_inv_full(const SiftInvBuf *buf);
extern bool sift_inv_empty(const SiftInvBuf *buf);

/* Writes one immutable segment through wt and fills its directory entry; then resets */
extern void sift_inv_write(SiftInvBuf *buf, SiftWriteTarget *wt, uint64 gen,
						   const SiftScoreRef *ref, SiftSegEntry *entry);

/* Spills the buffer to a sorted run (temporary file, freed by sift_runs_write); then resets */
extern SiftRun *sift_inv_spill(SiftInvBuf *buf);

/* Postings of the run's most frequent term (merge memory planning) */
extern uint64 sift_run_maxdf(const SiftRun *run);

/*
 * Merges runs with ascending, disjoint key ranges into one segment and
 * closes them. Allocates in CurrentMemoryContext.
 */
extern void sift_runs_write(SiftRun **runs, int nruns, SiftWriteTarget *wt, uint64 gen,
							const SiftScoreRef *ref, SiftSegEntry *entry);

#endif							/* SIFT_BUILD_H */
