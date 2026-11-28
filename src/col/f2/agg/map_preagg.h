/*
 * col/f2/agg/map_preagg.h - the aggregate scan of jsonb_each / jsonb_each_text of a map column, and of jsonb_to_recordset / jsonb_array_elements_text of an array column (col/f2/agg/unnest/run.c),
 * answering granules from the cubes of the elements (SIFT_PCUBE_ELEMENTS, SIFT_PCUBE_LIST: col/f2/preagg/v2.h) instead of decoding them.
 *
 * The pull of the scan offers each granule it is about to read (SiftF2GranOffer). A granule is answered from the cube of its part that has every element column the statement groups, tests or
 * filters by among its dimensions (agg/unnest/cells.h) and the measures its aggregates read, when every row of the granule is known to the snapshot: the heap pages of its rows are all-visible and
 * none of its rows is dead. The cells go through the derivations, the quals of the element, the FILTER clauses and the sets of keys as the pairs of the run do. Any other granule, a part whose cubes
 * hold no such elements, and a granule the cube left out (a document or a value the leaves cannot hold) are read as they were.
 */
#ifndef SIFT_COL_F2_V6_MAP_PREAGG_H
#define SIFT_COL_F2_V6_MAP_PREAGG_H

#include "agg/engine.h"
#include "agg/vet.h"
#include "col/f2/sink/sink.h"

typedef struct SiftV6MapPreagg SiftV6MapPreagg;

struct UnRun;

/* The state for one scan of the elements of the open run u into the groups of its pipe; NULL when the statement is not one the cubes of the elements can answer */
extern SiftV6MapPreagg *sift_v6_map_preagg_begin(struct UnRun *u);
/* A SiftF2GranSink: true when the granule was answered and is not to be read */
extern bool sift_v6_map_preagg_try_granule(void *arg, const SiftF2GranOffer *offer);
extern void sift_v6_map_preagg_end(SiftV6MapPreagg *m);

#endif							/* SIFT_COL_F2_V6_MAP_PREAGG_H */
