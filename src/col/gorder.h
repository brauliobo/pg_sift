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
 *
 * Version 3, a block of a text leaf: the key of an entry is the rank of its string among the distinct strings of the segment in the order of the leaf's collation (then
 * the bytes), so equal keys are equal strings and a scan merges the segments by comparing their strings, not their keys. Header words 6 and 7 are the collation (an OID) and
 * the bytes of the strings, which end the block: {u32 strings, u32 strings in a block, u32 blocks, u32 0}, u32 offset[blocks + 1] from the start of this section, then the
 * blocks of SIFT_CGORD_STR_BLK strings in rank order, each string a front coded {varint bytes shared with the one before, varint bytes of its own, the bytes}. A string is
 * at most SIFT_COL_DICT_MAX_ENTRY bytes (a leaf with a longer one, which a unit stores inline, gets no block).
 */
#ifndef SIFT_COL_GORDER_H
#define SIFT_COL_GORDER_H

#include "core/host.h"

#include "col/enc.h"

#define SIFT_CGORD_MAGIC	0x44474F43		/* "COGD" */
#define SIFT_CGORD_VERSION	1
#define SIFT_CGORD_VERSION_GROUP	2
#define SIFT_CGORD_VERSION_TEXT	3
#define SIFT_CGORD_STR_BLK	16
#define SIFT_CGORD_STR_MAX	256		/* bytes of the buffer for a string of a text block (SIFT_COL_DICT_MAX_ENTRY + 1) */
#define SIFT_CGORD_GRPENT	24
#define SIFT_CGORD_ROWS		1024		/* the entries of a chunk written below column format minor 30, and the most a chunk holds (the slots of the arrays) */
#define SIFT_CGORD_ROWS_FINE	512		/* from minor 30: a seek and a cursor decode half of the entries */
#define SIFT_CGORD_HDR		32
#define SIFT_CGORD_DIRENT	24
#define SIFT_GORD_PIN_MAX	16		/* the values of a group a scan reads at once: the elements of an IN list, or the equalities of a conjunction (a scan with more reads the plain order or the units) */
#define SIFT_CORDER_MAX_ROWS 16384	/* a range of the keys with more rows is read from the units: the addresses of a range are sorted by position before the heap is read (col/scan.c, prune_range_tids) */
#define SIFT_CGORD_MEM_ROWS	4096		/* rows of the write buffers a scan sorts for the order, at least (sift_col_gorder_open) */
#define SIFT_CGORD_CAP		(SIFT_CGORD_ROWS + 8)		/* slots of the arrays sift_cgord_decode fills */

/* A chunk of a global order decoded whole: the seek that ends in a chunk leaves it to the cursor that reads the entries from there (at UINT32_MAX: none) */
typedef struct SiftCGOrdBuf
{
	uint32		at;				/* the position of the chunk's first entry */
	uint32		cm;				/* the entries of the chunk decoded: the first cm */
	uint32		rows;			/* the entries of the chunk */
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
} SiftCGOrdBuf;

typedef struct SiftCGOrdHdr
{
	uint32		entries;
	uint32		values;
	uint32		chunks;
	uint32		crows;
	uint32		ngroups;		/* version 2: the groups of the end of the block, and the leaf they are the values of; else 0 and -1 */
	int			gcol;
	uint32		collid;			/* version 3: the collation of the strings, and the bytes of them at the end of the block; else 0 */
	uint32		strbytes;
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
 * are heap addresses (block << 16 | offset, offsets below 512). The chunks hold SIFT_CGORD_ROWS_FINE entries (a block of SIFT_CGORD_ROWS is read as well). The block is a multiple of 8 bytes.
 */
extern void sift_cgord_build(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, StringInfo out);
/* The same grouped by the leaf gcol: group[i] is the group of entry i of key / tid, ngroup[i] that of ntid[i]; every entry has one */
extern void sift_cgord_build_grouped(int64 *key, uint64 *tid, int64 *group, uint32 nvals, uint64 *ntid, int64 *ngroup, uint32 nnull, int gcol, StringInfo out);

/*
 * The block of a text leaf (version 3): key[i] is the segment code of the string of the entry in d, ranked here by cmp under collid (0: the bytes), which the caller
 * says how to compare (the library of the host knows the collations); otherwise as sift_cgord_build.
 */
typedef int (*SiftCStrCmp) (const char *a, uint32 la, const char *b, uint32 lb, uint32 collid);
extern SiftCStrCmp sift_cgord_strcmp;
/* The order of the strings of a text block: the collation, then the bytes (what the b-tree of the column does) */
extern int sift_cgord_str_order(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid);
extern void sift_cgord_build_text(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, const SiftCDict *d, uint32 collid, StringInfo out);
/* Where the decode of a front coded block of strings stands: the next string to decode, its place in the block, and the bytes of the string before it, which out holds */
typedef struct SiftCStrPos
{
	uint32		next;
	uint32		pos;
	uint32		olen;
} SiftCStrPos;
/* The string idx of a text block (idx = rank % SIFT_CGORD_STR_BLK): b is the front coded block of the strings that holds it, len its bytes; out SIFT_CGORD_STR_MAX, which keeps the string decoded last so that the next call goes on from it; false when malformed */
extern bool sift_cgord_str_get(const uint8 *b, uint32 len, SiftCStrPos *st, uint32 idx, char *out, uint32 *slen);
/* The strings section of a block of len bytes whose header was parsed: its number of strings and blocks; false when it is malformed */
extern bool sift_cgord_str_head(const uint8 *sec, const SiftCGOrdHdr *h, uint32 *nstr, uint32 *nblk);

#ifdef SIFT_TESTING
extern int	sift_test_col_gorder_rows;		/* sift.test_col_gorder_rows: 1024 writes the chunks of minor 21, 0 the current ones */
#endif
/* The entries of the chunks a block is written with: SIFT_CGORD_ROWS_FINE (SIFT_CGORD_ROWS in a test that asks for it, with the segment format 21) */
extern uint32 sift_cgord_rows(void);

/* Does the leaf get a block: a plain integer, date or timestamp attribute of the top level whose key is the integer itself (what the scan reads back as the value) */
extern bool sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d);
/* ... or a text one of the top level (version 3): the leaves order_array_columns names */
extern bool sift_cgord_leaf_array_ok(const SiftColSchema *s, const SiftColDef *d);
/* a text leaf: its block is version 3 */
extern bool sift_cgord_leaf_text(const SiftColDef *d);
/* a date or a timestamp: the leaves whose order is read by a range or an ORDER BY .. LIMIT */
extern bool sift_cgord_leaf_time(const SiftColDef *d);

/* Parses the header of a block of len bytes; false when it is malformed (magic, counts, directory out of the block) */
extern bool sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h);

/* The groups of a version 2 block of len bytes whose header was parsed, false when they are out of the block, not ascending, or do not tile the entries */
extern bool sift_cgord_groups(const uint8 *blk, const SiftCGOrdHdr *h, uint64 len, SiftCGOrdGrp *out);

/* The directory entry ent (SIFT_CGORD_DIRENT bytes) of chunk c of a block whose header was parsed, false when the entry is out of the block or its offsets are */
extern bool sift_cgord_dirent(const uint8 *ent, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d);

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
