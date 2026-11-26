/*
 * agg/group/ext.h - the states of the aggregates SiftAggV6 added to count, sum, avg, min and max over numbers: stddev and variance of a float column, bool_or and bool_and, min and max of
 * a text column. Each state is the 32 bytes of a V6Acc (agg/group/int.h): n, v0, v1 and pad; the functions here are portable and know nothing of the group around them.
 *
 * The moments are the transition state of PostgreSQL's float8_accum (N, Sx, Sxx, the update of Youngs and Cramer) and its float8_combine, computed the way they compute it, so that a
 * variance over the rows in the order PostgreSQL reads them is the one it gives. The final functions are float8_var_pop, float8_var_samp and their square roots.
 *
 * A text state keeps its winner in the byte arena of the group: the offset in v0 and the length in v1, or the bytes themselves in v0 when there are at most 8 of them (pad bit 0); the rest of pad
 * is the room the winner has, so a later winner that fits takes its place without a new copy. The offsets stay valid while the group lives, in whatever table or spill the state is moved to.
 */
#ifndef SIFT_AGG_V6_GROUP_EXT_H
#define SIFT_AGG_V6_GROUP_EXT_H

#include "core/host.h"

#include <math.h>
#include <string.h>

/* ---- moments ---- */

typedef struct SiftMom
{
	int64		n;
	double		sx;
	double		sxx;
	int64		flags;
} SiftMom;

_Static_assert(sizeof(SiftMom) == 32, "a state of moments is the 32 bytes of an accumulator");

#define SIFT_MOM_OVERFLOW	1

typedef enum SiftMomFinal
{
	SMF_VAR_POP,
	SMF_VAR_SAMP,
	SMF_STDDEV_POP,
	SMF_STDDEV_SAMP
} SiftMomFinal;

/* `x * N - Sx` of float8_accum, rounded once (fused: the server's build contracts it, agg/osa/round.h) or twice */
static inline double
sift_mom_tmp(double x, double n, double sx, bool fused)
{
	return fused ? fma(x, n, -sx) : (x * n) - sx;
}

static inline void
sift_mom_add(SiftMom *a, double x, bool fused)
{
	const double n0 = (double) a->n,
				n = n0 + 1.0,
				sx = a->sx + x;

	if (a->n > 0)
	{
		const double tmp = sift_mom_tmp(x, n, sx, fused);

		a->sxx += tmp * tmp / (n * n0);
		if (isinf(sx) || isinf(a->sxx))
		{
			if (!isinf(a->sx) && !isinf(x))
				a->flags |= SIFT_MOM_OVERFLOW;
			a->sxx = NAN;
		}
	}
	else if (isnan(x) || isinf(x))
		a->sxx = NAN;
	a->sx = sx;
	a->n++;
}

static inline void
sift_mom_merge(SiftMom *d, const SiftMom *s)
{
	const double n1 = (double) d->n,
				n2 = (double) s->n;
	double		tmp,
				sx,
				sxx;

	if (s->n == 0)
		return;
	if (d->n == 0)
	{
		*d = *s;
		return;
	}
	sx = d->sx + s->sx;
	tmp = d->sx / n1 - s->sx / n2;
	sxx = d->sxx + s->sxx + n1 * n2 * tmp * tmp / (n1 + n2);
	if ((isinf(sx) && !isinf(d->sx) && !isinf(s->sx)) || (isinf(sxx) && !isinf(d->sxx) && !isinf(s->sxx)))
		d->flags |= SIFT_MOM_OVERFLOW;
	d->flags |= s->flags;
	d->n += s->n;
	d->sx = sx;
	d->sxx = sxx;
}

/* false: the aggregate is NULL (no row, or one row of a sample statistic) */
static inline bool
sift_mom_final(SiftMomFinal kind, const SiftMom *a, double *out)
{
	const bool	samp = kind == SMF_VAR_SAMP || kind == SMF_STDDEV_SAMP;
	double		v;

	if (a->n == 0 || (samp && a->n == 1))
		return false;
	v = a->sxx / (samp ? (double) a->n - 1.0 : (double) a->n);
	*out = (kind == SMF_STDDEV_POP || kind == SMF_STDDEV_SAMP) ? sqrt(v) : v;
	return true;
}

/* ---- booleans ---- */

typedef struct SiftBoolAcc
{
	int64		n;
	uint64		v;
	int64		unused[2];
} SiftBoolAcc;

static inline void
sift_bool_offer(SiftBoolAcc *a, bool is_or, bool x)
{
	a->v = a->n == 0 ? (uint64) x : (is_or ? a->v | (uint64) x : a->v & (uint64) x);
	a->n++;
}

static inline void
sift_bool_merge(SiftBoolAcc *d, const SiftBoolAcc *s, bool is_or)
{
	if (s->n == 0)
		return;
	d->v = d->n == 0 ? s->v : (is_or ? d->v | s->v : d->v & s->v);
	d->n += s->n;
}

/* ---- text ---- */

typedef struct SiftTextSlot
{
	int64		n;				/* the non-NULL values folded */
	uint64		v0;				/* the offset in the arena, or the bytes themselves when inline */
	int64		v1;				/* the length */
	int64		pad;			/* bit 0: inline; from bit 8: the room of the winner in the arena */
} SiftTextSlot;

_Static_assert(sizeof(SiftTextSlot) == 32, "a text state is the 32 bytes of an accumulator");

#define SIFT_TSLOT_INLINE	1
#define SIFT_TSLOT_INLINE_MAX	8

/* MEM[MEM-AGG-04]: SiftTextArena append-only string arena (cap doubles up to MaxAllocSize with the old arena live; counted by the spill check; a replaced winner of MIN or MAX that outgrew its room leaves its bytes until the query ends) */
typedef struct SiftTextArena
{
	MemoryContext cx;
	char	   *buf;
	uint64		cap;
	uint64		used;
} SiftTextArena;

/* the order of two values under the collation (NULL: bytewise, which is C's, and the order of memcmp that PostgreSQL's text compare gives for a deterministic collation only where it is the same) */
typedef int (*SiftTextCmpFn) (void *arg, const char *a, uint32 alen, const char *b, uint32 blen);

static inline const char *
sift_tslot_bytes(const SiftTextArena *ar, const SiftTextSlot *s, uint32 *len)
{
	*len = (uint32) s->v1;
	return (s->pad & SIFT_TSLOT_INLINE) ? (const char *) &s->v0 : ar->buf + s->v0;
}

static inline int
sift_tslot_cmp(SiftTextCmpFn fn, void *arg, const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			c;

	if (fn != NULL)
		return fn(arg, a, alen, b, blen);
	c = memcmp(a, b, Min(alen, blen));
	return c != 0 ? c : (alen < blen ? -1 : alen > blen);
}

static inline void
sift_tslot_set(SiftTextArena *ar, SiftTextSlot *s, const char *p, uint32 len)
{
	if (len <= SIFT_TSLOT_INLINE_MAX)
	{
		s->v0 = 0;
		memcpy(&s->v0, p, len);
		s->pad = SIFT_TSLOT_INLINE;
	}
	else
	{
		const uint64 room = (uint64) s->pad >> 8;

		if ((s->pad & SIFT_TSLOT_INLINE) || room < len)
		{
			if (ar->used + len > ar->cap)
			{
				if (ar->used + len > MaxAllocSize)
					ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the text MIN or MAX values of the groups are over 1 GB"),
									errhint("Group by fewer columns or aggregate shorter values.")));
				ar->cap = Min(Max(ar->cap * 2, Max(ar->used + len, 4096)), MaxAllocSize);
				ar->buf = ar->buf == NULL ? MemoryContextAlloc(ar->cx, ar->cap) : repalloc(ar->buf, ar->cap);
			}
			s->v0 = ar->used;
			s->pad = (int64) ((uint64) len << 8);
			ar->used += len;
		}
		memcpy(ar->buf + s->v0, p, len);
	}
	s->v1 = len;
}

/* is the value one that replaces the winner of the state: the least for a minimum, the greatest for a maximum */
static inline void
sift_tslot_offer(SiftTextArena *ar, SiftTextSlot *s, bool is_min, const char *p, uint32 len, SiftTextCmpFn fn, void *arg)
{
	if (s->n > 0)
	{
		uint32		wlen;
		const char *w = sift_tslot_bytes(ar, s, &wlen);
		const int	c = sift_tslot_cmp(fn, arg, p, len, w, wlen);

		if (is_min ? c >= 0 : c <= 0)
		{
			s->n++;
			return;
		}
	}
	sift_tslot_set(ar, s, p, len);
	s->n++;
}

/* d takes the winner of s when it is better; both are states of the one arena, and s is not used again */
static inline void
sift_tslot_merge(SiftTextArena *ar, SiftTextSlot *d, const SiftTextSlot *s, bool is_min, SiftTextCmpFn fn, void *arg)
{
	uint32		slen;
	const char *sp;

	if (s->n == 0)
		return;
	if (d->n == 0)
	{
		*d = *s;
		return;
	}
	sp = sift_tslot_bytes(ar, s, &slen);
	{
		uint32		dlen;
		const char *dp = sift_tslot_bytes(ar, d, &dlen);
		const int	c = sift_tslot_cmp(fn, arg, sp, slen, dp, dlen);
		const int64 n = d->n + s->n;

		if (is_min ? c < 0 : c > 0)
			*d = *s;
		d->n = n;
	}
}

#endif							/* SIFT_AGG_V6_GROUP_EXT_H */
