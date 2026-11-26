/*
 * col/f2/agg/preagg.h - SiftAggV6 answering granules from the pre-aggregates of their part (col/f2/preagg/v2.h) instead of decoding them.
 *
 * The scan of col/f2/scan/scan.c offers each granule it is about to read (SiftF2GranOffer). A granule is answered from a cube of its part when the statement is one the cubes hold (keys that are
 * dimensions, aggregates that are measures or distinct sets of the cube, tests on dimensions or that its zones decide for every row) and every row of it is known to the snapshot:
 *   A. the heap pages of its rows are all-visible and none of its rows is dead: its cells are folded into the groups;
 *   B. a few of its rows are dead (at most 5%, 64): they are taken out of the cells first, which costs those rows (not for a distinct count, a float sum, or a dead row that is the least or the
 *      greatest of its group);
 *   C. anything else (a boundary granule, a pending delete, more dead rows, a cube that left the granule out): the granule is read as it was, and only it.
 * The other granules of the part are answered from the cube the same.
 */
#ifndef SIFT_COL_F2_V6_PREAGG_H
#define SIFT_COL_F2_V6_PREAGG_H

#include "agg/engine.h"
#include "agg/acc.h"
#include "agg/vet.h"
#include "col/f2/sink/sink.h"
#include "col/f2/preagg/v2.h"

typedef struct SiftV6Preagg SiftV6Preagg;

/* The generation of the dictionary of a part of host (0: no part), which tells it from the dictionaries of the parts of the other children of a partitioned table */
extern uint64 sift_v6_dict_gen(Relation host, uint64 part_gen);

/* The state for one scan of the index or table into g (first: the leaf of each attribute; vet: the heap of an index, NULL for a table); NULL when the statement is not one the cubes can answer */
extern SiftV6Preagg *sift_v6_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, const int *first, SiftV6Vet *vet, SiftV6ScanStats *stats);
/* A SiftF2GranSink: true when the granule was answered and is not to be read */
extern bool sift_v6_preagg_try_granule(void *arg, const SiftF2GranOffer *offer);
/* The groups the cells of the granules go into from now on: a participant of a parallel run folds into the state of the morsel it takes */
extern void sift_v6_preagg_set_group(SiftV6Preagg *pa, SiftV6Group *g);
extern void sift_v6_preagg_end(SiftV6Preagg *pa);
/* Whether every row of the granule is known to the snapshot without a look at the row: the heap pages of a sift_col index are all-visible, the inserters of a sift_table are settled */
extern bool sift_v6_preagg_rows_known(SiftV6Vet *vet, const SiftF2GranOffer *o);
/* The state of the aggregate a from the measure of a cell */
extern void sift_v6_preagg_meas_acc(const SiftV6Agg *a, const SiftPreaggMeasure *pm, V6Acc *out);

#endif							/* SIFT_COL_F2_V6_PREAGG_H */
