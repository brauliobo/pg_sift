/*
 * col/setsum.h - the keys of a leaf in a unit: the distinct values the unit holds, as a bitmap or a list
 * where they fit a few percent of the unit (a count(DISTINCT) adds them without reading the rows), else exactly while they are few and as a bloom filter beyond,
 * so that an equality, IN or range test skips the units that cannot hold a match where the zone (a minimum and a maximum) cannot tell.
 */
#ifndef SIFT_COL_SETSUM_H
#define SIFT_COL_SETSUM_H

#include "core/host.h"

#include "col/preagg.h"

#define SIFT_CSET_EXACT			1		/* the sorted distinct keys, as offsets from the smallest in 1, 2, 4 or 8 bytes each */
#define SIFT_CSET_BLOOM			2		/* a bloom filter of the keys */
#define SIFT_CSET_PRES			3		/* every distinct key (column format minor 15): a bitmap of 64-bit words from the word of the smallest key (or PLIST, where that is smaller) */
#define SIFT_CSET_PLIST			4		/* every distinct key as a list (column format minor 15): the exact summary's layout, with no limit on the keys but the bytes */
#define SIFT_CSET_RUNS			5		/* every distinct key as runs of consecutive keys (column format minor 23): the starts and the lengths of the runs, where no bitmap or list of offsets fits */
#define SIFT_CSET_HDR			16
#define SIFT_CSET_LIMIT			32		/* up to this many distinct keys a unit keeps them exactly in a summary, a bloom filter beyond (a key list has no such limit: it takes the bytes of the unit's share) */
#define SIFT_CSET_LIMIT_MAX		1024	/* a stored exact summary has at most this many keys */
#define SIFT_CSET_BLOOM_BITS	10		/* bits a bloom filter takes for each distinct key */
#define SIFT_CSET_BLOOM_MIN_BITS	3		/* the least bits a key gets in the filter a default key column keeps in what its share has left (about a quarter of the other units pass it) */
#define SIFT_CSET_BLOOM_K		5
#define SIFT_CSET_PROBES		8		/* a range that spans at most this many keys is probed key by key in a bloom filter, a wider one is not tested */
#define SIFT_CSET_PRES_PCT		3		/* the key lists and bitmaps of all the leaves of a unit take at most this share of its bytes together */
#define SIFT_CSET_FILTER_PCT	5		/* with the bloom filters of the leaves that got no list: at most this share of the bytes of a unit together */

typedef struct SiftCSet
{
	uint8		kind;
	uint8		aux;			/* EXACT: bytes of a key offset; BLOOM: probes */
	uint32		n;				/* distinct keys */
	int64		base;			/* EXACT, RUNS: the smallest key; PRES: the key of bit 0 (a multiple of 64) */
	uint64		nbits;			/* BLOOM, PRES: bits */
	const uint8 *data;			/* PRES: little endian words, not aligned */
	uint32		dlen;			/* RUNS: bytes of data */
} SiftCSet;

/* Whether a leaf can keep its keys: a pre-aggregate dimension leaf, or a top-level uuid */
extern bool sift_cset_leaf_ok(const SiftColSchema *s, const SiftColDef *d);
/*
 * The option key_columns: 'off' or empty (none), 'auto' (the build chooses: none until it has), 'auto: a, b' (what it chose, which a rebuild keeps)
 * or 'a, b' (named by the user). Resolved against a schema and the names of its leaves (the attribute name or the shredder path) into
 * spec->set (the leaves that keep their keys), spec->pres (those of them that can keep a key list or a bitmap: top-level int2, int4 and date columns,
 * whose keys are the 32-bit values count(DISTINCT) adds) and spec->keyauto. NULL, or why it is not valid; s NULL: the syntax only.
 */
extern const char *sift_cset_keys_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
extern const char *sift_cset_keys_syntax(const char *opt);
/* Whether a leaf can keep a key list or a bitmap */
extern bool sift_cset_pres_leaf_ok(const SiftColSchema *s, const SiftColDef *d);

/*
 * The distinct non-NULL, non-overflow keys of a leaf's chunk, ascending, in out (room for c->n); returns how many. The same rule builds a unit's
 * summary and verifies it.
 */
extern uint32 sift_cset_keys(const SiftCPaggCol *c, int64 *out);

/*
 * Appends the summary of the nd keys (ascending, distinct) to out and returns true, or appends nothing and returns false when it would tell
 * no more than the zone: fewer than two keys, or every integer between the smallest and the largest.
 */
/* a bloom filter of the keys of a unit in at most max_bytes: up to SIFT_CSET_BLOOM_BITS bits for each key, the probes that fit; false when a key would get fewer than SIFT_CSET_BLOOM_MIN_BITS */
extern bool sift_cset_build_filter(const int64 *keys, uint32 nd, Size max_bytes, StringInfo out);
extern bool sift_cset_build(const int64 *keys, uint32 nd, uint32 limit, StringInfo out);

/*
 * Appends the presence of the non-NULL, non-overflow keys of the leaf's column to out, as the smaller of a bitmap and a list of offsets, and returns
 * true, or appends nothing and returns false when there is none or it would take more than max_bytes. A unit whose keys are too many for the list
 * and span too wide a range for the bitmap costs a pass for its smallest and largest key and a hash set that gives up at the most the list holds.
 */
extern bool sift_cset_build_pres(const SiftCPaggCol *c, Size max_bytes, StringInfo out);

/* Parses a stored summary; false when it is malformed (kind, flag bits, count, length, order, filter fill, bitmap ends and population) */
extern bool sift_cset_open(const uint8 *blob, uint32 len, SiftCSet *s);

/* false only when no key of [lo, hi] can be in the unit */
extern bool sift_cset_may_hold(const SiftCSet *s, int64 lo, int64 hi);

/* the ascending distinct keys of an exact summary (n of them) */
extern void sift_cset_members(const SiftCSet *s, int64 *out);

/* Is the stored blob what the writer makes from keys (the verifier): an exact one holds exactly them, a filter has their bits and no others */
extern bool sift_cset_matches(const SiftCSet *s, const int64 *keys, uint32 nd);

#endif							/* SIFT_COL_SETSUM_H */
