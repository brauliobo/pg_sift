/*
 * col/point.h - the point directory of a leaf in a segment: an open addressing table with a slot for each
 * distinct key of the leaf (the key of a row, the code of a dictionary leaf), holding the 32-bit fingerprint of the key and either its only row or where the
 * list of its rows is, so that an equality or a short IN list finds its rows (and the units that hold them) with a probe or two, where the zones and
 * small-set summaries only cut the units that cannot hold the value. The table does not say that a row holds the value, only that its fingerprint matches:
 * the scans that use it (the access method of sift_col) have the heap check the condition again. Portable (core/host.h only).
 */
#ifndef SIFT_COL_POINT_H
#define SIFT_COL_POINT_H

#include "col/enc.h"
#include "core/host.h"

#define SIFT_CPOINT_MAGIC	0x4B4C4F43		/* "COLK" */
#define SIFT_CPOINT_VERSION_ROWS	2		/* the frame of a segment whose tables all list rows (minor 14) */
#define SIFT_CPOINT_VERSION_ORD	4			/* the frame of a segment that also has global order blocks (minor 21): the header's last word counts their records, which follow those of the tables */
#define SIFT_CPOINT_VERSION	3			/* the frame of a segment with a table of heap addresses (minor 16): its records carry the mode of the table */
#define SIFT_CPOINT_MODE_ROWS	0		/* record word 5: the slots list the rows of the segment (the other modes of the frame) */
#define SIFT_CPOINT_MODE_ORDER	2		/* ... or the record is the global order block of the leaf (col/gorder.h): chunks, entries, values, 0, words of 8 bytes */
#define SIFT_CPOINT_MODE_TIDS	1		/* ... or the heap addresses of the rows */
#define SIFT_CPOINT_MODE_ZROWS	3		/* ... or the slots list rows as the table of addresses lists them: a list is {u32 count, u32 bytes, the first row then the differences in LEB128} (the values of a list) */
#define SIFT_CPOINT_EXACT	0x100		/* in record word 5 with a mode: the table has ceil(keys * 4 / 3) slots, not a power of two, and a key starts at the position the high 32 bits of its hash scale to (column format minor 33) */
#define SIFT_CPOINT_PACKED	0x200		/* in record word 5 with the mode ROWS or TIDS: the block is a packed table of entries by bucket, not slots; its parameters are in the SiftCPackPar after the records */
#define SIFT_CPOINT_TID_LIST	UINT64CONST(0x8000)	/* in a table of addresses, in the low 16 bits of a slot's payload: the high 32 are the word offset of a list, not a block (no offset number has this bit) */
#define SIFT_CPOINT_TID_MAXOFF	512		/* an offset number of the address codes below this, which every heap page of 8 kB does (MaxHeapTuplesPerPage is 291) */
#define SIFT_CPOINT_EMPTY	UINT64_MAX
#define SIFT_CPOINT_MAX_KEYS	65536		/* an IN list of more keys is answered by the units (a lookup that finds more rows is too, below) */
#define SIFT_CPOINT_MIN_KEYS	64			/* up to this many keys a lookup is made whatever the segment's rows */
#define SIFT_CPOINT_MAX_RANGE	64			/* so is a range of more keys: the zones cut a range of values by their order */
#define SIFT_CPOINT_DENSE_ROWS	4096		/* a list that expects more rows than this and a 64th of the segment's rows reads them for nothing: the units hold them as well; so a value of a list in more is not listed (SIFT_CPOINT_DENSE) */
#define SIFT_CPOINT_DENSE_SHARE	64
#define SIFT_CPOINT_CHUNK_MIN	65			/* a lookup of this many keys or more is read a chunk of keys at a time by a stream (sift_cseg_point_stream_open): a LIMIT looks up the first chunk only */
#define SIFT_CPOINT_CHUNK_FIRST	32			/* keys of the first chunk; each chunk has four times as many as the one before */
#define SIFT_CPOINT_CHUNK_LAST	4096
#define SIFT_CPOINT_CHUNK_ROWS	(UINT32_C(16) << 20)	/* a segment of more rows is looked up whole: one key cannot list more addresses than it has rows (8 bytes each) */
#define SIFT_CPOINT_MAX_ROWS	65536		/* a lookup that finds more rows is not a point lookup: the units answer it */
#define SIFT_CPOINT_SEG_ROWS	(UINT32_C(1) << 30)	/* a segment of this many rows or more has no tables: a slot keeps a list offset in 31 bits */
#define SIFT_CPOINT_LIST		(UINT32_C(1) << 31)	/* in a slot, the low 32 bits are this flag and the word offset of a list of rows, not the only row of the key */
#define SIFT_CPOINT_DENSE	UINT32_C(0x7FFFFFFF)	/* in a slot, the payload of a key too common to list: the units answer it; no row of a segment is this */
#define SIFT_CPOINT_RECLEN		32			/* the bytes of a record of the header */
#define SIFT_CPOINT_PK_BUCKET	64			/* entries a bucket of a packed table holds on average (the directory costs 4 bytes a bucket) */
#define SIFT_CPOINT_PK_RBITS	16			/* bits of the hash a packed table of hashes keeps beyond the bucket (the fingerprint, as the 16 bits of a slot of addresses) */
#define SIFT_CPOINT_PK_RUN		1024		/* a packed table is not built when a bucket would hold more entries than this: the common key is then listed once, in a list */
#define SIFT_CPOINT_PK_PAR		32			/* the bytes of SiftCPackPar */

/*
 * The packed table of a leaf (column format minor 35): the entries (key, row or address) of the leaf, one for each row with a value, sorted by k and the code of
 * the payload, in buckets of about SIFT_CPOINT_PK_BUCKET entries. k is the key less kmin (dense: the keys lie in a range as wide as the rows, and k is the key
 * itself, no entry lists a wrong key) or the top bits of the hash of the key. The bucket of an entry is k >> rbits and an entry keeps k's low rbits and the
 * payload code, pbits in all, one after the other, so that a lookup reads the two directory words of its bucket and the entries between them: two blocks.
 * Block: u32 start[nbuckets + 1] (the index of the first entry of each bucket), padded to 8 bytes, then the entries as bits (rbits of k, pbits of code), padded
 * to 8 bytes. The payload code is the row, or (block - pbase) * pmul + offset of a heap address. Portable.
 */
typedef struct SiftCPackPar
{
	uint8		dense;			/* k is key - kmin; else the top bits of the hash */
	uint8		rbits;
	uint8		pbits;
	uint8		zero0;
	uint32		nbuckets;
	int64		kmin;
	uint64		pbase;			/* the first block of the addresses (0 for rows) */
	uint32		pmul;			/* the offset numbers of an address are below this (1 for rows) */
	uint32		zero1;
} SiftCPackPar;

/* A table is for a plain integer-like, text or uuid column of the top level (the leaves a small-set summary takes) */
extern const char *sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_cpoint_syntax(const char *opt);

extern uint64 sift_cpoint_hash(int64 key);

/* The slots of a table of n keys, at most 3/4 full: exactly enough (exact), or the next power of two */
extern uint32 sift_cpoint_slots(uint32 n, bool exact);

/* A slot is the fingerprint (the low 32 bits of the hash) over the row of a key with one row, or SIFT_CPOINT_LIST and the word offset of its list; it starts at the position the high 32 bits give */
static inline uint32
sift_cpoint_pos(uint64 hash, uint32 nslots, bool exact)
{
	return exact ? (uint32) (((hash >> 32) * nslots) >> 32) : (uint32) (hash >> 32) & (nslots - 1);
}

/* The slot after p, wrapping at the end of the table */
static inline uint32
sift_cpoint_next(uint32 p, uint32 nslots)
{
	return p + 1 == nslots ? 0 : p + 1;
}

static inline bool
sift_cpoint_slot_is(uint64 slot, uint64 hash)
{
	return (slot >> 32) == (hash & 0xFFFFFFFF);
}

/*
 * A table of heap addresses has the same slots with a 16 bit fingerprint over the 48 bit address of the key's only row (block << 16 | offset, the document key of
 * the units), or over SIFT_CPOINT_TID_LIST and the word offset of its list; a list is {u32 count (2 or more), u32 bytes, the bytes: the first address as a code
 * (block << 9 | offset) in LEB128, then the differences of the addresses after it}, padded to a word. A lookup returns addresses, so no unit is read.
 */
static inline bool
sift_cpoint_tid_slot_is(uint64 slot, uint64 hash)
{
	return (slot >> 48) == (hash & 0xFFFF);
}

static inline uint64
sift_cpoint_tid_code(uint64 key)
{
	return (key >> 16) << 9 | (key & 0xFFFF);
}

static inline uint64
sift_cpoint_tid_key(uint64 code)
{
	return (code >> 9) << 16 | (code & (SIFT_CPOINT_TID_MAXOFF - 1));
}

/*
 * Appends the leaf's block for the n entries (key, address) to out, addresses ascending in each key (sift_cpoint_build for the table of heap addresses; the
 * numbers it returns are the same: words counts the words of the lists, each padded to a word). With packed, the block is the packed table (SiftCPackPar, below)
 * when that is smaller than the slots and the lists: par->pbits is then not 0, nslots is the entries, nkeys the distinct k and words the 4-byte words of the block.
 */
extern void sift_cpoint_build_tids(const int64 *key, const uint64 *tid, uint32 n, bool exact, bool packed, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words);

/*
 * Appends the leaf's block for the n entries (key, row) to out, rows ascending in each key: the table (nslots * 8 bytes), then the lists, each {u32 count (2 or
 * more), u32 rows[count]} (words * 4 bytes, padded to 8), for the keys with more than one row. nkeys is the distinct keys. A key with more than `dense` rows
 * (0: no limit) has the payload SIFT_CPOINT_DENSE and no list; nrows is the rows the lists hold, a dense key counting one. With dense > 0 the table is of the
 * values of a list (SIFT_CPOINT_MODE_ZROWS): the lists are those of the table of addresses, and the rows of each key are listed once.
 */
extern void sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, uint32 dense, bool exact, bool packed, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words, uint32 *nrows);

/* The bucket and the remainder of key in a packed table (false: the key is out of the range of a dense table, so no entry holds it) */
static inline bool
sift_cpoint_pk_locate(const SiftCPackPar *par, int64 key, uint64 hash, uint32 *bucket, uint64 *rem)
{
	uint64		k;

	if (par->dense)
	{
		k = (uint64) key - (uint64) par->kmin;
		if (key < par->kmin || (k >> par->rbits) >= par->nbuckets)
			return false;
	}
	else
		k = hash >> (64 - (uint32) __builtin_ctz(par->nbuckets) - par->rbits);
	*bucket = (uint32) (k >> par->rbits);
	*rem = k & ((UINT64CONST(1) << par->rbits) - 1);
	return true;
}

/* The bits [at, at + n) of buf (n <= 56); the 8 bytes from the byte of at are readable */
static inline uint64
sift_cpoint_pk_get(const uint8 *buf, uint64 at, uint32 n)
{
	uint64		w;

	memcpy(&w, buf + at / 8, sizeof(w));
	return (w >> (at % 8)) & ((UINT64CONST(1) << n) - 1);
}

/* The bytes of the block of a packed table of n entries */
extern uint64 sift_cpoint_pk_bytes(const SiftCPackPar *par, uint32 n);

/* The heap address (block << 16 | offset) of a payload code of a table of addresses */
static inline uint64
sift_cpoint_pk_tid(const SiftCPackPar *par, uint64 code)
{
	return (par->pbase + code / par->pmul) << 16 | (code % par->pmul);
}

#endif
