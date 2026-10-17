/*
 * sift_build.h - in-memory inverted buffer and sorted runs, shared by build
 * and promotion.
 *
 * The buffer holds docs (key, dl) and per-term posting slices (doc ordinal
 * deltas + tf [+ positions], about 1.3 bytes per posting without) within a memory budget. A full
 * buffer is either written as a segment or spilled to a sorted run (a
 * temporary file); runs merge into one segment with memory independent of
 * the posting counts (one buffered reader per run, a bounded number at a
 * time: more runs are merged in passes first). Runs come from the heap
 * build (ascending, disjoint key ranges) or from segments being merged
 * (interleaved ranges).
 */
#ifndef SIFT_BUILD_H
#define SIFT_BUILD_H

#include "storage/sharedfileset.h"

#include "sift_analyze.h"
#include "sift_segment.h"

#ifdef SIFT_TESTING
/* Testing knobs (sift.test_*, 0 = off): docs per run, merge fan-in, postings of a segment's largest term */
extern PGDLLIMPORT int sift_test_run_docs;
extern PGDLLIMPORT int sift_test_fanin;
extern PGDLLIMPORT int sift_test_segment_postings;
/* kB: the most memory a CREATE INDEX of this backend allocated since the variable was last set (to 0) */
extern PGDLLIMPORT int sift_test_build_peak;
/* blocks of 64 kB the buffer's pool has (0 = 60000), bytes of the text of a heap page it holds (0 = by memory) */
extern PGDLLIMPORT int sift_test_pool_blocks;
extern PGDLLIMPORT int sift_test_page_text;
#endif

typedef struct SiftInvBuf SiftInvBuf;
typedef struct SiftRun SiftRun;

/*
 * budget: bytes of maintenance memory the buffer may use before it is full.
 * positions: the docs' terms carry position records (SiftTermFreq.pos), which
 * the segments written from the buffer keep.
 */
extern SiftInvBuf *sift_inv_create(Size budget, MemoryContext mcxt, bool positions);

/*
 * Adds a document (nterms = 0 and dl = 0 for text without tokens: it still
 * counts in N); keys must be strictly ascending (the heap build sorts each
 * page, promotion sorts its records). Term bytes are copied.
 */
extern void sift_inv_add(SiftInvBuf *buf, ItemPointer tid, const SiftTermFreq *terms,
						 int nterms, uint32 dl);
extern bool sift_inv_full(const SiftInvBuf *buf);

/* Can a doc of nterms terms (from textlen bytes of text) at tid be added? Spill first if not, whatever the buffer holds */
extern bool sift_inv_fits(const SiftInvBuf *buf, ItemPointer tid, int nterms, Size textlen);
extern bool sift_inv_empty(const SiftInvBuf *buf);

/* Writes one immutable segment through wt and fills its directory entry; then resets */
extern void sift_inv_write(SiftInvBuf *buf, SiftWriteTarget *wt, uint64 gen,
						   const SiftScoreRef *ref, SiftSegEntry *entry);

/* Spills the buffer to a sorted run (temporary file, freed by sift_runs_write); then resets */
extern SiftRun *sift_inv_spill(SiftInvBuf *buf);

/* Spills to a file of a fileset (parallel build workers hand their runs to the leader) */
extern SiftRun *sift_inv_spill_shared(SiftInvBuf *buf, SharedFileSet *fileset, int worker, int id);

/*
 * Spills the live part of an immutable segment (docs and postings VACUUM did
 * not remove) as a run; NULL when it has none.
 */
extern SiftRun *sift_run_from_segment(SiftSegment *seg, const SiftScoreRef *ref, MemoryContext mcxt);

/*
 * Merges runs into one segment and closes them; the key ranges of the runs
 * may interleave. Allocates in CurrentMemoryContext.
 */
extern void sift_runs_write(SiftRun **runs, int nruns, SiftWriteTarget *wt, uint64 gen,
							const SiftScoreRef *ref, SiftSegEntry *entry);

#endif							/* SIFT_BUILD_H */
