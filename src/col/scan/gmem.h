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
} ColGOrdMem;

/* by key, then address */
extern void gord_pairs_sort(GOrdPair *a, uint32 n);
/* The entries with a value cut into ranges of the key of the same width; pv is freed (gorder.c) */
extern void gmem_ranges(ColGOrdMem *g, MemoryContext mcxt, GOrdPair *pv, uint32 nv);
/* The rows of the write buffers of a text leaf sorted by string, in mc (gorder.c); false as gord_mem */
extern bool gord_mem_text(SiftColSource *src, int leaf, uint64 maxrows, uint32 collid, int v, MemoryContext mc);
/* Makes the sorted rows of the write buffers for value v of the scan, or finds them kept by the backend (gmem.c) */
extern bool gord_mem(SiftColSource *src, int leaf, uint64 maxrows, int gleaf, int64 gval, int v, uint32 collid);

#endif
