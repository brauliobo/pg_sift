/*
 * core/range.h - bounds and ranges of the key of a partition: the one comparison, successor and prefix search the directory, the maintenance picks, the scope runs and the tails share.
 *
 * A bound is a key tuple of at most SIFT_BOUND_COMPS components (an integer or NULL each) and, after them, the 48-bit heap address that splits one key between two partitions, in an
 * encoding whose byte order is the key order (NULL after every integer, a shorter bound before every bound it is a prefix of): comparing two bounds is a memcmp. A bound is exact, never
 * cut: a partition is found by comparing its lower bound with a key, and a cut bound would send the keys between it and the true bound to the wrong partition. Partitions tile the key space,
 * so a partition's upper bound is the lower bound of the next and only lower bounds are stored; a range has both for the callers that compare two of them.
 */
#ifndef SIFT_RANGE_H
#define SIFT_RANGE_H

#include <stdbool.h>
#include <stdint.h>

#define SIFT_BOUND_COMPS	3		/* key components of a bound: three integers and the address are the most 38 bytes hold */
#define SIFT_BOUND_PAYLOAD	38
#define SIFT_BOUND_BYTES	40		/* a bound as stored (core/page.h SiftChainRef.lo): the payload, zero padded */
#define SIFT_BOUND_TID_MAX	((UINT64_C(1) << 48) - 1)

/*
 * The columns of key_order, at most SIFT_KEY_ORDER_MAX: rows are sorted by all of them, then by address. A bound holds the first SIFT_BOUND_COMPS of them (and the address only when the
 * key has no more), so a key of more columns is shared: partitions that start at one bound are all read.
 */
#define SIFT_KEY_ORDER_MAX	8

/* How a key column of key_order is ordered (the flags of a bound's component): DESC reverses the integers, NULLS FIRST puts the NULLs before them; none, the default of ASC: ascending, NULLs last */
#define SIFT_KEY_DESC			1
#define SIFT_KEY_NULLS_FIRST	2

typedef struct SiftBound
{
	uint8_t		len;			/* bytes of the encoding in use */
	uint8_t		ncomp;			/* its key components, the address not counted */
	uint8_t		bytes[SIFT_BOUND_PAYLOAD];
} SiftBound;

_Static_assert(sizeof(SiftBound) == SIFT_BOUND_BYTES, "a bound is the 40 bytes of a directory reference");

/* [lo, hi): hi with no bytes is unbounded; the empty lo is below every key */
typedef struct SiftRange
{
	SiftBound	lo;
	SiftBound	hi;
} SiftRange;

extern void sift_bound_clear(SiftBound *b);
/*
 * Add a component or the address: false (the bound unchanged) when it holds SIFT_BOUND_COMPS components, or already has an address, or the address is above SIFT_BOUND_TID_MAX.
 * The _key forms put the component as its key column is ordered (SIFT_KEY_*): the integer with its bits reversed for DESC, a tag for NULLS FIRST that sorts before the integers'. The
 * plain forms are those of ASC, NULLS LAST. A bound whose columns are ordered otherwise holds their encodings: sift_bound_comp gives the integer back for ASC only.
 */
extern bool sift_bound_push_i64(SiftBound *b, int64_t v);
extern bool sift_bound_push_null(SiftBound *b);
extern bool sift_bound_push_key_i64(SiftBound *b, int64_t v, unsigned flags);
extern bool sift_bound_push_key_null(SiftBound *b, unsigned flags);
extern bool sift_bound_push_tid(SiftBound *b, uint64_t tid);

/*
 * A bound as stored: the payload padded with zeros to SIFT_BOUND_BYTES, whose memcmp is the order of sift_bound_cmp (a tag is never 0, so a shorter bound sorts before its extensions). Unpacking
 * reads the tags: false for bytes that are no bound (a tag past the components, a bound cut short, bytes after the padding begins).
 */
extern void sift_bound_pack(const SiftBound *b, uint8_t *out);
extern bool sift_bound_unpack(const uint8_t *in, SiftBound *b);

/* Component i of a bound: its integer in *v, or that it is NULL, or that the bound stops before it (it holds the lowest key of the others) */
typedef enum SiftComp
{
	SIFT_COMP_VALUE,
	SIFT_COMP_NULL,
	SIFT_COMP_NONE
} SiftComp;

extern SiftComp sift_bound_comp(const SiftBound *b, uint32_t i, int64_t *v);
/* The address of a bound, when it has one */
extern bool sift_bound_tid(const SiftBound *b, uint64_t *tid);

/* The order of two bounds: below zero, zero, above zero */
extern int	sift_bound_cmp(const SiftBound *a, const SiftBound *b);
/* The leading key components in which two bounds agree: the depth at which they differ (the address is no component) */
extern uint32_t sift_bound_depth(const SiftBound *a, const SiftBound *b);
/* The least bound above every bound that has prefix p as a prefix, in *out; false when there is none (the prefix is the last group of its level: unbounded) */
extern bool sift_bound_prefix_next(const SiftBound *p, SiftBound *out);

extern bool sift_range_unbounded_hi(const SiftRange *r);
extern bool sift_range_overlap(const SiftRange *a, const SiftRange *b);
extern bool sift_range_contains(const SiftRange *r, const SiftBound *key);
/* May the range hold a key that has the prefix */
extern bool sift_range_meets_prefix(const SiftRange *r, const SiftBound *prefix);

/*
 * The heaviest set of the n closed ranges r[i] = [lo, hi] (hi a bound) that are strictly apart (the hi of each below the lo of the next), with the weight of each: their indexes
 * ascending in out[] (room for n), and their number. What a set of segments that tile the key range is, picked among the segments of one directory.
 */
extern uint32_t sift_range_apart(const SiftRange *r, const uint64_t *weight, uint32_t n, uint32_t *out);

/* The keys a reader is confined to; shared: the key has more leaves than a bound holds (sift_range_span) */
typedef struct SiftKeyRange
{
	SiftRange	keys;
	bool		shared;
} SiftKeyRange;

/* Do two readers confined to these keys read the same partitions? */
extern bool sift_key_range_equal(const SiftKeyRange *a, const SiftKeyRange *b);

/*
 * The partitions of a tiling that may hold keys of the range [keys->lo, keys->hi), as the contiguous span [*first, *last) of the n partitions whose lower bounds, ascending, at(refs, i)
 * gives. It is empty (first == last) only when the range is empty or the first partition starts above it, which a tiling that begins with the empty bound never does. shared: the key has
 * more leaves than a bound holds, so a partition may hold rows at the lower bound of the next and the partitions that start at the same bound are all in the span.
 */
typedef const SiftBound *(*SiftBoundAt) (const void *refs, uint32_t i);
extern void sift_range_span(const void *refs, SiftBoundAt at, uint32_t n, const SiftRange *keys, bool shared, uint32_t *first, uint32_t *last);
/* The range of the keys of ncomp components and an address that have the prefix */
extern void sift_prefix_range(const SiftBound *prefix, uint32_t ncomp, SiftRange *out);

#endif							/* SIFT_RANGE_H */
