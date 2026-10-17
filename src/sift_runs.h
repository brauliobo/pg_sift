/*
 * sift_runs.h - in-memory inverted buffer and sorted runs, the write path shared by every host: the heap build and promotion
 * of PostgreSQL and the merge of SQLite.
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
#ifndef SIFT_RUNS_H
#define SIFT_RUNS_H

#include "sift_host.h"

#include "sift_analyze.h"
#include "sift_segment.h"

#ifdef SIFT_TESTING
/* Testing knobs (sift.test_*, 0 = off): docs per run, merge fan-in, postings of a segment's largest term */
extern PGDLLIMPORT int sift_test_run_docs;
extern PGDLLIMPORT int sift_test_fanin;
extern PGDLLIMPORT int sift_test_segment_postings;
/* blocks of 64 kB the buffer's pool has (0 = 60000) */
extern PGDLLIMPORT int sift_test_pool_blocks;
/* Called by a merge to a run or a segment after each term (nterms so far, df of the term), when set: how the host tests it */
extern PGDLLIMPORT void (*sift_test_merge_term) (uint32 nterms, uint64 df);
#define TEST_MERGE_TERM(nterms, df)	do { if (sift_test_merge_term) sift_test_merge_term(nterms, df); } while (0)
#define TEST_KNOB(knob, normal)	((knob) > 0 ? (knob) : (normal))
#else
#define TEST_MERGE_TERM(nterms, df)	((void) 0)
#define TEST_KNOB(knob, normal)	(normal)
#endif

typedef struct SiftInvBuf SiftInvBuf;
typedef struct SiftRun SiftRun;

/* A sorted run: a file, which the parallel build shares through a fileset */
struct SiftRun
{
	BufFile    *file;
	uint64		ndocs;
	uint64		maxdf;			/* postings of its largest term */
	uint64		maxblob;		/* bytes the largest term takes of a segment, at most */
	uint64		dict;			/* bytes the terms take of a segment's dictionary, at most */
	int			level;			/* merge passes the run has been through (spill cascade) */
	SharedFileSet *fileset;		/* NULL for a temporary file, else the file sift_run_file(worker, id) of the set */
	int			worker;
	int			id;
};

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

/* ---------- for the parallel build, which keeps its runs in a fileset and merges them as it spills ---------- */

/* Where the runs a build participant writes go: temporary files, or a fileset the leader reads */
typedef struct RunHome
{
	SharedFileSet *fileset;
	int			worker;
	int			nfiles;			/* ids taken */
} RunHome;

/* The most runs one merge reads: its buffers must fit the budget, the leader reads this many of each worker */
#define MAX_MERGE_FANIN	256

extern const char *sift_run_file(int worker, int run);
extern int	sift_merge_fanin(Size budget);
extern SiftRun *sift_merge_to_run(SiftRun **runs, int nruns, Size budget, RunHome *home);
extern int	sift_shrink_runs(SiftRun **runs, int nruns, int target, Size budget, RunHome *home);

/* Postings of the largest term of a segment that fit its memory budget (the posting writer takes about 10 bytes of each) */
extern uint64 sift_seg_postings(Size budget);

#endif							/* SIFT_RUNS_H */
