/*
 * agg/v6/spill.h - the partitioned spill of the operators of SiftAggV6 that hold more than their slice of work_mem (the exact COUNT(DISTINCT) of v6_distinct.h and the Grace hash join of
 * v6_grace_join.h): a set of radix partitions, each a temporary file (BufFile) that is created at its first record and closed as soon as its pass is done. A record is a 64-bit tag, a length and
 * the bytes; the operator decides what the tag means (a hash, flags). Records come back in the order they were written.
 */
#ifndef SIFT_AGG_V6_SPILL_H
#define SIFT_AGG_V6_SPILL_H

#include "core/host.h"

#define SIFT_SPILL_MIN_PARTS	16
#define SIFT_SPILL_MAX_PARTS	256

typedef struct SiftSpillSet SiftSpillSet;

/* A power of two between SIFT_SPILL_MIN_PARTS and SIFT_SPILL_MAX_PARTS from the bytes the operator expects to spill and the budget one partition is read back in */
extern uint32 sift_spill_nparts(uint64 expect_bytes, uint64 budget_bytes);

extern SiftSpillSet *sift_spill_create(MemoryContext cx, uint32 nparts);
extern uint32 sift_spill_nparts_of(const SiftSpillSet *s);
extern void sift_spill_put(SiftSpillSet *s, uint32 part, uint64 tag, const void *bytes, uint32 len);
/* The partition is complete: the next sift_spill_get reads it from its first record */
extern void sift_spill_rewind(SiftSpillSet *s, uint32 part);
/* The next record of the partition into *buf (grown as needed: *cap is its size), false after the last */
extern bool sift_spill_get(SiftSpillSet *s, uint32 part, uint64 *tag, uint8 **buf, uint32 *cap, uint32 *len);
extern uint64 sift_spill_part_records(const SiftSpillSet *s, uint32 part);
extern uint64 sift_spill_part_bytes(const SiftSpillSet *s, uint32 part);
/* Closes the file of the partition: its records are gone */
extern void sift_spill_drop(SiftSpillSet *s, uint32 part);
extern uint64 sift_spill_total_records(const SiftSpillSet *s);
extern uint64 sift_spill_total_bytes(const SiftSpillSet *s);
extern void sift_spill_free(SiftSpillSet *s);

/* The partition of a 64-bit hash among nparts (a power of two): its high bits, the low ones index the hash tables */
static inline uint32
sift_spill_part_of(uint64 hash, uint32 nparts)
{
	return (uint32) (hash >> 32) & (nparts - 1);
}

#endif							/* SIFT_AGG_V6_SPILL_H */
