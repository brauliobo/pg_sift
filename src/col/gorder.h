/*
 * col/gorder.h - the global order of a leaf in a segment: every row of the segment with a value, sorted by
 * (key, heap address), as chunks of SIFT_CGORD_ROWS entries, then the rows without a value by address. ORDER BY .. LIMIT n reads the first n entries of the segment,
 * whatever the number of its units, where the sorted run of a unit (col/runs.h) opens every unit. The entries name heap
 * rows, not rows of the segment: the scan that reads them has the heap say whether a row is live and what it holds. Portable (core/host.h only).
 *
 * Block (little endian): header {u32 magic, u32 version, u32 entries, u32 values, u32 chunks, u32 chunk rows, u32 0, u32 0}, a directory of 24 bytes per
 * chunk {i64 first key, u32 offset of the chunk from the start of the block, u32 bytes of its keys, u32 bytes of its addresses, u32 0}, the chunks (the
 * keys and then the addresses as two codec blobs, each padded to 8), padded to 8. Entry i < values has the key of the chunk and an address; an entry
 * from values on has no key (it is a NULL) and keys 0.
 *
 * Version 2, a block grouped by another leaf (option order_array_group): the entries are sorted by (group, key, address) and the NULL rows by (group, address); rows with no
 * group are not in it. After the chunks come the groups, 24 bytes each {i64 group, u32 first value entry, u32 value entries, u32 first NULL entry, u32 NULL entries} ascending by
 * group; header words 6 and 7 are their number and the group leaf + 1. The keys of a group ascend, so a scan that pins the group reads its entries only, as a b-tree
 * on (group, key) would; a scan that does not pin it cannot use the block.
 */
#ifndef SIFT_COL_GORDER_H
#define SIFT_COL_GORDER_H

#include "core/host.h"

#include "col/enc.h"

#define SIFT_CGORD_MAGIC	0x44474F43		/* "COGD" */
#define SIFT_CGORD_VERSION	1
#define SIFT_CGORD_VERSION_GROUP	2
#define SIFT_CGORD_GRPENT	24
#define SIFT_CGORD_ROWS		1024
#define SIFT_CGORD_HDR		32
#define SIFT_CGORD_DIRENT	24
#define SIFT_CGORD_MEM_ROWS	4096		/* rows of the write buffers a scan sorts for the order, at least (sift_col_gorder_open) */
#define SIFT_CGORD_CAP		(SIFT_CGORD_ROWS + 8)		/* slots of the arrays sift_cgord_decode fills */

typedef struct SiftCGOrdHdr
{
	uint32		entries;
	uint32		values;
	uint32		chunks;
	uint32		crows;
	uint32		ngroups;		/* version 2: the groups of the end of the block, and the leaf they are the values of; else 0 and -1 */
	int			gcol;
} SiftCGOrdHdr;

typedef struct SiftCGOrdGrp
{
	int64		group;
	uint32		vstart;
	uint32		vcount;
	uint32		nstart;
	uint32		ncount;
} SiftCGOrdGrp;

typedef struct SiftCGOrdDir
{
	int64		first;			/* the key of the chunk's first entry (0 for a chunk of NULL rows) */
	uint32		off;
	uint32		klen;
	uint32		alen;
	uint32		pad;
} SiftCGOrdDir;

/*
 * Appends the block of the key (key[i], tid[i]) of the rows with a value and the addresses ntid of those without (the lists are left as they are); the addresses
 * are heap addresses (block << 16 | offset, offsets below 512). The block is a multiple of 8 bytes.
 */
extern void sift_cgord_build(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, StringInfo out);
/* The same grouped by the leaf gcol: group[i] is the group of entry i of key / tid, ngroup[i] that of ntid[i]; every entry has one */
extern void sift_cgord_build_grouped(int64 *key, uint64 *tid, int64 *group, uint32 nvals, uint64 *ntid, int64 *ngroup, uint32 nnull, int gcol, StringInfo out);

/* Does the leaf get a block: a plain integer, date or timestamp attribute of the top level whose key is the integer itself (what the scan reads back as the value) */
extern bool sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d);

/* Parses the header of a block of len bytes; false when it is malformed (magic, counts, directory out of the block) */
extern bool sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h);

/* The groups of a version 2 block of len bytes whose header was parsed, false when they are out of the block, not ascending, or do not tile the entries */
extern bool sift_cgord_groups(const uint8 *blk, const SiftCGOrdHdr *h, uint64 len, SiftCGOrdGrp *out);

/* The directory entry of chunk c of a block whose header was parsed, false when the entry is out of the block or its offsets are */
extern bool sift_cgord_dirent(const uint8 *dir, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d);

/* Entries of chunk c: crows, or what is left in the last one */
static inline uint32
sift_cgord_chunk_rows(const SiftCGOrdHdr *h, uint32 c)
{
	return c + 1 < h->chunks ? h->crows : h->entries - c * h->crows;
}

/*
 * Decodes the first w entries (all m when w >= m) of a chunk of m from its two blobs (kb, klen) and (ab, alen) into key[0..w) and addr[0..w) (heap addresses); both need
 * SIFT_CGORD_CAP slots. A chunk of NULL rows gives 0 for every key. False when the blobs are malformed.
 */
extern bool sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 m, uint32 w, int64 *key, uint64 *addr);

#endif
