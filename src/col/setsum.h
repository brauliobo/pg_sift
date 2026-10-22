/*
 * col/setsum.h - the small-set summary of a leaf in a unit: the distinct values the unit holds,
 * exactly while they are few and as a bloom filter beyond, so that an equality, IN or range test skips the units that cannot hold a match
 * where the zone (a minimum and a maximum) cannot tell.
 */
#ifndef SIFT_COL_SETSUM_H
#define SIFT_COL_SETSUM_H

#include "core/host.h"

#include "col/preagg.h"

#define SIFT_CSET_EXACT			1		/* the sorted distinct keys, as offsets from the smallest in 1, 2, 4 or 8 bytes each */
#define SIFT_CSET_BLOOM			2		/* a bloom filter of the keys */
#define SIFT_CSET_HDR			16
#define SIFT_CSET_LIMIT			32		/* default: up to this many distinct keys a unit keeps them exactly */
#define SIFT_CSET_LIMIT_MAX		1024
#define SIFT_CSET_BLOOM_BITS	10		/* bits a bloom filter takes for each distinct key */
#define SIFT_CSET_BLOOM_K		5
#define SIFT_CSET_PROBES		8		/* a range that spans at most this many keys is probed key by key in a bloom filter, a wider one is not tested */

typedef struct SiftCSet
{
	uint8		kind;
	uint8		aux;			/* EXACT: bytes of a key offset; BLOOM: probes */
	uint32		n;				/* distinct keys */
	int64		base;			/* EXACT: the smallest key */
	uint64		nbits;			/* BLOOM */
	const uint8 *data;
} SiftCSet;

/* The option set_columns, resolved against a schema and the names of its leaves (the attribute name or the shredder path): NULL, or why it is not valid */
extern const char *sift_cset_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_cset_syntax(const char *opt);

/*
 * The distinct non-NULL, non-overflow keys of a leaf's chunk, ascending, in out (room for c->n); returns how many. The same rule builds a unit's
 * summary and verifies it.
 */
extern uint32 sift_cset_keys(const SiftCPaggCol *c, int64 *out);

/*
 * Appends the summary of the nd keys (ascending, distinct) to out and returns true, or appends nothing and returns false when it would tell
 * no more than the zone: fewer than two keys, or every integer between the smallest and the largest.
 */
extern bool sift_cset_build(const int64 *keys, uint32 nd, uint32 limit, StringInfo out);

/* Parses a stored summary; false when it is malformed (kind, flag bits, count, length, order, filter fill) */
extern bool sift_cset_open(const uint8 *blob, uint32 len, SiftCSet *s);

/* false only when no key of [lo, hi] can be in the unit */
extern bool sift_cset_may_hold(const SiftCSet *s, int64 lo, int64 hi);

/* the ascending distinct keys of an exact summary (n of them) */
extern void sift_cset_members(const SiftCSet *s, int64 *out);

/* Is the stored blob what the writer makes from keys (the verifier): an exact one holds exactly them, a filter has their bits and no others */
extern bool sift_cset_matches(const SiftCSet *s, const int64 *keys, uint32 nd);

#endif							/* SIFT_COL_SETSUM_H */
