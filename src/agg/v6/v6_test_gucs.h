/*
 * agg/v6/v6_test_gucs.h - the sift.test_ settings that outlive the legacy aggregate engine
 */
#ifndef SIFT_AGG_V6_TEST_GUCS_H
#define SIFT_AGG_V6_TEST_GUCS_H

#include "postgres.h"

/* Superblocks whose matches a search scan holds at a time (a page of matches takes 64 bytes) */
extern PGDLLIMPORT int sift_search_chunk_sbs;

/* Rows below which an aggregate scan takes no workers; 0 asks for them whatever they save (sift.test_agg_par_min_rows) */
extern PGDLLIMPORT int sift_agg_par_min_rows;

#ifdef SIFT_TESTING
extern void sift_v6_define_test_gucs(void);
extern void sift_v6_test_wait(void);
extern void sift_v6_test_item_gate(void);
#else
#define sift_v6_test_item_gate() ((void) 0)
#define sift_v6_test_wait() ((void) 0)
#endif

#endif							/* SIFT_AGG_V6_TEST_GUCS_H */
