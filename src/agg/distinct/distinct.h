/*
 * agg/distinct/distinct.h - exact COUNT(DISTINCT) of any width in SiftAggV6 (EXT-23): text, varchar, bytea, uuid, numeric and tuples of columns, in one hash set of exact key bytes, with no 64-bit cap and
 * no hash standing for a value (two keys are equal only when their bytes are).
 *
 * A key is the bytes of the value, or of the columns of a COUNT(DISTINCT (a, b, ..)) row encoded by SiftDistinctKey so that two rows give the same bytes only when every column is equal (a NULL equals
 * a NULL, as in the comparison of two rows). The set is an open-addressing table over a chunked bump arena that holds the key bytes once; the slot of a key (the group, and the aggregate when a
 * query has several COUNT(DISTINCT)) is part of its identity and has its own count. The bytes the set holds are counted exactly (arena, table, counts) and held to budget_bytes: when the next insert
 * would pass it, the keys of all but one hash partition (sift_spill_nparts, 16..256) leave for temporary files and the rows of those partitions go straight to them; a second overflow sends the
 * resident partition away too. sift_distinct_finish reads each partition back through a set of its own (a different hash at each level, so that a partition that is still too large splits again), and
 * adds its counts: the answer equals that of PostgreSQL's sort-and-compare whatever the budget. Past SIFT_DISTINCT_MAX_LEVEL levels a set stops spilling (its keys would have to collide in six
 * independent hashes).
 */
#ifndef SIFT_AGG_V6_DISTINCT_H
#define SIFT_AGG_V6_DISTINCT_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/batch.h"
#include "agg/bytes.h"

#define SIFT_DISTINCT_MAX_LEVEL		6

typedef struct SiftDistinctStats
{
	uint64		adds;			/* rows offered */
	uint64		spilled_records;
	uint64		spilled_bytes;
	uint32		passes;			/* partitions read back */
	uint32		max_level;		/* 0: no spill; 1: one level of partitions; more: a partition split again */
	uint64		peak_bytes;		/* the most bytes any set held at once */
} SiftDistinctStats;

typedef struct SiftDistinct SiftDistinct;

/* expect_bytes: what the planner expects the keys to need in all (0: unknown), to choose the number of partitions */
extern SiftDistinct *sift_distinct_create(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes);

/*
 * A grouped set: the groups of a GROUP BY are too many to give each a slot, and a group that leaves the table of the aggregate for a temporary file has no stable number. A key is then the bytes
 * of its group (the prefix, which the caller encodes as it likes) and of its value, built with sift_dkey_group and the sift_dkey_* of the value, and the slot is the number of the aggregate in
 * the query (COUNT(DISTINCT a), COUNT(DISTINCT (b, c)) in one statement are slot 0 and 1). The set keeps no count: sift_distinct_finish hands emit the count of each (slot, prefix) as it counts the
 * keys of the table and of each partition it reads back, in pieces (the same prefix may come again from another partition): the counts of a group are the sum of its pieces, the answer that
 * sorting every (group, value) would give. emit may add to a table that spills, which is why a piece goes out as soon as a few hundred groups have one.
 */
typedef void (*SiftDistinctGroupEmit) (void *arg, uint32 slot, const uint8 *prefix, uint32 plen, uint64 count);

extern SiftDistinct *sift_distinct_create_grouped(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes, SiftDistinctGroupEmit emit, void *emit_arg);
extern void sift_distinct_add(SiftDistinct *d, uint32 slot, const void *key, uint32 len);
/*
 * The same with the hash of the key given by the caller, which must give the same hash to the same bytes in every add to the set (and never mix with sift_distinct_add in one set): the owner of
 * a partition of records that carry the hash of their key adds them without hashing them again. The bits of the hash that route a key to a partition are not the ones the table and the spill partitions use.
 */
extern void sift_distinct_add_prehashed(SiftDistinct *d, uint32 slot, const void *key, uint32 len, uint64 hash);
extern void sift_distinct_add_u64(SiftDistinct *d, uint32 slot, uint64 v);
/* Empties a set that was never finished, or one that was, to hold the keys of another partition: its table and counts are kept for the next ones */
extern void sift_distinct_reset(SiftDistinct *d);
/* Resolves the partitions that went to temporary files; the counts are final after it, and no row can be added */
extern void sift_distinct_finish(SiftDistinct *d);
extern uint64 sift_distinct_count(const SiftDistinct *d, uint32 slot);
extern uint32 sift_distinct_nslots(const SiftDistinct *d);
extern uint64 sift_distinct_bytes(const SiftDistinct *d);
extern const SiftDistinctStats *sift_distinct_stats(const SiftDistinct *d);
extern void sift_distinct_free(SiftDistinct *d);

/* The key of a row of several columns */
typedef SiftBytes SiftDistinctKey;

extern void sift_dkey_init(SiftDistinctKey *k, MemoryContext cx);
extern void sift_dkey_reset(SiftDistinctKey *k);
extern void sift_dkey_free(SiftDistinctKey *k);
extern void sift_dkey_null(SiftDistinctKey *k);
extern void sift_dkey_i64(SiftDistinctKey *k, int64 v);
/* -0 equals 0 and every NaN equals every NaN, as float8 does in a comparison */
extern void sift_dkey_f64(SiftDistinctKey *k, double v);
extern void sift_dkey_bytes(SiftDistinctKey *k, const void *p, uint32 len);
extern void sift_dkey_uuid(SiftDistinctKey *k, const SiftUuid16 *u);
/* Starts the key of a grouped set: the prefix of the group (the key is cleared first) */
extern void sift_dkey_group(SiftDistinctKey *k, const void *prefix, uint32 plen);
/* The value of a row of a vector (SVEC_I32, SVEC_I64, SVEC_NUM64, SVEC_BOOL, SVEC_F64, SVEC_UUID, SVEC_STRVIEW, SVEC_DICT32 whose dict is a SiftDictSrc; slab as for sift_distinct_add_str); a NULL is sift_dkey_null */
extern void sift_dkey_vec(SiftDistinctKey *k, const SiftVec *v, uint32 row, const char *slab);
extern void sift_distinct_add_dkey(SiftDistinct *d, uint32 slot, const SiftDistinctKey *k);
/* Every key the table holds (a set that never spilled holds them all) */
typedef void (*SiftDistinctVisit) (void *arg, uint32 slot, const void *key, uint32 len);
extern void sift_distinct_foreach(const SiftDistinct *d, SiftDistinctVisit visit, void *arg);
/* The keys of the slots whose bit is set in slots, each distinct key once, to visit as the set is finished (the table's, then those of each partition read back): the values a SUM or AVG adds */
extern void sift_distinct_visit_values(SiftDistinct *d, uint32 slots, SiftDistinctVisit visit, void *arg);
/* The same with the hash each key was added with */
typedef void (*SiftDistinctVisitHashed) (void *arg, uint32 slot, const void *key, uint32 len, uint64 hash);
extern void sift_distinct_foreach_hashed(const SiftDistinct *d, SiftDistinctVisitHashed visit, void *arg);

/*
 * The rows of a batch: every active row whose value is not NULL goes to the slot slot_base + gid[row] (gid NULL: slot_base). COUNT(DISTINCT x) does not count NULLs. slab: the base the offsets of the
 * long strings of a string vector are from (NULL when they are pointers).
 */
extern void sift_distinct_add_i64(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v);
extern void sift_distinct_add_i32(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v);
extern void sift_distinct_add_uuid(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v);
extern void sift_distinct_add_str(SiftDistinct *d, const SiftBatchSel *bs, const uint32 *gid, uint32 slot_base, const SiftVec *v, const char *slab);

#endif							/* SIFT_AGG_V6_DISTINCT_H */
