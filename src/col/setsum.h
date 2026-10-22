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
#define SIFT_CSET_PRES			3		/* every distinct key (key_presence, column format minor 15): a bitmap of 64-bit words from the word of the smallest key */
#define SIFT_CSET_HDR			16
#define SIFT_CSET_LIMIT			32		/* default: up to this many distinct keys a unit keeps them exactly */
#define SIFT_CSET_LIMIT_MAX		1024
#define SIFT_CSET_BLOOM_BITS	10		/* bits a bloom filter takes for each distinct key */
#define SIFT_CSET_BLOOM_K		5
#define SIFT_CSET_PROBES		8		/* a range that spans at most this many keys is probed key by key in a bloom filter, a wider one is not tested */
#define SIFT_CSET_PRES_PCT		3		/* a unit keeps its key presence only when the bitmap takes at most this share of the unit's bytes */

typedef struct SiftCSet
{
	uint8		kind;
	uint8		aux;			/* EXACT: bytes of a key offset; BLOOM: probes */
	uint32		n;				/* distinct keys */
	int64		base;			/* EXACT: the smallest key; PRES: the key of bit 0 (a multiple of 64) */
	uint64		nbits;			/* BLOOM, PRES: bits */
	const uint8 *data;			/* PRES: little endian words, not aligned */
} SiftCSet;

/* The option set_columns, resolved against a schema and the names of its leaves (the attribute name or the shredder path): NULL, or why it is not valid */
/* Whether a leaf can have a set: a pre-aggregate dimension leaf, or a top-level uuid */
extern bool sift_cset_leaf_ok(const SiftColSchema *s, const SiftColDef *d);
extern const char *sift_cset_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_cset_syntax(const char *opt);
/* The option key_presence: the same list syntax; its leaves are top-level int2, int4 and date columns, whose keys are the 32-bit values count(DISTINCT) adds */
extern bool sift_cset_pres_leaf_ok(const SiftColSchema *s, const SiftColDef *d);
extern const char *sift_cset_pres_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);

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

/*
 * Appends the presence bitmap of the nd keys (ascending, distinct, at least one) to out and returns true, or appends nothing and returns false when it
 * would take more than max_bytes.
 */
extern bool sift_cset_build_pres(const int64 *keys, uint32 nd, Size max_bytes, StringInfo out);

/* Parses a stored summary; false when it is malformed (kind, flag bits, count, length, order, filter fill, bitmap ends and population) */
extern bool sift_cset_open(const uint8 *blob, uint32 len, SiftCSet *s);

/* false only when no key of [lo, hi] can be in the unit */
extern bool sift_cset_may_hold(const SiftCSet *s, int64 lo, int64 hi);

/* the ascending distinct keys of an exact summary (n of them) */
extern void sift_cset_members(const SiftCSet *s, int64 *out);

/* Is the stored blob what the writer makes from keys (the verifier): an exact one holds exactly them, a filter has their bits and no others */
extern bool sift_cset_matches(const SiftCSet *s, const int64 *keys, uint32 nd);

#endif							/* SIFT_COL_SETSUM_H */
