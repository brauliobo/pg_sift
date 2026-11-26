/*
 * agg/osa/round.h - whether this server's percentile_cont fuses the multiply-add of its proportion and of its interpolation, and float8_accum that of its update (probed on first use)
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
/* whether the server rounds `x * N - Sx` of the update of float8_accum once (probed on first use with the server's own function) */
extern bool osa_accum_fused(void);

#endif							/* SIFT_AGG_V6_OSA_ROUND_H */
