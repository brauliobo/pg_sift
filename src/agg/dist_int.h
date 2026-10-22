/*
 * agg/dist_int.h - count(DISTINCT x) per group (agg/dist.c)
 */
#ifndef SIFT_AGG_DIST_INT_H
#define SIFT_AGG_DIST_INT_H

#include "agg/agg_fwd_int.h"

/* open addressing set of (group, value id) pairs; a slot holds the pair with bit 63 set, 0 is empty */
typedef struct Dist Dist;

typedef struct DSet
{
	uint64	   *slot;
	int64	   *cnt;			/* counted sets: the times each pair was added, per slot */
	uint64		n;
	uint64		cap;
	bool		counted;
} DSet;

/*
 * The distinct values of count(DISTINCT x) per group, x an integer, or the id (dense, from 0) of a dictionary or numeric value. Three
 * structures, each bounded by hash_mem (work_mem times hash_mem_multiplier); the aggregate moves to the next one when its structure
 * would outgrow that, and nothing is lost on the way:
 *
 *   DD_BITS  a bitmap per group, in chunks of 4096 bits allocated when a value falls in them, under a directory of chunk pointers that
 *            covers the range of values seen so far (the same for every group). A value costs one bit when the domain is dense (customer
 *            ids 1 to 2 million by 36 months is 9 MB); a test-and-set tells whether it is new. The range is limited to 2^28 values.
 *   DD_HASH  an open addressing set of (group, value) pairs, sized to hash_mem; values are inserted a batch at a time, the slots
 *            prefetched first.
 *   DD_SORT  every (group, value) pair goes through a tuplesort (work_mem, spilling to disk as the normal plan's sort does), and the
 *            groups' counts are taken from the sorted runs when the scan ends. A group is named by its sort id (the groups are
 *            renumbered while the scan runs), the number of its first value.
 *
 * dist_add() returns true when the pair is new and the count of the group is to be incremented at once; in DD_SORT it cannot know and
 * returns false, and dist_finish() sets the counts of all the groups.
 */
#define DB_SHIFT		12
#define DB_WORDS		(1 << (DB_SHIFT - 6))
#define DB_NCD_MIN		64
#define DB_NCD_MAX		65536
#define DB_DENSE		32				/* bits from which a chunk goes to the leader as it is rather than as pairs */
#define DB_PREFETCH		(2 << 20)		/* bytes of bitmaps from which the cache no longer holds them */
#define DB_BATCH		32				/* values whose bits are prefetched together */
#define DB_ARENA		255				/* chunks per arena block; the first chunk of a block links to the previous block */

typedef enum DistMode
{
	DD_BITS,
	DD_HASH,
	DD_SORT
} DistMode;

typedef uint64 *DistChunk;

struct Dist
{
	DistMode	mode;
	bool		sgn;			/* the values are int32 */
	bool		done;			/* DD_SORT: sorted and counted */
	uint64		budget;			/* bytes the bitmaps or the set may take */
	/* DD_BITS */
	int64		cbase;			/* chunk number of directory entry 0 */
	uint32		ncd;			/* entries of a directory (0: nothing yet) */
	uint32		ngd;			/* groups with room in gd */
	DistChunk **gd;				/* per group: its directory */
	uint64		bytes;
	uint64	   *arena;
	uint32		arena_left;
	/* DD_HASH */
	DSet		ds;
	uint64		maxn;
	/* DD_SORT */
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
	uint32		nsid;
	uint32		nsidcap;
	uint32	   *sid;			/* per group: its sort id + 1 */
	uint32		sidgroups;
	uint64	   *cnt;			/* per sort id: distinct values, once sorted */
};

static inline uint64
dset_hash(uint64 x)
{
	x *= UINT64CONST(0x9E3779B97F4A7C15);
	return x ^ (x >> 32);
}

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* dist.c */
void agg_pair_sort(int natts, int flags, Tuplesortstate **sort, TupleTableSlot **put, TupleTableSlot **get);
void dset_grow(AggExec *ex, DSet *s, const uint32 *remap);
bool dset_add(AggExec *ex, DSet *s, uint32 g, uint32 v);
Dist *dist_new(AggExec *ex, bool sgn);
bool dist_add(AggExec *ex, Dist *d, uint32 g, uint32 v);
uint64 dist_or_words(AggExec *ex, Dist *d, uint32 g, int64 wbase, const uint8 *words, uint64 nwords);
uint64 dist_or_chunk(AggExec *ex, Dist *d, uint32 g, int64 xc, const uint64 *bits);
void dist_regroup(AggExec *ex, Dist *d, const uint32 *remap, uint32 oldcap, uint32 newcap);
void dist_update(AggExec *ex, int ai, const uint64 *ok, int nsel, const uint32 *v);
void dist_finish_all(AggExec *ex);
void dist_end_all(AggExec *ex);
void dist_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out);
void dset_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out);

#pragma GCC visibility pop

#endif
