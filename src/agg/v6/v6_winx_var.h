/*
 * agg/v6/v6_winx_var.h - VAR_POP, VAR_SAMP, STDDEV_POP and STDDEV_SAMP of a window as state that folds one row at a time, with the results of PostgreSQL's own aggregates.
 *
 * float8: the Youngs-Cramer update of float8_accum (n, the sum, the sum of squared deviations), the overflow rule and the NaN of an infinite input included; the result is the quotient of
 * float8_var_pop and float8_var_samp, and its square root for the deviations.
 * integers and numerics of at most 18 digits (the scaled integer of agg/vec.h): the sum in 128 bits and the sum of squares in 256, so nothing rounds while rows are folded; the result is
 * num / den with num = n * sum(x^2) - sum(x)^2 and den = n^2 (population) or n * (n - 1) (sample), as numeric_stddev_internal forms them, for the caller to divide as numeric_div does. num has
 * scale 2 * (the scale of x).
 */
#ifndef SIFT_AGG_V6_WINX_VAR_H
#define SIFT_AGG_V6_WINX_VAR_H

#include "core/host.h"

typedef enum SiftWinVarKind
{
	SWV_VAR_POP,
	SWV_VAR_SAMP,
	SWV_STDDEV_POP,
	SWV_STDDEV_SAMP
} SiftWinVarKind;

static inline bool
sift_winvar_is_sample(SiftWinVarKind k)
{
	return k == SWV_VAR_SAMP || k == SWV_STDDEV_SAMP;
}

static inline bool
sift_winvar_is_stddev(SiftWinVarKind k)
{
	return k == SWV_STDDEV_POP || k == SWV_STDDEV_SAMP;
}

/* 128-bit members of structs that palloc gives: palloc aligns to 8, not to the 16 of the plain types, and the compiler moves the plain ones with aligned instructions */
typedef __int128 SiftWinI128 __attribute__((aligned(8)));
typedef unsigned __int128 SiftWinU128 __attribute__((aligned(8)));

typedef struct SiftU256
{
	uint64		w[4];			/* little endian limbs */
} SiftU256;

typedef struct SiftWinVarF64
{
	uint64		n;
	double		sx;
	double		sxx;
} SiftWinVarF64;

typedef struct SiftWinVarInt
{
	uint64		n;
	SiftWinI128 sx;
	SiftU256	sxx;
} SiftWinVarInt;

/* The exact quotient of an integer state: null when PostgreSQL gives NULL, else num / den (num may be 0) */
typedef struct SiftWinVarExact
{
	bool		null;
	SiftU256	num;
	SiftWinU128 den;
} SiftWinVarExact;

/* false: a finite input made the sum of squares overflow, the error float8_accum raises (22003, value out of range: overflow) */
extern bool sift_winvar_f64_add(SiftWinVarF64 *s, double x);
/* false: the result is NULL */
extern bool sift_winvar_f64_final(const SiftWinVarF64 *s, SiftWinVarKind kind, double *out);

/* false: the sum of squares needs more than 256 bits (some 2^128 rows of the largest int8) */
extern bool sift_winvar_int_add(SiftWinVarInt *s, int64 x);
/* false: num does not fit 256 bits (cannot happen below 2^64 rows of int8) */
extern bool sift_winvar_int_final(const SiftWinVarInt *s, SiftWinVarKind kind, SiftWinVarExact *out);

/* The digits of v with a decimal point scale digits from the right ("12345" scale 2 is "123.45"; "5" scale 3 is "0.005"); false when cap is too small */
extern bool sift_u256_to_dec(const SiftU256 *v, int scale, char *buf, size_t cap);
extern bool sift_u128_to_dec(unsigned __int128 v, char *buf, size_t cap);

#endif							/* SIFT_AGG_V6_WINX_VAR_H */
