/*
 * agg/v6/v6_settings.h - the settings every aggregate scan reads
 */
#ifndef SIFT_AGG_V6_SETTINGS_H
#define SIFT_AGG_V6_SETTINGS_H

#include "postgres.h"

extern PGDLLIMPORT bool sift_enable_agg;
extern PGDLLIMPORT bool sift_agg_exact_float;
extern PGDLLIMPORT bool sift_agg_force;
extern PGDLLIMPORT double sift_agg_scale_horizon;

extern void sift_v6_define_gucs(void);

#endif							/* SIFT_AGG_V6_SETTINGS_H */
