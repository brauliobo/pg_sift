/*
 * agg/par/derive.h - the keys and arguments of the statement that the scan computes from columns (SiftV6Derive of agg/engine.h) for the compute threads: the rows of a granule that pass, from the
 * leaves par_load decoded, in pure C (no memory is taken: a text is a view into the bytes of the column or into the spec). The kernels are those of the serial scan (agg/expr/str.h).
 * A NULL in a column it reads is not for a thread: the caller refuses the granule before, and the one that extract gives for an infinite timestamp is its false.
 */
#ifndef SIFT_AGG_V6_PAR_DERIVE_H
#define SIFT_AGG_V6_PAR_DERIVE_H

#include "postgres.h"

#include "agg/par/group.h"
#include "agg/engine.h"

struct ParLoad;

/* A key or an argument as the threads read it: the column itself (d is NULL), or d computed from the leaf `at` and the leaves of its conditions */
typedef struct ParSrc
{
	const SiftV6Derive *d;
	int			at;
	int			cond[SIFT_V6_MAX_CASE];
} ParSrc;

/* The value of each row that passes, as a 64-bit integer xor flip, in out[0 .. nact) (an integer column, length, extract); false when a row has none (extract of an infinite timestamp) */
extern bool par_src_ints(const ParSrc *s, const struct ParLoad *ld, uint64 flip, uint64 *out);

/* The text of each row that passes, in out[0 .. nact) (a text column, the host of an URL, CASE) */
extern void par_src_strs(const ParSrc *s, const struct ParLoad *ld, SiftStrRef *out);

#endif							/* SIFT_AGG_V6_PAR_DERIVE_H */
