/*
 * agg/sip.h - sideways information passing: the join keys of the build side of an inner or semi join, kept as a filter that the scan of the other
 * side asks before it decodes a payload column. A filter holds the range of the keys, the keys themselves while there are at most
 * SIFT_SIP_EXACT_MAX distinct ones (no false positive, and a zone is told apart by a bisection), and a split-block Bloom filter (core/sbbf.h, the one
 * of the skipping indexes) sized by the distinct keys expected. It is asked at two levels: a granule by the minimum and maximum of its zone
 * (sift_sip_probe_granule: a granule whose zone holds none of the keys is not read) and the rows of a batch (sift_sip_probe_vec: the key column is
 * decoded, the rows whose key is not in the filter leave the batch mask, and when none is left no payload column is decoded). A NULL key never joins.
 *
 * A filter is exact in what it keeps, never in what it drops: a key that was inserted always passes (no false negative at any level), another one may
 * pass (the Bloom rate, 0 with the exact keys). A filter that has not been finalized passes everything, so an outer scan that gets ahead of the build
 * side loses only the pruning. When the keys outgrow the Bloom filter (the expected rate above SIFT_SIP_MAX_FPR at the size allowed) the Bloom filter is
 * dropped and the range stays. The size is bounded by Min(SIFT_SIP_MAX_BYTES, work_mem / 8): nothing here is a setting.
 *
 * A filter is one block of memory, its header then its Bloom blocks at a fixed offset, with no pointer: it is valid at any address of any process, so that
 * the builders of a parallel join share one in a shared segment (sift_sip_init_at), and the probes read it there.
 *
 * One builder inserts into a filter; builders that run at the same time each fill their own filter of the same size and merge it into the shared one
 * with sift_sip_merge_atomic (OR of the words, compare-and-swap of the range), which several of them may do at once; the shared filter is finalized
 * by one process after the last merge, and probed after that.
 */
#ifndef SIFT_AGG_SIP_H
#define SIFT_AGG_SIP_H

#include "agg/kern/kern.h"
#include "core/adapt_pipe.h"

#define SIFT_SIP_EXACT_MAX		64
#define SIFT_SIP_MIN_BLOCKS		32				/* 1 KB */
#define SIFT_SIP_MAX_BYTES		(2 * 1024 * 1024)
#define SIFT_SIP_BITS_PER_KEY	12
#define SIFT_SIP_MAX_FPR		0.30

#define SIFT_SIP_FLAG_READY			(UINT32_C(1) << 0)	/* finalized: every other flag is valid */
#define SIFT_SIP_FLAG_EMPTY			(UINT32_C(1) << 1)	/* no key: nothing joins */
#define SIFT_SIP_FLAG_HAS_RANGE		(UINT32_C(1) << 2)	/* min_key and max_key hold the keys (an integer or date key, not a hash only) */
#define SIFT_SIP_FLAG_HAS_EXACT		(UINT32_C(1) << 3)	/* exact_keys is every distinct key, sorted */
#define SIFT_SIP_FLAG_HAS_BLOOM		(UINT32_C(1) << 4)	/* the blocks are probed */
#define SIFT_SIP_FLAG_BLOOM_OFF		(UINT32_C(1) << 5)	/* too many keys for the size allowed: only the range prunes */

typedef struct SiftSipFilter
{
	uint32		state_flags;	/* atomic */
	SiftVecType key_type;		/* SVEC_I32 or SVEC_I64: the keys are sign-extended; a hash-only filter takes insert_hash */
	uint16		n_exact;
	bool		exact_open;		/* every key so far is in exact_keys */
	bool		hash_only;		/* a hash was inserted: the keys have no range and no exact set */
	int64		min_key;		/* INT64_MAX and INT64_MIN until a key is inserted */
	int64		max_key;
	int64		exact_keys[SIFT_SIP_EXACT_MAX];
	uint32		n_blocks;		/* a power of 2, SIFT_SIP_MIN_BLOCKS up to the size allowed */
	uint64		n_inserted;		/* the rows of the build side that were inserted, distinct or not */
	/* the n_blocks Bloom blocks follow, at SIFT_SIP_HDR_BYTES */
} SiftSipFilter;

#define SIFT_SIP_HDR_BYTES		TYPEALIGN(SIFT_SBBF_BLOCK_BYTES, sizeof(SiftSipFilter))

static inline SiftSbbfBlock *
sift_sip_blocks(const SiftSipFilter *sip)
{
	return (SiftSbbfBlock *) ((char *) sip + SIFT_SIP_HDR_BYTES);
}

/* What a scan did with a filter, for EXPLAIN; the caller owns it and may pass NULL */
typedef struct SiftSipStats
{
	uint64		granules_probed;
	uint64		granules_pruned;
	uint64		rows_in;
	uint64		rows_out;
} SiftSipStats;

/* The most bytes of blocks a filter may take: Min(SIFT_SIP_MAX_BYTES, work_mem / 8) */
extern uint64 sift_sip_budget_bytes(void);

/* The blocks of a filter for about est_ndv distinct keys within max_bytes: a power of 2, at least SIFT_SIP_MIN_BLOCKS */
extern uint32 sift_sip_blocks_for(uint64 est_ndv, uint64 max_bytes);
/* The bytes of a filter of n_blocks blocks, and an empty one laid out in them at raw (SIFT_SBBF_BLOCK_BYTES aligned): in a shared segment, say */
extern Size sift_sip_size(uint32 n_blocks);
extern SiftSipFilter *sift_sip_init_at(void *raw, SiftVecType key_type, uint32 n_blocks);
/* An empty filter for about est_ndv distinct keys, its blocks within max_bytes (a power of 2 of blocks, at least SIFT_SIP_MIN_BLOCKS); all in cxt */
extern SiftSipFilter *sift_sip_create(MemoryContext cxt, SiftVecType key_type, uint64 est_ndv, uint64 max_bytes);
extern void sift_sip_free(SiftSipFilter *sip);

/* The build side: keys of the filter's type, the keys of a column (rows of mask, NULL = all; a NULL key is not inserted), or a hash of a key of a type with no range */
extern void sift_sip_insert_i64(SiftSipFilter *sip, int64 key);
/* The same for a caller that has the hash of the key already (sift_aggk6_hash1): a build that hashes the key for a table makes no second hash */
extern void sift_sip_insert_i64_hashed(SiftSipFilter *sip, int64 key, uint64 hash);
extern void sift_sip_insert_hash(SiftSipFilter *sip, uint64 hash);
extern void sift_sip_insert_vec(SiftSipFilter *sip, const SiftVec *keys, const uint64 *mask);

/* Ends the build: the flags (READY last, with release order) */
extern void sift_sip_finalize(SiftSipFilter *sip);
/* Adds the keys of src (the same size, not finalized) to dst, while others may be adding theirs: the exact keys are dropped, the range and the Bloom bits are merged */
extern void sift_sip_merge_atomic(SiftSipFilter *dst, const SiftSipFilter *src);

static inline uint32
sift_sip_flags(const SiftSipFilter *sip)
{
	return __atomic_load_n(&sip->state_flags, __ATOMIC_ACQUIRE);
}

/* No key can join: the scan of the other side returns no row without a read */
static inline bool
sift_sip_is_empty(const SiftSipFilter *sip)
{
	const uint32 f = sift_sip_flags(sip);

	return (f & SIFT_SIP_FLAG_READY) && (f & SIFT_SIP_FLAG_EMPTY);
}

/* Level 1 and 2: false when no key of the filter can be in a granule or a part whose key column lies in [zmin, zmax] (always true before the filter is ready) */
extern bool sift_sip_probe_granule(const SiftSipFilter *sip, int64 zmin, int64 zmax, SiftSipStats *st);

/*
 * Level 3: io_mask (the rows of the batch, the bits >= keys->len zero) keeps the rows whose key may join; keys is dense, of the filter's type, its NULL
 * rows leave the mask. Returns the rows left.
 */
extern uint16 sift_sip_probe_vec(const SiftSipFilter *sip, const SiftVec *keys, uint64 *io_mask, SiftSipStats *st);

/* The hash-only build side and probe: the Bloom filter alone (a key of a type with no range, a composite key hashed by the caller); hashes[r] is the hash of row r */
extern uint16 sift_sip_probe_hashes(const SiftSipFilter *sip, const uint64 *hashes, uint64 *io_mask, uint16 n, SiftSipStats *st);

/*
 * A scan that probes a filter whose keys are most of its rows pays the hash and the load of the Bloom blocks of every row for nothing. The probe state of
 * one filter in one scan (or one participant of it) measures the share of the rows that pass, and stops the batch probe when it is above
 * SIFT_SIP_DISABLE_HIT_RATE after SIFT_SIP_ADAPT_WARMUP_BATCHES batches. The probe of a granule by its zone (sift_sip_probe_granule) is never stopped: two
 * compares a granule. A stopped probe looks at one granule in SIFT_SIP_REPROBE_GRANULES and starts again when the rows that pass it are below
 * SIFT_SIP_REENABLE_HIT_RATE, a skew that moved. The state has no atomic and no lock: it is the scan's own.
 */
#define SIFT_SIP_ADAPT_WARMUP_BATCHES	8
#define SIFT_SIP_DISABLE_HIT_RATE		0.70
#define SIFT_SIP_REENABLE_HIT_RATE		0.55
#define SIFT_SIP_REPROBE_GRANULES		64
#define SIFT_SIP_EWMA_ALPHA				0.25

typedef struct SiftSipProbeState
{
	SiftSipStats stats;
	double		ewma_hit_rate;	/* the share of the rows that passed, in [0, 1] */
	uint32		batches_probed;
	uint32		granules_skipped_probe;	/* the granules run with the probe stopped */
	uint32		auto_disables;
	uint32		auto_reenables;
	uint32		reprobe_in;		/* the rows of the granule that looks again, and the ones that passed */
	uint32		reprobe_out;
	bool		vec_disabled;
	bool		reprobing;
	SiftAdaptOp *op;			/* the conjunct of the scan's pipeline that stands for the probe, told when it is stopped; NULL if none */
} SiftSipProbeState;

/* The probe of a batch under the state: the rows left (all, with nothing looked at, while the probe is stopped) */
extern uint16 sift_sip_probe_vec_adapt(const SiftSipFilter *sip, SiftSipProbeState *ps, const SiftVec *keys, uint64 *io_mask);
/* The end of a granule: counts the granules a stopped probe skipped, and decides on the granule it looked at again */
extern void sift_sip_adapt_granule_end(SiftSipProbeState *ps);

/* The rate at which a key that is not in the filter passes it, measured from the bits set in the blocks: 0 for an exact filter, 1 without blocks */
extern double sift_sip_fpr(const SiftSipFilter *sip);

#endif							/* SIFT_AGG_SIP_H */
