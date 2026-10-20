/*
 * sift_aggwide.h - a wide leaf as the dictionary
 * leaf the aggregate scan reads: the lanes of a unit are decoded once into a dictionary of the distinct values, each entry the exact value
 * PostgreSQL holds, display scale and interval fields included, and a code per row. The executor then treats the leaf as any other
 * dictionary leaf, except that an entry is turned into a datum of the leaf's type (sift_agg_wide_datum) instead of a text.
 */
#ifndef SIFT_AGGWIDE_H
#define SIFT_AGGWIDE_H

#include "catalog/pg_type_d.h"

#include "sift_col.h"
#include "sift_colsrc.h"

typedef struct SiftAggWide
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
} SiftAggWide;

/* The entries of the rows of `lanes` (the lane vectors of def, as sift_col_read_lanes gives them), in the order of their first row */
extern void sift_aggwide_build(SiftAggWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 n);

/* The Datum of an entry of the dictionary of a leaf of type typid (NUMERICOID or INTERVALOID) */
extern Datum sift_aggwide_datum(Oid typid, const char *bytes, uint32 len);

/* Does the SQL type have its leaf read through such a dictionary? */
static inline bool
sift_aggwide_type(Oid typid)
{
	return typid == NUMERICOID || typid == INTERVALOID;
}

#endif							/* SIFT_AGGWIDE_H */
