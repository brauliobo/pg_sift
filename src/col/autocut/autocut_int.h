/*
 * col/autocut/autocut_int.h - what the files of the automatic cut share
 * the statics one file of the decision calls from another
 */
#ifndef SIFT_COL_AUTOCUT_INT_H
#define SIFT_COL_AUTOCUT_INT_H

#include "col/setsum.h"

#define AUTOCUT_SBS			256		/* superblocks sampled, at most */

#define AUTOCUT_LEAF_BYTES	1.0		/* what a row takes in a leaf that is not read from the sample, and has no statistics */

typedef struct Cell
{
	int64		key;
	uint32		sb;
	uint32		n;
} Cell;

typedef struct Track
{
	int			c;				/* leaf of the schema */
	AttrNumber	hatt;
	uint8		type;
	bool		cand;			/* key-shaped: an equality on its values is tested */
	bool		btree;			/* a b-tree of the table, unique and on this column alone, answers its lookups (a primary key): its zones are not worth keeping against a cut */
	bool		time;			/* a date or timestamp that is not a key, or the start lane of a range or inet column: a range of its values is tested */
	const SiftColDef *shape;	/* the head of the range or network whose lane this is (col/shape.c); NULL for a column's own value */
	int			lane;			/* which lane of the head */
	bool		image;			/* the first lane of a range or network: the image of its head, taken as untracked, is dictionary-coded in the order of the rows, and a unit of the cut by the lane lists it in the order of the lane, so it is priced like the lane */
	int64	   *v;				/* the value of each sampled row, 0 for a NULL */
	bool	   *nl;
	Cell	   *cells;
	int64		ncells;
	int64		cap;
	double		lo[AUTOCUT_SBS];
	double		hi[AUTOCUT_SBS];
	bool		has[AUTOCUT_SBS];
} Track;

typedef struct Samp
{
	uint32		n;				/* sampled rows */
	uint32		start[AUTOCUT_SBS + 1];	/* the rows of sampled superblock s are start[s] to start[s + 1] */
	int			nsb;
	SiftKey	   *tid;			/* the address of each sampled row, the sampled superblocks side by side (superblock s starts at block s * SIFT_SB_PAGES) */
} Samp;

extern int	acut_cell_cmp(const void *a, const void *b);

extern double	acut_zone_share(const Track *t, int n, int *m);

extern bool	acut_unique_btree(Relation heap, AttrNumber att);

extern bool	acut_scattered(const Track *t, int n);

extern AttrNumber	acut_cluster_att(Relation heap);

extern double	acut_cut_speedup(double reads, double ideal, double rows_per_unit);

extern double	acut_cut_benefit(double speedup, double relevance);

extern double	acut_cut_cost(double growth_pct, int nleaves);

extern double	acut_segment_rows(void);

extern double	acut_hot_share(Relation heap, const Track *t, uint32 nsampled);

extern double	acut_zone_loss(const Track *t, uint32 nrows, double nsb, double rows_per_unit, double kept_share);

extern double	acut_units_read(Track *t, int nsampled, double nsb, double rows_per_unit, bool keyed, bool kept, double *ideal, bool *fired);

extern double	acut_ranges_read(const Track *t, uint32 nrows, int nsampled, double nsb, double rows_per_unit, double *ideal, bool *fired);

extern double	acut_leaf_bytes(Relation heap, const SiftColDef *d, AttrNumber hatt);

extern double	acut_layout_bytes(const Track *tr, int ntr, int nleaves, const Samp *s, int key, int key2, bool units, double *plain, double nsb, double heap_pages, double rows_per_unit);

extern double	acut_mid_rows(Relation heap, BlockNumber nblocks, double nsb);

#endif