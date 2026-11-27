/*
 * col/f2/agg/map_preagg.h - the aggregate scan of jsonb_each / jsonb_each_text of a map column, and of jsonb_to_recordset / jsonb_array_elements_text of an array column (col/f2/agg/unnest/run.c),
 * answering granules from the cubes of the elements (SIFT_PCUBE_ELEMENTS, SIFT_PCUBE_LIST: col/f2/preagg/v2.h) instead of decoding them.
 *
 * The pull of the scan offers each granule it is about to read (SiftF2GranOffer). A granule is answered from the cube of its part when the statement groups by the string column of the element the
 * cube is of, or by nothing, reads the count of the elements and the count and value of their numeric columns, its tests on the element are tests on that column, and every row of the granule is known
 * to the snapshot: the heap pages of its rows are all-visible and none of its rows is dead. The cells that pass the tests are folded into the groups. Any other granule, a part whose cubes hold no such
 * elements, and a granule the cube left out (a document or a value the leaves cannot hold) are read as they were.
 */
#ifndef SIFT_COL_F2_V6_MAP_PREAGG_H
#define SIFT_COL_F2_V6_MAP_PREAGG_H

#include "agg/engine.h"
#include "agg/vet.h"
#include "col/f2/sink/sink.h"

typedef struct SiftV6MapPreagg SiftV6MapPreagg;

/* The state for one scan of the elements of the statement into g; NULL when the statement is not one the cubes of the elements can answer */
extern SiftV6MapPreagg *sift_v6_map_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6Vet *vet, SiftV6ScanStats *stats);
/* A SiftF2GranSink: true when the granule was answered and is not to be read */
extern bool sift_v6_map_preagg_try_granule(void *arg, const SiftF2GranOffer *offer);
extern void sift_v6_map_preagg_end(SiftV6MapPreagg *m);

#endif							/* SIFT_COL_F2_V6_MAP_PREAGG_H */
