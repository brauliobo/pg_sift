/*
 * agg/join/grace.h - the partitioned (Grace) hash join of SiftAggV6: an inner, left, semi or anti equi-join whose build side does not fit in half of work_mem is not refused back to
 * PostgreSQL's row-at-a-time join but split into radix partitions (16..256, v6_spill.h) that go through BufFile temporary files and are joined one at a time within the budget.
 *
 * The build side is read into a hash table of exact key bytes while it is under budget (work_mem / 2: the other half is the probe side's and the output's). At the first overflow the rows of every
 * partition but one go to their files and the table keeps that partition (the hybrid hash join: the probe rows of the resident partition are joined as they arrive, the others are written to the probe
 * files of their partitions); a second overflow sends the resident partition away too. sift_gjoin_next then joins each pair of files: the build file is loaded into the table when it fits; when it does not,
 * both files are split again by a different hash (up to SIFT_GJOIN_MAX_LEVEL levels); a partition that still does not fit (more rows with one key than the budget holds) is joined in chunks of the build
 * file, one scan of the probe file for each chunk, with a bit for each probe row telling whether it has matched (the bits are in a temporary file, a window of them in memory). The rows are the same as PostgreSQL's hash join whatever the budget; their order is not.
 *
 * A key is the bytes the caller makes of the join columns (equal bytes, equal keys); a NULL key never matches: the build row is dropped, an inner or semi probe row is dropped, a left or anti probe row is
 * emitted with no match (NOT EXISTS, not NOT IN). The join takes equi conditions only: a residual qual of the join is the caller's to apply to the pairs (a left join with one is PostgreSQL's).
 */
#ifndef SIFT_AGG_V6_GRACE_JOIN_H
#define SIFT_AGG_V6_GRACE_JOIN_H

#include "core/host.h"

#define SIFT_GJOIN_MAX_LEVEL	4

typedef enum SiftJoinType
{
	SGJ_INNER,
	SGJ_LEFT,
	SGJ_SEMI,
	SGJ_ANTI
} SiftJoinType;

/* build is NULL for the probe row of a left join that matched nothing, and for the rows of an anti join; for a semi join, build is NULL too (the row is emitted once) */
typedef void (*SiftJoinEmit) (void *arg, const uint8 *probe, uint32 probe_len, const uint8 *build, uint32 build_len);

typedef struct SiftGraceStats
{
	uint64		build_rows;
	uint64		probe_rows;
	uint64		spilled_bytes;	/* written to temporary files, both sides */
	uint32		partitions;		/* of the first split */
	uint32		max_level;		/* 0: no spill, 1: one split, more: a partition split again */
	uint32		chunk_passes;	/* scans of a probe file for a chunk of an unsplittable partition */
	uint64		peak_bytes;
} SiftGraceStats;

typedef struct SiftGraceJoin SiftGraceJoin;

/* work_mem_bytes: the budget of the join, half of it for the build side; est_build_bytes: what the planner expects the build side to take (0: unknown) */
extern SiftGraceJoin *sift_gjoin_create(MemoryContext cx, SiftJoinType type, uint64 work_mem_bytes, uint64 est_build_bytes, SiftJoinEmit emit, void *emit_arg);
extern void sift_gjoin_build(SiftGraceJoin *j, const void *key, uint32 klen, bool key_null, const void *payload, uint32 plen);
/* Every build row is in: the probe rows may come */
extern void sift_gjoin_build_done(SiftGraceJoin *j);
extern void sift_gjoin_probe(SiftGraceJoin *j, const void *key, uint32 klen, bool key_null, const void *payload, uint32 plen);
/*
 * Joins the partitions that went to files, one probe row of a partition for each call: the rows are emitted as they are made, so that what the caller does not take is not joined and not kept. True
 * while there is more to join, false when every partition is. Every temporary file is closed when its partition is joined, and at sift_gjoin_free.
 */
extern bool sift_gjoin_next(SiftGraceJoin *j);
extern const SiftGraceStats *sift_gjoin_stats(const SiftGraceJoin *j);
extern void sift_gjoin_free(SiftGraceJoin *j);

#endif							/* SIFT_AGG_V6_GRACE_JOIN_H */
