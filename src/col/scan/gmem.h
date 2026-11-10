/*
 * col/scan/gmem.h - the rows of the write buffers sorted for the global order of a leaf (col/scan/gorder.c reads them like the entries of a segment,
 * col/scan/gmem.c makes and keeps them)
 */
#ifndef SIFT_COL_SCAN_GMEM_H
#define SIFT_COL_SCAN_GMEM_H

#include "col/scan_int.h"

typedef struct GOrdPair
{
	int64		key;
	uint64		tid;
} GOrdPair;

typedef struct GOrdText
{
	const char *s;
	uint32		len;
	uint64		tid;
} GOrdText;

/* A segment read in memory is named by its generation and its live rows: rows only die, so the same two numbers are the same rows */
typedef struct GFreeTok
{
	uint64		gen;
	uint64		live;
} GFreeTok;

#ifdef SIFT_TESTING
extern PGDLLIMPORT uint64 sift_col_gkept_stat[4];
#define GKEPT_STAT(i)	(sift_col_gkept_stat[i]++)
#else
#define GKEPT_STAT(i)	((void) 0)
#endif

typedef struct ColGOrdMem
{
	int			leaf;
	int			gleaf;			/* the rows of the group gval of this leaf only (-1: all) */
	int64		gval;
	uint32		n;				/* entries: nvals with a value by (key, address), then those without by address */
	uint32		nvals;
	int64	   *key;
	uint64	   *tid;
	/* numeric key: key and tid of a range hold its entries once the range is sorted (gmem_sort); raw holds them by range, unsorted */
	GOrdPair   *raw;
	uint32	   *bstart;			/* the first entry of each of the nb ranges of the key (nb + 1 values) */
	bool	   *bdone;
	uint32		nb;
	int			shift;			/* the range of a key is (key - kmin) >> shift */
	int64		kmin;
	uint32		collid;
	/* a text key: the key of an entry is the position of the first entry of its string; str[pos] (len[pos] bytes, in the batches of the buffers) is the string of the entry at pos,
	 * all set once its range is sorted. ent holds the rows by their order in the buffers, which the pairs of raw name */
	const char **str;
	uint32	   *len;
	GOrdText   *ent;
	bool		bytes;			/* the range of a string is that of its 8 bytes after the first plen, which all the strings share: the collation orders by bytes */
	uint32		plen;
	const char *pre;			/* the first plen bytes of every string */
	MemoryContext own;			/* the context of an order made for one group of a scan that reads the groups one after the other, deleted when the next group replaces it (else NULL) */
} ColGOrdMem;

/* by key, then address */
extern void gord_pairs_sort(GOrdPair *a, uint32 n);
/* The entries with a value cut into ranges of the key of the same width; pv is freed (gorder.c) */
extern void gmem_ranges(ColGOrdMem *g, MemoryContext mcxt, GOrdPair *pv, uint32 nv);
/* The rows of the write buffers of a text leaf sorted by string, in mc (gorder.c); false as gord_mem */
extern bool gord_mem_text(SiftColSource *src, int leaf, uint32 collid, int v, MemoryContext mc);
/* The batches whose rows the memory source sorts: those of the write buffers, then those of the units of the segments without an order block (src->gb) */
static inline const SiftColBatch *
gord_mem_batch(const SiftColSource *src, int i)
{
	return i < src->nmem ? src->mem[i].b : src->gb[i - src->nmem];
}

/* The kept orders (gmem.c) and the order of a set of entries, for the segments without a block (gfree.c) */
struct GOrdKept;
extern ColGOrdMem *gord_mem_make(int leaf, int gleaf, int64 gval, MemoryContext mc, GOrdPair *pv, uint32 nv, GOrdPair *pn, uint32 nn);
extern ColGOrdMem *gkept_use(SiftColSource *src, struct GOrdKept *k);
extern struct GOrdKept *gkept_find_free(const SiftColSource *src, int leaf, int gleaf, int64 gval, const GFreeTok *tok, int n);
extern void gkept_store_free(SiftColSource *src, MemoryContext cx, ColGOrdMem *g, int leaf, int gleaf, int64 gval, const GFreeTok *tok, int n, int v);

/* The rows of those batches */
extern uint64 gord_mem_rows(const SiftColSource *src);
/* The values of group leaf gcol of the live rows of a segment without an order block, appended to vals (room for its rows); false when one has no value or an overflow one */
extern bool gord_free_group_values(SiftColSegment *seg, int gcol, int64 *vals, uint64 *n);
/* Reads the live rows of the segments that keep no order block of leaf (and of gleaf, when >= 0) into src->gb; false when they are more than the memory source may hold */
extern bool gord_mem_load(SiftColSource *src, int leaf, int gleaf, uint64 rows, uint32 collid);
/* Makes the sorted rows of the write buffers and the unordered segments for value v of the scan, or finds them kept by the backend (gmem.c): false when they are more than the cap of a read of `rows` rows */
extern bool gord_mem(SiftColSource *src, int leaf, uint64 rows, int gleaf, int64 gval, int v, uint32 collid);

#endif
