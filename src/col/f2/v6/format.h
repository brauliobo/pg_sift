/*
 * col/f2/v6/format.h - the registration of the storage format of the relations of format major 2: a sift_col index and a sift_table, both read through their SiftF2Store.
 */
#ifndef SIFT_F2_V6_FORMAT_H
#define SIFT_F2_V6_FORMAT_H

#include "postgres.h"

#include "agg/v6/vexpr.h"

/* Registers the format at load, so that the operators of SiftAggV6 that read through it (the joins among them) are offered */
extern void sift_f2_v6_format_init(void);

/* The batches of the input over the relation (SiftStoreOps.open); the source is closed by its close */
extern SiftV6Source *sift_f2_v6_source_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted);

#endif							/* SIFT_F2_V6_FORMAT_H */
