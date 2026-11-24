/*
 * col/v1/src_wide.h - a wide leaf as the dictionary
 * leaf a unit reader of Format 1 gives: the lanes of a unit are decoded once into a dictionary of the distinct values, each entry the exact value
 * PostgreSQL holds, display scale and interval fields included, and a code per row. The executor then treats the leaf as any other
 * dictionary leaf, except that an entry is turned into a datum of the leaf's type (sift_srcwide_datum) instead of a text.
 */
#ifndef SIFT_COL_V1_SRC_WIDE_H
#define SIFT_COL_V1_SRC_WIDE_H

#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"

#include "col/col.h"

typedef struct SiftSrcWide
{
	MemoryContext cxt;
	uint32		n;				/* rows */
	uint32		cap;
	uint32	   *codes;			/* [n]: the entry of the row, 0 for a NULL */
	uint8	   *isnull;			/* [n] */
	bool		has_null;
	uint32		nd;				/* entries */
	uint32		dcap;
	uint32	   *off;			/* nd + 1 */
	char	   *bytes;
	size_t		blen;
	size_t		bcap;
	uint32		nslot;			/* the table that finds an entry by the stored words of its value */
	uint32	   *slot;
	uint64	   *key;			/* 3 words per entry */
	char	   *txt;			/* room for the text of any image */
	struct DNum *recs;			/* sift_aggwide_nums: [n], made by sift_aggwide_rec when a row first asks */
	uint32		rcap;
	const SiftColDef *def;		/* ... from these lanes */
	SiftColVec	lanes[SIFT_COL_MAX_LANES];
	struct NumMemo *memo;		/* ... and what it made of the entries of an image dictionary, nmemo (a power of 2) slots */
	uint32		nmemo;
	MemoryContext acx;			/* the numerics of the records that are not an integer and a scale, reset by each build */
} SiftSrcWide;

/* The entries of the rows of `lanes` (the lane vectors of def, as sift_col_read_lanes gives them), in the order of their first row */
extern void sift_srcwide_build(SiftSrcWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 n);

/* The room for n rows of codes and null flags, and the numeric of row i of an image or a 128-bit lane leaf (a NaN when nan) in b, which is as large as SIFT_NIMG_TEXT_MAX + 64 bytes */
extern void sift_srcwide_rows_room(SiftSrcWide *w, uint32 n);
extern uint32 sift_srcwide_numeric_entry(SiftSrcWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 i, bool nan, uint8 *b);

/* The Datum of an entry of the dictionary of a leaf of a type sift_srcwide_type says: the entry is the datum */
extern Datum sift_srcwide_datum(Oid typid, const char *bytes, uint32 len);

/* Does the SQL type have its leaf read through such a dictionary? */
static inline bool
sift_srcwide_type(Oid typid)
{
	return typid == NUMERICOID || typid == INTERVALOID || typid == UUIDOID || typid == BYTEAOID || typid == INETOID || typid == CIDROID ||
		typid == MACADDROID || typid == MACADDR8OID || typid == TIMETZOID || typid == BITOID || typid == VARBITOID || type_is_array(typid) ||
		type_is_range(typid) || type_is_multirange(typid);
}

#endif							/* SIFT_COL_V1_SRC_WIDE_H */
