/*
 * agg/settings.h - the settings every aggregate scan reads
 */
#ifndef SIFT_AGG_V6_SETTINGS_H
#define SIFT_AGG_V6_SETTINGS_H

#include "postgres.h"

#include "nodes/pathnodes.h"

extern PGDLLIMPORT bool sift_enable_agg;
extern PGDLLIMPORT bool sift_agg_exact_float;
extern PGDLLIMPORT bool sift_agg_force;
extern PGDLLIMPORT double sift_agg_scale_horizon;

extern void sift_v6_define_gucs(void);

/* sift.agg_force: the price of a path of the aggregate scan against the normal plan's */
extern void sift_v6_force_price(Path *p);
extern Cost sift_v6_forced(Cost c);

#endif							/* SIFT_AGG_V6_SETTINGS_H */
