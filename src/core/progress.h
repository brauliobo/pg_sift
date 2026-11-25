/*
 * core/progress.h - sift.progress: one slot of a table in shared memory for each maintenance job that runs (a rewrite, a merge, a promotion, the merges of a VACUUM), with the
 * phase it is in, the segments it has replaced of those it found and the bytes it wrote. Nothing on disk; a backend that cannot map the table (no room in /dev/shm) goes without it.
 */
#ifndef SIFT_PROGRESS_H
#define SIFT_PROGRESS_H

#include "utils/rel.h"

#include "core/worker.h"

/* The job of this backend starts: true when it took the slot (a job that runs inside another one reports through the outer one's slot, and its end is the outer one's) */
extern bool sift_progress_begin(Relation index, SiftJob job);

/* The job enters a phase: the segments it will replace in it are those the index holds now, none done yet */
extern void sift_progress_phase(Relation index, const char *phase);

/* A step replaced done segments and wrote bytes */
extern void sift_progress_step(int64 done, uint64 bytes);

/* The job ends (began: what sift_progress_begin said); an error that aborts the transaction ends it as well */
extern void sift_progress_end(bool began);
/* The slot of a job whose error was caught and not raised on (core/quarantine/quarantine.h): given back like the abort of the transaction does */
extern void sift_progress_abort(void);

#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_progress_gate;
#endif

#endif							/* SIFT_PROGRESS_H */
