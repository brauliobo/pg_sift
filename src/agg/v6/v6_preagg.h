/*
 * agg/v6/v6_preagg.h - SiftAggV6 answering granules from the pre-aggregates of their part (col/f2/preagg_v2.h) instead of decoding them.
 *
 * The scan of col/f2/f2_scan.c offers each granule it is about to read (SiftF2GranOffer). A granule is answered from a cube of its part when the statement is one the cubes hold (keys that are
 * dimensions, aggregates that are measures or distinct sets of the cube, tests on dimensions or that its zones decide for every row) and every row of it is known to the snapshot:
 *   A. the heap pages of its rows are all-visible and none of its rows is dead: its cells are folded into the groups;
 *   B. a few of its rows are dead (at most 5%, 64): they are taken out of the cells first, which costs those rows (not for a distinct count, a float sum, or a dead row that is the least or the
 *      greatest of its group);
 *   C. anything else (a boundary granule, a pending delete, more dead rows, a cube that left the granule out): the granule is read as it was, and only it.
 * The other granules of the part are answered from the cube the same.
 */
#ifndef SIFT_AGG_V6_PREAGG_H
#define SIFT_AGG_V6_PREAGG_H

#include "agg/v6/v6.h"
#include "agg/v6/v6_acc.h"
#include "agg/v6/v6_vet.h"
#include "col/f2/f2_sink.h"

typedef struct SiftV6Preagg SiftV6Preagg;

/* The state for one scan of the index or table into g (first: the leaf of each attribute; vet: the heap of an index, NULL for a table); NULL when the statement is not one the cubes can answer */
extern SiftV6Preagg *sift_v6_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, const int *first, SiftV6Vet *vet, SiftV6ScanStats *stats);
/* A SiftF2GranSink: true when the granule was answered and is not to be read */
extern bool sift_v6_preagg_try_granule(void *arg, const SiftF2GranOffer *offer);
/* The groups the cells of the granules go into from now on: a participant of a parallel run folds into the state of the morsel it takes */
extern void sift_v6_preagg_set_group(SiftV6Preagg *pa, SiftV6Group *g);
extern void sift_v6_preagg_end(SiftV6Preagg *pa);

/* group_cells.c: n cells of a granule (keyvec[i] is key i, one row for each; acc[c * naggs + j] the partial state of aggregate j of cell c) folded into the groups */
extern void sift_v6_group_merge_cells(SiftV6Group *g, SiftVec *const *keyvec, const V6Acc *acc, uint16 n);
/* n rows of the keys with a value of the COUNT(DISTINCT) aggregate agg each */
extern void sift_v6_group_merge_dist(SiftV6Group *g, int agg, SiftVec *const *keyvec, SiftVec *arg, uint16 n);

#endif							/* SIFT_AGG_V6_PREAGG_H */
