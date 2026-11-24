/*
 * agg/v6/v6_osa_round.h - whether this server's percentile_cont fuses the multiply-add of its proportion and of its interpolation (probed on first use)
 */
#ifndef SIFT_AGG_V6_OSA_ROUND_H
#define SIFT_AGG_V6_OSA_ROUND_H

#include "postgres.h"

typedef struct OsaRound
{
	bool		prop;
	bool		lerp;
} OsaRound;

extern const OsaRound *osa_round(bool array);
extern double osa_prop(double frac, int64 n, int64 first, bool fused);
extern double osa_lerp(double lo, double hi, double prop, bool fused);

#endif							/* SIFT_AGG_V6_OSA_ROUND_H */
