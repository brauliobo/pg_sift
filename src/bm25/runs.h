/*
 * bm25/runs.h - in-memory inverted buffer and sorted runs, the write path shared by every host: the heap build and promotion
 * of PostgreSQL and the merge of SQLite.
 *
 * The buffer holds docs (key, dl) and per-term posting slices (doc ordinal
 * deltas + tf [+ positions], about 1.3 bytes per posting without) within a memory budget. A full
 * buffer is either written as a segment or spilled to a sorted run (a
 * temporary file); runs merge into one segment with memory independent of
 * the posting counts (one buffered reader per run, a bounded number at a
 * time: more runs are merged in passes first). Runs come from the heap
 * build (ascending, disjoint key ranges) or from segments being merged
 * (interleaved ranges). The segment a merge writes is streamed too: the docmap and the
 * dictionary index go through temporary files, and the postings of a large term are staged
 * there (sift_segw_spool), so no part of the memory grows with the postings of a term.
 */
#ifndef SIFT_RUNS_H
#define SIFT_RUNS_H

#include "core/host.h"

#include "bm25/analyze.h"
#include "bm25/segment.h"

#ifdef SIFT_TESTING
/* Testing knobs (sift.test_*, 0 = off): docs per run, merge fan-in, postings of a segment's largest term */
extern PGDLLIMPORT int sift_test_run_docs;
extern PGDLLIMPORT int sift_test_fanin;
extern PGDLLIMPORT int sift_test_segment_postings;
/* blocks of 64 kB the buffer's pool has (0 = 60000) */
extern PGDLLIMPORT int sift_test_pool_blocks;
/* bytes of runs under which a range of terms of a parallel build is not cut, and between two samples of the terms of a run (0 = 512 kB and 64 kB) */
extern PGDLLIMPORT int sift_test_range_bytes;
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
	uint64		sum_dl;			/* of its docs */
	uint64		trailer;		/* where the term samples start after its end, 0 for a run without (see run_finish) */
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
 * page, promotion sorts its records). Term bytes are copied. Returns whether the buffer is full now.
 */
extern bool sift_inv_add(SiftInvBuf *buf, ItemPointer tid, const SiftTermFreq *terms,
						 int nterms, uint32 dl);
extern bool sift_inv_full(const SiftInvBuf *buf);

/* Can a doc of nterms terms (from textlen bytes of text) be added? Spill first if not; only docs of sift_inv_small_doc bytes or more need to ask */
extern bool sift_inv_fits(const SiftInvBuf *buf, int nterms, Size textlen);
extern Size sift_inv_small_doc(const SiftInvBuf *buf);

/* Is the key of tid above that of the last doc added (or is the buffer empty)? Spill first if not */
extern bool sift_inv_after(const SiftInvBuf *buf, ItemPointer tid);
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

/* Where the runs a build participant writes go: temporary files, or a fileset the leader reads */
typedef struct RunHome
{
	SharedFileSet *fileset;
	int			worker;
	int			nfiles;			/* ids taken */
} RunHome;

/* The most runs one merge reads: its buffers must fit the budget, the leader reads this many of each worker */
#define MAX_MERGE_FANIN	256

/*
 * The segment of a group of runs written by several processes, each a range of the terms (the build of a parallel CREATE INDEX): the
 * runs hold samples of their terms, which cut the terms of the group into ranges of about the same bytes. The process of the segment
 * writes the docs (sift_runs_write_docs), the others (and it) the postings of a range each (sift_runs_write_range, into a range writer
 * of segment.h), which it then stitches into the segment.
 */
typedef struct SiftTermCut
{
	int			len;
	char		term[SIFT_MAX_TERM_BYTES];
} SiftTermCut;

#define MAX_RANGES		64

/* Whether the runs hold the samples of their terms and are files of a fileset (the processes of a build open them by name) */
extern bool sift_runs_splittable(SiftRun **runs, int nruns);

/* The cuts (in term order) that split the terms of the runs into about target ranges of the same bytes of the runs, none under min_bytes; returns their number (target - 1 at most) */
extern int	sift_runs_cuts(SiftRun **runs, int nruns, Size budget, uint64 min_bytes, int target, SiftTermCut *cuts);

/* Starts the segment of the runs with all their docs; the terms come from the ranges */
extern SiftSegmentWriter *sift_runs_write_docs(SiftRun **runs, int nruns, Size budget, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref,
											   MemoryContext mcxt);

/* The terms from lo (NULL: the first) up to, not including, hi (NULL: the last) of the runs into the range writer */
extern void sift_runs_write_range(SiftRun **runs, int nruns, Size budget, const SiftTermCut *lo, const SiftTermCut *hi, SiftSegmentWriter *rw);

/* Closes the runs of a segment that was written in ranges */
extern void sift_runs_close(SiftRun **runs, int nruns);

extern const char *sift_run_file(int worker, int run);
extern int	sift_merge_fanin(Size budget);
extern SiftRun *sift_merge_to_run(SiftRun **runs, int nruns, Size budget, RunHome *home);
extern int	sift_shrink_runs(SiftRun **runs, int nruns, int target, Size budget, RunHome *home);

/* Postings of the largest term of a segment that fit its memory budget (the posting writer takes about 10 bytes of each) */
extern uint64 sift_seg_postings(Size budget);

#endif							/* SIFT_RUNS_H */
