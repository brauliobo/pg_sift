/*
 * core/f2/f2_gate.h - the gates of the durable steps of format major 2 in the testing build: with sift.test_phase_gate = G a backend that enters phase P waits for the advisory lock G + P
 * that the test holds (the fault matrix ends it there by terminate, cancel or an immediate stop). A release build has no gate. Phases 2 to 9 belong to the builds and the
 * logs of the tables (bm25/build.c, table/v1/load, core/v1_col/tomb_write.c, core/sortruns.h).
 */
#ifndef SIFT_CORE_F2_GATE_H
#define SIFT_CORE_F2_GATE_H

#ifdef SIFT_TESTING
#include "bm25/build.h"

#define SIFT_F2_GATE_MANIFEST	10	/* a commit of the manifest, before it takes a page */
#define SIFT_F2_GATE_COMPACT	11	/* the outputs of a compaction are written and nothing names them */
#define SIFT_F2_GATE_TIER		12	/* a part is in the object store and the manifest does not name it */
#define SIFT_F2_GATE_SCOPE		13	/* the summaries of the scopes are written and the manifest does not name them */
#define SIFT_F2_GATE_COL_DELTA	14	/* the column deltas of a partition are appended and no commit has published them */

#define SIFT_F2_GATE(phase)		sift_test_phase_wait(phase)
#else
#define SIFT_F2_GATE(phase)		((void) 0)
#endif

#endif
