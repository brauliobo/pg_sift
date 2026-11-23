/*
 * core/sbbf.h - the split-block Bloom filter (Putze, Sanders, Singler): the one implementation behind the runtime join filter of agg/sip.h and the
 * skipping indexes of the column format.
 *
 * A filter is an array of nblocks blocks of 256 bits (eight 32-bit words). A key of 64-bit hash h picks one block by the high half
 * (sift_sbbf_block_of, a multiply-shift, so nblocks is any number, not a power of 2) and sets, in each of the eight words of it, the bit
 * ((uint32) h * SIFT_SBBF_SALT[j]) >> 27: one cache line touched, no false negative, a false positive when the eight bits of another key's mask
 * are all set. sift_sbbf_fpr gives the expected rate.
 *
 * Every SIMD level implements the same two operations over a hash, insert and the probe of a word of 64 hashes under a row mask, and returns the
 * bits of the scalar level (the specification): the batch probes are made of the word probe, and the kernel slots sbbf_probe_* of agg/kern6.h call
 * them. sift_sbbf_ops_for(level) never returns NULL (a level the CPU lacks falls back downwards). sift_sbbf_insert_atomic is for builders that share
 * a filter: the words are set by atomic OR, a probe that runs at the same time sees a subset of the bits of the keys being inserted.
 *
 * No allocation, no ereport; the blocks are the caller's, of any alignment (the payload of a skipping index is not aligned): the vector levels load and
 * store unaligned, so every function takes void pointers. sift_sbbf_insert_atomic alone needs the blocks 4-byte aligned.
 */
#ifndef SIFT_SBBF_H
#define SIFT_SBBF_H

#include "core/cpu.h"
#include "core/host.h"

#define SIFT_SBBF_BLOCK_BYTES	32
#define SIFT_SBBF_WORD_ROWS		64
#define SIFT_SBBF_BPK			10		/* bits for each distinct key: a false positive rate of about 1.2% */
#define SIFT_SBBF_BPK_LEAN		8		/* the form for a filter that does not fit its budget at SIFT_SBBF_BPK: a fifth smaller, a false positive rate of about 2.4% */

typedef struct __attribute__((aligned(32))) SiftSbbfBlock
{
	uint32		w[8];
} SiftSbbfBlock;

_Static_assert(sizeof(SiftSbbfBlock) == SIFT_SBBF_BLOCK_BYTES, "a split block is 256 bits");

/* Odd multiplicative salts of the eight words (the ones of Parquet's split-block filter) */
static const uint32 SIFT_SBBF_SALT[8] __attribute__((aligned(32))) = {
	0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
	0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U
};

typedef struct SiftSbbfOps
{
	/* sets the eight bits of the hash in its block */
	void		(*insert) (void *blocks, uint32 nblocks, uint64 hash);
	/* the bits of mask whose hash (hashes[i] for bit i, 64 readable entries) is found in the filter: a subset of mask */
	uint64		(*probe_word) (const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 mask);
} SiftSbbfOps;

extern const SiftSbbfOps sift_sbbf_ops_scalar;
extern const SiftSbbfOps sift_sbbf_ops_avx2;
extern const SiftSbbfOps sift_sbbf_ops_avx512;
extern const SiftSbbfOps sift_sbbf_ops_neon;
extern const SiftSbbfOps *sift_sbbf_ops_for(SiftSimdLevel level);

/* The block of a hash */
static inline uint32
sift_sbbf_block_of(uint64 hash, uint32 nblocks)
{
	return (uint32) (((uint64) (uint32) (hash >> 32) * nblocks) >> 32);
}

/* The bit of the word j of a hash, as a mask of the word */
static inline uint32
sift_sbbf_bit(uint64 hash, int j)
{
	return UINT32_C(1) << (((uint32) hash * SIFT_SBBF_SALT[j]) >> 27);
}

/* The 64 bit hash of a key: the integers and the bytes (what the skipping indexes of the column format store) */
extern uint64 sift_sbbf_hash_i64(int64 v);
extern uint64 sift_sbbf_hash_bytes(const void *p, uint32 len);

/* The scalar insert and probe of one hash, the specification of every level */
extern void sift_sbbf_insert_scalar(void *blocks, uint32 nblocks, uint64 hash);
extern bool sift_sbbf_probe_scalar(const void *blocks, uint32 nblocks, uint64 hash);

/* The insert with an atomic OR of each word, for the builders of one filter that run at the same time */
extern void sift_sbbf_insert_atomic(void *blocks, uint32 nblocks, uint64 hash);
/* The insert at the active level of sift.simd */
extern void sift_sbbf_insert(void *blocks, uint32 nblocks, uint64 hash);
/* Whether a hash may be in the filter, at the active level */
extern bool sift_sbbf_probe(const void *blocks, uint32 nblocks, uint64 hash);

/*
 * hashes[0..n) tested against the filter: bit i of io_mask (ceil(n / 64) words, the bits >= n zero) stays set when it was and the hash of row i is
 * in the filter, the others clear; a clear bit is not read. Returns the number of bits left. At the active level, or at the given one.
 */
extern uint16 sift_sbbf_probe_batch_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n);
extern uint16 sift_sbbf_probe_batch(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n);

/* Some / every one of the hashes[0..nhashes) is in the filter (false / true for none) */
extern bool sift_sbbf_probe_any_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes);
extern bool sift_sbbf_probe_all_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes);

/* Some / every one of the hashes[0..n) is in the filter, at the active level */
extern bool sift_sbbf_probe_any(const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 n);
extern bool sift_sbbf_probe_all(const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 n);

/* The blocks that hold ndistinct keys at bits_per_key bits each, at least one */
extern uint32 sift_sbbf_nblocks(uint32 ndistinct, uint32 bits_per_key);
extern uint32 sift_sbbf_blocks_for(uint64 n_keys, uint32 bits_per_key);
/* The expected false positive rate of a filter of nblocks blocks after n_keys distinct keys, in [0, 1] */
extern double sift_sbbf_fpr(uint64 n_keys, uint32 nblocks);

#endif							/* SIFT_SBBF_H */
