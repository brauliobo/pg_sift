/*
 * col/seg_write/seg_write_int.h - what the files of the segment writer share
 * the statics one file of the writer calls from another
 */
#ifndef SIFT_COL_SEG_WRITE_INT_H
#define SIFT_COL_SEG_WRITE_INT_H

#include "col/ktab.h"
#include "col/seg_int.h"

/* The entries of the point directory of one leaf: the key and the row number (in the segment) of each row with a value; bad: some unit cannot be listed (an overflow value, inline strings, too many rows) */
typedef struct OrdBlock
{
	uint32		nch;			/* chunks (0: none) */
	uint32		nent;
	uint32		nvals;
	StringInfoData tab;
} OrdBlock;

typedef struct PointAcc
{
	int64	   *key;
	uint32	   *row;
	uint64	   *tid;			/* the heap address of each entry (a table of addresses, option point_tid_columns), else NULL */
	uint32		n;
	uint32		cap;
	bool		bad;
	bool		want;			/* the index names the leaf (option point_columns or point_tid_columns) */
	bool		tids;			/* ... and wants the table to list heap addresses */
	bool		ord;			/* the leaf also gets a global order block (col/gorder.h): the entries are kept for it, with the addresses of the rows without a value */
	int64	   *gk;				/* ord with a group leaf: the group of each entry of key, and of each one of ntid (a row with no group is left out when the block is built) */
	uint8	   *gok;
	int64	   *ngk;
	uint8	   *ngok;
	uint64	   *ntid;
	uint32		nn;
	uint32		ncap;
	uint32		gcap;
	OrdBlock	ob[2];			/* its blocks once built: the plain order, and (the group of the build's choice) the order sorted by group, which is the second slot of the leaf */
	uint32		nslots;			/* the block, once built at the end of the segment (0: none) */
	bool		exact;			/* ... with the slot count of SIFT_CPOINT_EXACT */
	SiftCPackPar par;			/* ... or as a packed table (par.pbits not 0, SIFT_CPOINT_PACKED): nslots is its entries */
	uint32		nkeys;
	uint32		words;
	StringInfoData tab;
} PointAcc;

/* The rows of each code of the dictionary of a leaf of the option count_columns, over the units added; bad: a unit stores strings inline or has an overflow value, so the rows would not add up */
typedef struct CountAcc
{
	uint32	   *rows;
	uint32		cap;
	bool		bad;
} CountAcc;

/* The small-set summaries (or the sorted runs) of one leaf: the blobs of its units one after the other and where each starts */
#define CDICT_LOOKUP_MIN	1024

#define LST_MIN_UNITS		64		/* a segment of fewer units keeps no leaf statistics: its zone arrays are a few pages */

typedef struct SetAcc
{
	StringInfoData blobs;
	uint64	   *off;			/* [capsb + 1]; unit u's blob is blobs[off[u], off[u + 1]) */
	bool		any;			/* some unit has one */
	uint32		kinds;			/* SETS_EXACT, SETS_BLOOM, SETS_PRES (RUNS_ORD, RUNS_IDENT): the kinds of blob the units have */
} SetAcc;

struct SiftColSegWriter
{
	SiftWriteTarget *wt;
	uint64		index_rows;		/* the rows of the index (sift_colw_set_index_rows) */
	uint64		gen;
	const SiftColSchema *schema;
	const SiftCcPolicy *pol;	/* [ncols] compression policy per leaf, NULL: plain chunks */
	MemoryContext mcxt;
	SiftStreamWriter *sw;
	SiftCDict **dict;			/* [ncols], STR leaves */
	uint32	  **dictmap;		/* [ncols] scratch: batch code -> segment code */
	bool	   *inl;			/* [ncols] scratch: the STR leaf is stored inline in the unit being added */
	SiftCZone  *zones;			/* [unit * ncols + col] */
	uint8	   *lst;			/* the leaf statistics block (seg_int.h LstHeader), made with the zones; NULL for a segment of fewer than LST_MIN_UNITS units */
	Size		lstlen;
	SiftCPaggSpec pagg;			/* the pre-aggregates to write (docs/COL-PREAGG.md) */
	SiftCLeafAgg *lagg;			/* [unit * ncols + col] exact sums of the INT leaves */
	uint64	   *cubeoff;		/* [unit]: offset of the unit's cube blob in cube; [nsb] the end */
	StringInfoData cube;		/* the cube blobs of all units */
	uint64	   *ecubeoff;		/* the same for the element blobs (minor 4) */
	StringInfoData ecube;
	uint64	   *xcubeoff;		/* the same for the exception cubes and element blobs (minor 19): a unit with none has an empty blob */
	StringInfoData xcube;
	uint64	   *xecubeoff;
	StringInfoData xecube;
	bool		excepts;		/* the units may get exceptions: a sift_col index, written under a setting that allows them */
	bool		anyexc;			/* some unit has exceptions: the segment is column format minor 19 */
	bool		anymeaslen;		/* some unit has a rollup with the length of each measure: the segment is column format minor 36 */
	uint64	   *pmask;			/* [unit * SIFT_PAGEMASK_WORDS]: the pages of its superblock that hold the rows of the unit (superunits) */
	SiftColSBRef *sbdir;
	uint32		nsb;
	uint32		capsb;
	uint64		rows;
	uint32		sb_lo;
	uint32		sb_hi;
	uint32		aim_rows;		/* the rows of a unit of the page cut of the table: a key-cut unit's cube is budgeted as that unit's */
	bool		keyed;			/* the units are key-cut: the segment is column format minor 17 */
	uint8		format;			/* SiftSegEntry.format: what the writer was begun with (a segment with a version section needs SIFT_COL_FORMAT_VERS, the sift_table sees to it); 4 writes the tags of overflow values */
	uint8		setting;		/* the minor the caller asked for, before the schema and the kinds narrowed it: 7 lets a cube hold min and max, 9 a unit the rollup of its rows, 10 a unit the small-set summary of a leaf, 12 the sorted run of a leaf */
	SetAcc	   *set;			/* [ncols], NULL unless the segment keeps small-set summaries (sift_colw_set_preagg) */
	bool		anysets;
	bool		anypres;		/* some unit keeps a key list or bitmap of a leaf (minor 15) */
	bool		anyrunl;		/* ... and one of them as runs of consecutive keys (minor 24) */
	SetAcc	   *run;			/* [ncols], NULL unless the segment keeps sorted runs (sift_colw_set_preagg) */
	bool		anyruns;
	bool		anyheads;		/* ... and one of them has its head (column format minor 18) */
	SetAcc	   *hll;			/* [ncols], NULL unless the segment keeps HyperLogLog sketches (sift_colw_set_preagg) */
	bool		anyhll;
	SiftCKtab  *kt;				/* the key tables of the leaves the index keeps key lists for (minor 22), NULL when the segment has none to keep */
	StringInfoData ktab;
	CountAcc   *cnt;			/* [ncols], NULL unless the segment counts the rows of the codes of some leaf (option count_columns, minor 24) */
	PointAcc   *pt;				/* [ncols], NULL unless the segment keeps point directories (sift_colw_set_preagg) */
	bool		anypoints;		/* some leaf has a point directory to write */
	bool		anyord;			/* some leaf has a global order block to write (minor 30; minor 21 had chunks of SIFT_CGORD_ROWS) */
	bool		anydual;		/* ... and some leaf a second one, sorted by group (minor 31) */
	bool		anydense;		/* ... and some chunk codes its addresses with the bits of its largest offset (minor 38) */
	bool		anytextord;		/* ... and some leaf of text (minor 32) */
	bool		anypacked;		/* ... and one of them is a packed table (column format minor 36) */
	bool		anytids;		/* ... and one of them lists heap addresses, or the rows of the values of a list in the frame of the addresses (column format minor 16) */
	bool		inexact;		/* some unit lacks its kinds: the segment keeps the minor it has (minor 14 promises complete kinds) */
	uint8	   *dform;			/* [ncols] the dictionary is written in blocks to be read in place (DICT_BLOCKS), and with the order to be searched in place (DICT_LOOKUP): minor 14; 0: the compact form */
	StringInfoData buf;
	/* the version section: one VersUnit per unit, the vectors of the units, the range of the xids */
	VersUnit   *vunit;
	StringInfoData vblob;
	bool		anyver;
	uint32		vmin;
	uint32		vmax;
};

extern void	segw_sw_pad8(SiftStreamWriter *sw);

extern bool	segw_elem_point(const SiftColSegWriter *w, int c);

extern bool	segw_gorder_grouped(const SiftColSegWriter *w, int c);

extern bool	segw_gorder_plain(const SiftColSegWriter *w, int c);

extern void	segw_add_preagg(SiftColSegWriter *w, const SiftColBatch *b, Size unit_bytes);

extern void	segw_add_vers(SiftColSegWriter *w, const SiftColBatch *b);

extern uint64	segw_put8(SiftColSegWriter *w, const void *data, Size len);

extern bool	segw_rooted_ok(const SiftColSegWriter *w);

extern void	segw_build_points(SiftColSegWriter *w);

extern void	segw_write_points(SiftColSegWriter *w, SiftSectionRef *sec);

extern void	segw_write_dicts(SiftColSegWriter *w, SiftSectionRef *sec);

extern SetsLeaf *	segw_layout_blobs(SiftColSegWriter *w, const SetAcc *acc, Size at, Size *start, Size *bytes, int *nleaves);

extern void	segw_write_blobs(SiftColSegWriter *w, const SetAcc *acc, uint32 magic, SetsLeaf *sl, int n);

extern void	segw_write_zones(SiftColSegWriter *w, SiftSectionRef *sec);

extern void	segw_write_vers(SiftColSegWriter *w);

#endif