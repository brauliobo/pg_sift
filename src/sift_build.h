/*
 * sift_build.h - in-memory inverted buffer shared by build and promotion
 * (DESIGN.md (i) Build, (c) Promotion).
 */
#ifndef SIFT_BUILD_H
#define SIFT_BUILD_H

#include "sift_analyze.h"
#include "sift_segment.h"

typedef struct SiftInvBuf SiftInvBuf;

/* budget: bytes of maintenance memory the buffer may use before it is full */
extern SiftInvBuf *sift_inv_create(Size budget, MemoryContext mcxt);

/*
 * Adds a document. Keys in ascending order avoid any posting sort (heap
 * build); otherwise sift_inv_write sorts (promotion). Term bytes are copied.
 */
extern void sift_inv_add(SiftInvBuf *buf, ItemPointer tid, const SiftTermFreq *terms,
						 int nterms, uint32 dl);
extern bool sift_inv_full(const SiftInvBuf *buf);
extern bool sift_inv_empty(const SiftInvBuf *buf);

/* Writes one immutable segment through wt and fills its directory entry; then resets */
extern void sift_inv_write(SiftInvBuf *buf, SiftWriteTarget *wt, uint64 gen,
						   const SiftScoreRef *ref, SiftSegEntry *entry);

#endif							/* SIFT_BUILD_H */
