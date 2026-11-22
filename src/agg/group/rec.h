/*
 * agg/group/rec.h - the records of the groups of a table (agg/group/ht.h) outside the table: a record for the state of a group (its slot, its state
 * block and its wide key) and a record for each value of its count(DISTINCT) set, with the hash of the group at the same place in both, so that a
 * stream of records is partitioned by the bits of one word. The partitions of a parallel merge (agg/par/radix.h) and the files of a table that
 * outgrew its memory (agg/group/spill.h) hold the same records, and merge them the same way: into a table of the same shape, state by state and
 * value by value, in any order.
 *
 * A record: 8 bytes {len, kind}; for a state the 64 bytes of the slot, the state block (ext_stride bytes) and the wide key bytes; for a value the hash,
 * the 16 bytes of the key, the value and the wide key bytes. len is a multiple of 8.
 */
#ifndef SIFT_AGG_GROUP_REC_H
#define SIFT_AGG_GROUP_REC_H

#include "agg/group/dist.h"
#include "agg/group/ht.h"

/*
 * The state of a group beyond its key: init puts a new group in its empty state, merge adds the state of src (a partial group of another table) into
 * dst; slot aggregates and the state block (NULL when the table has none) are both passed, with the shape (ctx is the caller's). wide_key, ext_stride
 * and ext_dist_off are those of sift_ht_init_ex, of the tables that make the records and the ones that merge them alike. A shape with ext_dist_off set
 * keeps count(DISTINCT) sets, which are shipped as records of values, never as the sets.
 */
typedef struct SiftRadixShape
{
	bool		wide_key;
	uint32		ext_stride;
	uint32		ext_dist_off;
	const void *ctx;
	void		(*init) (const struct SiftRadixShape *sh, SiftAggSlot64 *slot, uint8 *ext);
	void		(*merge) (const struct SiftRadixShape *sh, SiftAggSlot64 *dst, uint8 *dst_ext, const SiftAggSlot64 *src, const uint8 *src_ext);
} SiftRadixShape;

/* The hash of the group of a record, and its length */
static inline uint64
sift_rec_hash(const uint8 *rec)
{
	uint64		h;

	memcpy(&h, rec + 8, sizeof(h));
	return h;
}

static inline uint32
sift_rec_len(const uint8 *rec)
{
	uint32		len;

	memcpy(&len, rec, sizeof(len));
	return len;
}

/* The buffer a record is built in: it grows */
typedef struct SiftRecBuf
{
	uint8	   *p;
	uint32		cap;
} SiftRecBuf;

/* Gets a record of one group (the hash of its group, the record, its length) */
typedef void (*SiftRecPut) (void *arg, uint64 hash, const uint8 *rec, uint32 len);

/* Every group of the table, as its state record and a value record for each value of its set, to put */
extern void sift_rec_flush_ht(const SiftSaltedHT *ht, const SiftRadixShape *shape, SiftRecBuf *buf, SiftRecPut put, void *arg);

/* The table the records are merged into and the bytes of the distinct sets it holds: init it with the shape, then merge every record */
typedef struct SiftRecMerge
{
	const SiftRadixShape *shape;
	SiftSaltedHT *ht;
	uint64		dist_bytes;
} SiftRecMerge;

extern void sift_rec_merge(SiftRecMerge *m, const uint8 *rec);

#endif							/* SIFT_AGG_GROUP_REC_H */
