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
 * Ends only (column format minor 40, a block of version 1 whose header word 7 is not 0): the block holds the first head and the last values - head value entries of the
 * segment, which has word 7 of them in all, then the first NULL rows by address (all of them when the segment has no more than SIFT_CGORD_ENDS of them), in the order
 * of a complete block. Word 6 is head. The entries left out lie between the two ends and, as neither end splits a run of equal keys, their keys are after every key
 * of the head and before every key of the tail. A scan reads an end and stops at its edge (agg/gord.h), a range count or a bitmap reads the units.
 *
 * Version 2, a block grouped by another leaf (option order_array_group): the entries are sorted by (group, key, address) and the NULL rows by (group, address); rows with no
 * group are not in it. After the chunks come the groups, 24 bytes each {i64 group, u32 first value entry, u32 value entries, u32 first NULL entry, u32 NULL entries} ascending by
 * group; header words 6 and 7 are their number and the group leaf + 1. The keys of a group ascend, so a scan that pins the group reads its entries only, as a b-tree
 * on (group, key) would; a scan that does not pin it cannot use the block.
 *
 * Version 4, a block of the layout compact (column format minor 41, option order_layout): the chunks of the first and last 2 * SIFT_CGORD_ENDS (8,192) entries of the segment and the chunks that
 * hold NULL rows are as in version 1; every other chunk (SIFT_CGORD_COMPACT in the directory's pad word) has the bucket of every key instead of the key (col/gquant.h: about 0.6 bytes an entry
 * where the key takes 4), the exact key of its first entry in the directory, and the addresses of all its entries as block << ob | offset, the offset bits of the whole chunk. Header word 6 is
 * the buckets per mean gap the writer chose (SIFT_GQ_Q; the reader does not need it), word 7 is 0. The key of an entry is in its bucket; where a search or a scan needs it exact it is the one of
 * the row at the entry's address (sift_cseg_gorder_fill reads it from the heap). A row that bulkdelete cleared may have a new tenant at its address: the addresses of such rows
 * are listed once for the segment (col/seg/probe.c) and an entry at one is answered as a dead row, without the heap.
 *
 * Version 3, a block of a text leaf: the key of an entry is the rank of its string among the distinct strings of the segment in the order of the leaf's collation (then
 * the bytes), so equal keys are equal strings and a scan merges the segments by comparing their strings, not their keys. Header words 6 and 7 are the collation (an OID) and
 * the bytes of the strings, which end the block: {u32 strings, u32 strings in a block, u32 blocks, u32 0}, u32 offset[blocks + 1] from the start of this section, then the
 * blocks of SIFT_CGORD_STR_BLK strings in rank order, each string a front coded {varint bytes shared with the one before, varint bytes of its own, the bytes}. A string is
 * at most SIFT_COL_DICT_MAX_ENTRY bytes (a leaf with a longer one, which a unit stores inline, gets no block).
 *
 * Version 5, a text block of column format minor 42: as version 3, but the section that ends the block holds no strings, only where to find them: {u32 strings, u32 bits, u32 0, u32 0},
 * then the dictionary code of the string of every rank, `bits` wide each, lowest first, padded to 8 bytes. The string of a rank is the one the segment's dictionary of the leaf holds for
 * that code (a dictionary of CDICT_LOOKUP_MIN strings or more is written in blocks to be read in place), so the block does not keep a second copy of the strings.
 *
 * Seekable chunk (column format minor SIFT_COL_FORMAT_SEEKORD, SIFT_CGORD_SEEK in the pad word of the directory entry; any version, and a block may mix them with plain chunks): the m <= 512 entries are in
 * sub-blocks of SIFT_CGORD_SUB (32), and the first key of a sub-block is a fence, so a window of 32 entries is decoded without the 512 of its chunk. The key blob is
 * {u8 fence width, u8 flags (bit 0: scaled, bit 1: fence deltas zigzag), u64 scale if scaled, u8 per sub-block (bit 7: its deltas zigzag, bits 0..6: their width), the fence deltas (the key of the first entry
 * of sub-block s less that of s - 1, divided by the scale; one width), then for each sub-block its deltas from entry to entry, each width bits, packed, rounded up to bytes}; the first key of
 * the chunk is the directory's. The address blob is {u8 width wa, u64 minimum of the codes, per sub-block {u8 gap width gw or 0xFF, u8 bytes}, then for each sub-block its entries, rounded up to bytes}: a code
 * (block << ob | offset, ob in the pad word) less the minimum, wa bits; where gw is not 0xFF an entry after the first whose key is that of the entry before it is the gap to it less 1 (gw bits), the others are
 * whole codes. A chunk that mixes NULL rows (key 0) with values has fences that do not ascend (zigzag).
 *
 * Positional chunk (SIFT_CGORD_POS, a chunk of a grouped block, version 2 or 6, written beside the complete plain block of the same leaf, which has no compact chunk): the entry is the
 * position of its row in the plain block (value rows by key and address, then the NULL rows by address), so its key and address are the plain block's. The key blob is that of a seekable chunk
 * over the positions (the positions of a group ascend, those of the next start again: zigzag in the sub-block where they do), the address blob is {u32 position of the first entry, u32 0}; the
 * directory's first key is the key of the first entry, as in any chunk. The pad word is SIFT_CGORD_POS | 1 << 8. The writer keeps a chunk positional where the positions of a group are at most
 * SIFT_CGORD_POS_MAX_GAP apart on the mean (the entries a scan reads are then a few chunks of the plain order, not one chunk for each) and the chunk takes at most SIFT_CGORD_POS_MAX_PCT percent of the
 * bytes of the form it would have had. A scan reads the entries of a positional chunk a few at a time, each from the plain order's chunk that holds its position.
 */
#ifndef SIFT_COL_GORDER_H
#define SIFT_COL_GORDER_H

#include "core/host.h"

#include "col/enc.h"
#include "core/limits.h"

#define SIFT_CGORD_MAGIC	0x44474F43		/* "COGD" */
#define SIFT_CGORD_VERSION	1
#define SIFT_CGORD_VERSION_GROUP	2
#define SIFT_CGORD_VERSION_TEXT	3
#define SIFT_CGORD_VERSION_COMPACT	4
#define SIFT_CGORD_VERSION_TEXTPERM	5
#define SIFT_CGORD_VERSION_GROUPQ	6		/* a grouped block (version 2) some of whose chunks are compact (column format minor 41) */
#define SIFT_CGORD_COMPACT	2		/* SiftCGOrdDir.pad bit 1: the chunk keeps the bucket of every key (col/gquant.h) */
#define SIFT_CGORD_TIES	4		/* SiftCGOrdDir.pad bit 2, with SIFT_CGORD_COMPACT: the chunk has a run of SIFT_CGORD_TIE_RUN equal keys or more, which the buckets do not tell apart */
#define SIFT_CGORD_TIE_RUN	8
#define SIFT_CGORD_SEEK	8		/* SiftCGOrdDir.pad bit 3 (column format minor SIFT_COL_FORMAT_SEEKORD): the chunk is a seekable one, see "Seekable chunk" above; bits 8..11 are the bits of the offset in the address code */
#define SIFT_CGORD_POS	16		/* SiftCGOrdDir.pad bit 4 (column format minor SIFT_COL_FORMAT_SEEKORD), a chunk of a block grouped beside a complete plain block of the leaf: see "Positional chunk" above */
#define SIFT_CGORD_POS_MAX_GAP	1024	/* the mean distance between the positions of a group in a chunk the writer keeps positional (twice the entries of a chunk of the plain order) */
#define SIFT_CGORD_POS_FIRST	4		/* the entries of the first window a scan reads of a positional chunk: each costs a read of the plain order */
#define SIFT_CGORD_POS_MAX_PCT	75	/* the writer keeps the positional form of a chunk where it takes at most this many percent of the bytes of the other forms */
#define SIFT_CGORD_SUB	32		/* the entries of a sub-block of a seekable chunk: its first key is a fence */
#define SIFT_CGORD_SUBS	(SIFT_CGORD_ROWS_FINE / SIFT_CGORD_SUB)
#define SIFT_CGORD_SEEK_MAX_PCT	115	/* the limit gord_seek_pct: the writer keeps the seekable form of a chunk where it takes at most this many percent of the bytes of the plain one */
#define SIFT_CGORD_RUNS_SHIFT	16		/* SiftCGOrdDir.pad bits 16..23, in a compact chunk of a grouped block: the groups after the first that start in the chunk */
#define SIFT_CGORD_ANCHORS(pad)	(((pad) >> SIFT_CGORD_RUNS_SHIFT) & 0xFFu)
/* what follows the addresses of such a chunk: n positions (u16, padded to 8) and n exact keys, the first key of each of those groups in it */
#define SIFT_CGORD_ANCHOR_BYTES(n)	((n) == 0 ? (uint64) 0 : (((uint64) (n) * 2 + 7) & ~(uint64) 7) + (uint64) (n) * 8)
#define SIFT_CGORD_L_WHOLE	0		/* the layouts of sift_cgord_build */
#define SIFT_CGORD_L_COMPACT	1		/* every chunk of the middle of the order compact */
#define SIFT_CGORD_L_AUTO	2		/* each chunk of the middle compact where that is at least a fifth smaller than the whole one */
#define SIFT_CGORD_OL_AUTO	0		/* what the option order_layout asks of a column (SiftCPAgg.glayout) */
#define SIFT_CGORD_OL_WHOLE	1
#define SIFT_CGORD_OL_COMPACT	2
#define SIFT_CGORD_STR_BLK	16
#define SIFT_CGORD_STR_MAX	256		/* bytes of the buffer for a string of a text block (SIFT_COL_DICT_MAX_ENTRY + 1) */
#define SIFT_CGORD_GRPENT	24
#define SIFT_CGORD_GRP_PACKED	(1u << 16)	/* word 7 of a group block: its table of groups is packed (sift_cgord_grp_pack); bits 17..22 are the bits of a group's distance from the first group, 23..28 of its first entry */
#define SIFT_CGORD_GRP_PACK_MIN	1024		/* the groups from which the table is packed: below it the table is a few pages and is read as it is */
#define SIFT_CGORD_ENDS_LEAVES	8			/* the date, timestamp and integer columns the build chooses to keep the ends of (the text columns it chooses keep the whole order, at most 2) */
#define SIFT_CGORD_PAY_BLOCK	65536		/* the bytes of index one avoided cold block of a query is worth (sift_cgord_pay): the one rule of the default formulas */
#define SIFT_CGORD_ENDS		4096		/* the entries kept at each end of a block of the ends only, and of the NULL rows: a top-N of up to this many rows reads them */
#define SIFT_CGORD_ROWS		1024		/* the entries of a chunk written below column format minor 30, and the most a chunk holds (the slots of the arrays) */
#define SIFT_CGORD_ROWS_FINE	512		/* from minor 30: a seek and a cursor decode half of the entries */
#define SIFT_CGORD_GAPS		1		/* SiftCGOrdDir.pad bit 0: the addresses of the chunk are the first of each run of equal keys and the gaps; bits 8..11 are the bits of the offset in the address code (0: 9, the code of the point directory) */
#define SIFT_CGORD_HDR		32
#define SIFT_CGORD_DIRENT	24
#define SIFT_CGORD_GRP_EAGER	1048576	/* bytes of the group table read whole by the second probe of a segment (43,690 groups): a probe that starts over for each row of an enclosing query searches it in memory, not entry by entry through the buffers */
#define SIFT_CGORD_DIR_EAGER	262144	/* bytes of a directory a cursor reads whole (about 5.4 million rows in a segment), not entry by entry */
#define SIFT_GORD_PIN_MAX	16		/* the values of a group a scan reads at once: the elements of an IN list, or the equalities of a conjunction (a scan with more reads the plain order or the units) */
#define SIFT_CORDER_MAX_ROWS 16384	/* a range of the keys with more rows is read from the units: the addresses of a range are sorted by position before the heap is read (col/scan/scan.c, prune_range_tids) */
#define SIFT_CORDER_MAX_KEYS 512		/* an IN list of more keys is read from the units: each key seeks a chunk of its own, which past this many costs more than the key columns of the units (col/scan/prune_point.c, scan_prune_set_tids) */
#define SIFT_CGORD_MEM_ROWS	16384		/* rows of the write buffers a scan sorts for the order of a text key under a locale, at least (sift_col_gorder_open) */
#define SIFT_CGORD_MEM_MAX	2097152		/* rows the scan sorts in memory for an order, those of the write buffers and of the segments without an order block, past which it reads units: the memory of their order (about 64 bytes a row), not the time of it */
#define SIFT_CGORD_FREE_MAX	65536		/* ... of which those of the segments without an order block, for a key of numbers or times: each scan makes their order, 100 instructions a row, and the cheapest read of the units a top-N falls back to is 6.9 M instructions of 1 and of 5 million rows (a top-N by key; an OFFSET, a range and an IN list cost 14 to 48 M), so more rows than this lose to the units in every class; a text key costs about 10 times as much a row (strings of a dictionary compared by the collation) */
/* the rows of a read of `rows` rows that may be sorted in memory: a text key under a locale at least SIFT_CGORD_MEM_ROWS (more for a long read), any other SIFT_CGORD_MEM_MAX. The planner prices no walk that the scan refuses */
#define SIFT_CGORD_MEM_CAP(locale_text, rows)	((locale_text) ? Max(SIFT_CGORD_MEM_ROWS, 8 * (uint64) (rows)) : (uint64) sift_limit(gord_mem_rows, SIFT_CGORD_MEM_MAX))
#define SIFT_CGORD_FREE_CAP(text)	((uint64) sift_limit(gord_free_rows, SIFT_CGORD_FREE_MAX) >> ((text) ? 4 : 0))
#define SIFT_CGORD_CAP		(SIFT_CGORD_ROWS + 8)		/* slots of the arrays sift_cgord_decode fills */

/* A chunk of a global order decoded whole: the seek that ends in a chunk leaves it to the cursor that reads the entries from there (at UINT32_MAX: none).
 * Whatever fills addr[] must also set afrom and an: a producer that decodes the whole chunk (a write buffer's order, a memory source) sets afrom = 0, an = cm,
 * or the cursor (cur_at, gord_cur_fill) reads the window as empty and decodes the chunk again. */
typedef struct SiftCGOrdBuf
{
	uint32		at;				/* the position of the chunk's first entry */
	uint32		cm;				/* the keys of the chunk decoded: the first cm */
	uint32		afrom;			/* the addresses of entries [afrom, afrom + an) of the chunk are in addr[] (a window: a cursor that reads a few entries of a chunk decodes those; of a seekable chunk, the sub-blocks of the window) */
	uint32		an;
	bool		desc;			/* the cursor that owns the buffer reads the chunk backwards: its window ends at the entry asked for */
	uint32		rows;			/* the entries of the chunk */
	uint32		kfrom;			/* the exact keys of entries [kfrom, kfrom + kn) of the chunk are in key[] (a chunk that is not compact: the first cm; a window of a seekable one: [afrom, cm)) */
	uint32		kn;
	bool		stored;			/* the chunk is of a stored segment: it stays as it is for the next merge over the same segments (a write buffer's is rebuilt by each) */
	bool		compact;		/* the chunk is compact: kmin[] holds the first key of the bucket of each entry (if kmin_ok), sh the bucket's bits, key[] the keys read from the heap so far (the bits of exact) */
	bool		kmin_ok;
	uint32		sh;
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
	int64		kmin[SIFT_CGORD_CAP];
	uint64		exact[SIFT_CGORD_ROWS / 64];
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
	uint32		head;			/* a block of the ends only: the entries of the first end (the others of the values are the last end); else 0 */
	uint32		vtotal;			/* ... and the values the segment has in all; 0: the block is complete */
	uint32		quant;			/* version 4: the buckets per mean gap the writer chose; else 0 */
	bool		strperm;		/* version 5: the section after the chunks holds the dictionary code of each rank, not the strings */
	uint8		gbits;			/* version 2 with a packed table of groups (SIFT_CGORD_GRP_PACKED): the bits of a group and of the start of its entries; else 0 */
	uint8		sbits;
} SiftCGOrdHdr;

/* What a scan wants of the blocks (sift_col_gorder_open): a block of the ends only answers a scan of few rows that starts at an end of it */
typedef struct SiftCGOrdWant
{
	int			rows;
	bool		desc;
	bool		nulls_first;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
	bool		bygroup;		/* the scan has no condition at all: a leaf with no order of its own may be read through the second order of another leaf (gorder_by_group), which lists every row and tests nothing */
} SiftCGOrdWant;

/* Where the entries of a source that is an end of a block stop being the segment's (sift_col_gorder_cuts): the values [lo, hi) it offers, and which of the sides is not the segment's own end */
typedef struct SiftCGOrdCuts
{
	uint32		lo;
	uint32		hi;
	bool		lo_cut;			/* entries before lo exist and are not in the block */
	bool		hi_cut;
	bool		null_cut;		/* the NULL rows are not all in the block */
} SiftCGOrdCuts;

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
 * ends: a segment of more than 2 * SIFT_CGORD_ENDS values keeps the ends only.
 * layout: SIFT_CGORD_L_COMPACT writes a block of version 4 when the segment has more than 8 * SIFT_CGORD_ENDS (32,768) values and dense is set, else the whole order;
 * SIFT_CGORD_L_AUTO the same with a compact chunk only where it is at least a fifth smaller than the whole one (a block with none is the whole order).
 */
extern void sift_cgord_build(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, bool dense, bool ends, int layout, StringInfo out);
/* The rows of the complete plain block of the leaf, which a grouped block may name its entries by position in (the rows with no group are among them) */
typedef struct SiftCGOrdPlain
{
	const int64 *key;
	const uint64 *tid;
	uint32		nvals;
	const uint64 *ntid;
	uint32		nnull;
} SiftCGOrdPlain;
/* The same grouped by the leaf gcol: group[i] is the group of entry i of key / tid, ngroup[i] that of ntid[i]; every entry has one. plain: the rows of the plain block written beside it, or NULL */
extern void sift_cgord_build_grouped(const int64 *key, const uint64 *tid, const int64 *group, const uint8 *ok, uint32 n, uint32 nvals, const uint64 *ntid, const int64 *ngroup,
									 const uint8 *nok, uint32 nn, uint32 nnull, int gcol, bool dense, int layout, const SiftCGOrdPlain *plain, StringInfo out);

/*
 * The block of a text leaf (version 3): key[i] is the segment code of the string of the entry in d, ranked here by cmp under collid (0: the bytes), which the caller
 * says how to compare (the library of the host knows the collations); otherwise as sift_cgord_build.
 */
typedef int (*SiftCStrCmp) (const char *a, uint32 la, const char *b, uint32 lb, uint32 collid);
extern SiftCStrCmp sift_cgord_strcmp;
/* The order of the strings of a text block: the collation, then the bytes (what the b-tree of the column does) */
/* The least string after every string that starts with s, in the order of the bytes (the C collation): palloc'd, *olen bytes; NULL when there is none (no byte left to carry into) */
extern char *sift_cgord_prefix_end(const char *s, uint32 len, uint32 *olen);
extern int sift_cgord_str_order(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid);
/* perm: the block names the strings by their dictionary codes (version 5) instead of holding them (version 3) */
/* The codes of the strings of d in the order of the collation, then the bytes (palloc'd, d->n of them); a block of both forms takes the same */
extern uint32 *sift_cgord_text_order(const SiftCDict *d, uint32 collid);
extern void sift_cgord_build_text(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, const SiftCDict *d, uint32 collid, const uint32 *order, bool dense, bool perm, StringInfo out);
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
/* ... and of a version 5 block: its number of strings and the bits of a code; the code of rank r from the bytes at `sec` + 16 */
extern bool sift_cgord_perm_head(const uint8 *sec, const SiftCGOrdHdr *h, uint32 *nstr, uint32 *bits);

#ifdef SIFT_TESTING
extern int64 sift_col_gorder_decoded;	/* the entries of order array chunks decoded (sift._test_gorder_decoded) */
extern bool sift_test_col_gorder_seek;	/* sift.test_col_gorder_seek: every chunk of an order array is written seekable, whatever its bytes */
extern bool sift_test_col_gorder_noskip;	/* sift.test_col_gorder_noskip: an OFFSET walks the entries it throws away (sift_gord_skip takes none) */
extern int	sift_test_col_gorder_choice;	/* sift.test_col_gorder_choice: 1 keeps the ends of every order the build chose, 2 all of it, 0 decides by sift_cgord_pay */
extern int	sift_test_col_gorder_ends;		/* sift.test_col_gorder_ends: the entries a block of the ends only keeps at each end, 0 for SIFT_CGORD_ENDS */
extern int	sift_test_col_gorder_rows;		/* sift.test_col_gorder_rows: 1024 writes the chunks of minor 21, 0 the current ones */
#endif
/*
 * Does a segment's order of a leaf have to hold every row, or the ends only? A scan with a bound in the middle of the values (ts >= x ORDER BY ts LIMIT n, a count of
 * a range, an OFFSET past the ends) reads from the units what a complete block hands the heap: the units whose zone holds the bound, whatever the key the units are cut by (a query
 * across the keys reads them all; one scoped by the key would read fewer, but the order cannot know which it will be asked). That is units * (SIFT_CGORD_PAY_UNIT_BLOCKS + the blocks of the leaf's bytes in a unit) cold blocks; a block is worth
 * SIFT_CGORD_PAY_BLOCK bytes of index, so the complete block pays when they exceed its bytes (about log2 of the mean gap of the keys plus log2 of the rows, bits an entry).
 * The zones are the units' (min, max, values) of the leaf; the bounds tried are the middles of SIFT_CGORD_PAY_PROBES units spread over the segment.
 */
#define SIFT_CGORD_PAY_PROBES	16
#define SIFT_CGORD_PAY_UNIT_BLOCKS	1		/* the blocks a unit read costs besides its leaf: its directory */
#define SIFT_CGORD_PAY_UNIT_BITS	0.7		/* the share of the bits of its zone's width that a unit's values code in (random timestamps over 300 days, 9,000 values a unit: 4.5 blocks read of the 6.2 the width says) */
typedef struct SiftCGOrdZones
{
	const int64 *min;
	const int64 *max;
	const uint32 *nvals;
	uint32		nsb;
	uint64		rows;			/* the rows of the segment, those with no value too */
} SiftCGOrdZones;

typedef struct SiftCGOrdPay
{
	double		units;			/* the units a bound in the middle must read */
	double		blocks;			/* ... as cold blocks */
	double		saved;			/* ... and the bytes of index they are worth */
	double		bytes;			/* the bytes the complete block would take */
	bool		complete;		/* saved > bytes */
} SiftCGOrdPay;
extern void sift_cgord_pay(const SiftCGOrdZones *z, SiftCGOrdPay *out);

/* The entries of the chunks a block is written with: SIFT_CGORD_ROWS_FINE (SIFT_CGORD_ROWS in a test that asks for it, with the segment format 21) */
extern uint32 sift_cgord_rows(void);
/* The entries a block of the ends only keeps at each end: SIFT_CGORD_ENDS (a testing build can lower it: sift.test_col_gorder_ends) */
extern uint32 sift_cgord_ends(void);

/* Does the leaf get a block: a plain integer, date or timestamp attribute of the top level whose key is the integer itself (what the scan reads back as the value) */
extern bool sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d);
/* ... or a text one of the top level (version 3): the leaves order_array_columns names */
extern bool sift_cgord_leaf_numeric(const SiftColSchema *s, const SiftColDef *d);
extern bool sift_cgord_leaf_array_ok(const SiftColSchema *s, const SiftColDef *d);
/* The layout (SIFT_CGORD_L_*) of the block of a segment of nvals values for a column whose order_layout is want (SIFT_CGORD_OL_*): the option, or for 'auto' the rule of this function */
extern int	sift_cgord_layout_pick(uint8 want, uint32 nvals);
/* The value of order_layout into want[column]; s NULL: the syntax only. NULL, or why it is not valid */
extern const char *sift_cgord_layout_parse(const char *opt, const SiftColSchema *s, const char *const *names, uint8 *want);
/* a text leaf: its block is version 3 */
extern bool sift_cgord_leaf_text(const SiftColDef *d);
/* a date or a timestamp: the leaves whose order is read by a range or an ORDER BY .. LIMIT */
extern bool sift_cgord_leaf_time(const SiftColDef *d);

/* Parses the header of a block of len bytes; false when it is malformed (magic, counts, directory out of the block) */
extern bool sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h);

/* The groups of a version 2 block, checked as they are read in windows of any size, in order: the state between two windows */
typedef struct SiftCGOrdGrpRun
{
	uint32		vnext;
	uint32		nnext;
	int64		prev;
	bool		started;
} SiftCGOrdGrpRun;

/* the state before the first group of a block whose header was parsed */
#define SIFT_CGORD_GRPRUN(h)	((SiftCGOrdGrpRun) {0, (h)->values, 0, false})
/* The n groups e that follow those checked: false when they are out of the block, not ascending, or do not tile the entries; _done: after the last group, that they tile all of them */
extern bool sift_cgord_groups_check(const SiftCGOrdHdr *h, SiftCGOrdGrpRun *st, const SiftCGOrdGrp *e, uint32 n);
/* The bytes of the table of groups of a block whose header was parsed */
extern uint64 sift_cgord_grp_bytes(const SiftCGOrdHdr *h);
/* The bytes of a packed table to read for n groups from i on (and the one after them): where, and how many; then unpack them from win into out with first, the group the table starts with */
extern void sift_cgord_grp_span(const SiftCGOrdHdr *h, uint32 i, uint32 n, uint64 *off, uint32 *len);
extern void sift_cgord_grp_unpack(const SiftCGOrdHdr *h, int64 first, uint32 i, uint32 n, const uint8 *win, SiftCGOrdGrp *out);
extern bool sift_cgord_groups_done(const SiftCGOrdHdr *h, const SiftCGOrdGrpRun *st);

/* The directory entry ent (SIFT_CGORD_DIRENT bytes) of chunk c of a block whose header was parsed, false when the entry is out of the block or its offsets are */
extern bool sift_cgord_dirent(const uint8 *ent, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d);

/* Entries of chunk c: crows, or what is left in the last one */
static inline uint32
sift_cgord_chunk_rows(const SiftCGOrdHdr *h, uint32 c)
{
	return c + 1 < h->chunks ? h->crows : h->entries - c * h->crows;
}

/* The writer's helpers shared by col/gorder.c and col/gorder_seek.c: zero bytes to a multiple of 8, the codes of the addresses of a chunk (block << ob | offset into dv, and ob), and the chunk as a seekable one (false, nothing kept, when its addresses do not fit) */
extern void sift_cgord_pad8(StringInfo out);
extern uint32 sift_cgord_addr_codes(const int64 *av, uint32 m, int64 *dv);
extern bool sift_cgord_seek_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *kv, const int64 *av, uint32 m);
/* ... and the chunk of the m positions pv as a positional one (d keeps its first key and offset) */
extern void sift_cgord_pos_chunk(StringInfo body, SiftCGOrdDir *d, const int64 *pv, uint32 m);
/*
 * The table of the ng groups gs of a group block of nvals entries, none of them a NULL key, as the bits of the group's distance from the first group and of the start of its entries
 * (the count of a group is where the next starts) behind the first group, for a table of at least SIFT_CGORD_GRP_PACK_MIN groups whose distances fit 32 bits: the flags for word 7 of the block, 0 when
 * it is not packed (out is left as it was).
 */
extern uint32 sift_cgord_grp_pack(StringInfo out, const SiftCGOrdGrp *gs, uint32 ng, uint32 nvals);

/*
 * Decodes the first w entries (all m when w >= m) of a chunk of m from its two blobs (kb, klen) and (ab, alen) into key[0..w) and addr[0..w) (heap addresses); both need
 * SIFT_CGORD_CAP slots. A chunk of NULL rows gives 0 for every key. False when the blobs are malformed.
 */
extern bool sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first, uint32 w, int64 *key, uint64 *addr);

/* A seekable chunk parsed: the fences (the first key of each sub-block) and where the sub-blocks are; the keys and addresses of a sub-block decode from here alone */
typedef struct SiftCGOrdSeek
{
	const uint8 *kb;
	const uint8 *ab;
	uint32		m;
	uint32		nb;				/* sub-blocks */
	uint32		ob;
	uint32		wa;
	uint64		scale;
	uint64		amin;
	int64		fence[SIFT_CGORD_SUBS];
	uint32		koff[SIFT_CGORD_SUBS + 1];	/* the bytes of the deltas of each sub-block, in kb */
	uint32		aoff[SIFT_CGORD_SUBS + 1];	/* ... and of its addresses, in ab */
	const uint8 *kw;			/* per sub-block: its delta width and zigzag bit */
	const uint8 *am;			/* per sub-block: the gap width of its addresses and their bytes */
} SiftCGOrdSeek;
/* Parses the chunk of m entries whose first key is first (the blobs padded by SIFT_COL_PAD); false when malformed */
extern bool sift_cgord_seek_open(SiftCGOrdSeek *s, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first);
/* The first sub-block in [lo, hi) whose fence is not before key (after: is after it), hi when none; the fences of [lo, hi) ascend */
extern uint32 sift_cgord_seek_fence(const SiftCGOrdSeek *s, uint32 lo, uint32 hi, int64 key, bool after);
/* The keys and addresses of sub-block b into key[b * SIFT_CGORD_SUB ..] and addr[..]; false when malformed */
extern bool sift_cgord_seek_decode(const SiftCGOrdSeek *s, uint32 b, int64 *key, uint64 *addr);
/* The key and the heap address of entry i of a seekable chunk alone: the entries of its sub-block before it are decoded, not the 32 */
extern bool sift_cgord_seek_get(const SiftCGOrdSeek *s, uint32 i, int64 *key, uint64 *addr);
/* A positional chunk of m entries parsed (s->ab is NULL), and the positions of its sub-block b into pos[b * SIFT_CGORD_SUB ..] */
extern bool sift_cgord_pos_open(SiftCGOrdSeek *s, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 m);
extern void sift_cgord_pos_decode(const SiftCGOrdSeek *s, uint32 b, int64 *pos);
/* Does a block (of len bytes, header parsed in h) have a seekable chunk: the format minor of its segment */
extern bool sift_cgord_has_seek(const uint8 *blk, const SiftCGOrdHdr *h);
/* The first w keys (all m when w >= m) of a chunk from its key blob alone; key needs SIFT_CGORD_CAP slots */
extern void sift_cgord_decode_keys(MemoryContext mcxt, const uint8 *kb, uint32 klen, uint32 m, uint32 w, int64 *key);
/*
 * The heap addresses of the entries [*a0, a1) of a chunk of m from its address blob, with the keys key[0 .. a1) of the chunk decoded (the runs of equal keys name the gaps of the blob): *a0 moves back to the start
 * of the run of equal keys it falls in, where the blob is coded from. addr[*a0 .. a1) are filled; addr needs SIFT_CGORD_CAP slots. False when the blob is malformed.
 */
extern bool sift_cgord_decode_addrs(MemoryContext mcxt, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, const int64 *key, uint32 *a0, uint32 a1, uint64 *addr);

/* A compact chunk of m entries whose first key is first: the first key of the bucket of each entry into kmin (SIFT_CGORD_CAP slots; NULL: not wanted) with its shift, and the addresses of the first w entries into addr (SIFT_CGORD_CAP slots); false when malformed */
extern bool sift_cgord_decode_compact(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 pad, uint32 m, int64 first, uint32 w, int64 *kmin, uint32 *sh, uint64 *addr);
extern bool sift_cgord_group_bounds(int64 *kmin, int64 *key, uint64 *exact, uint32 m, int64 first, const uint8 *anch, uint32 n);

#endif
