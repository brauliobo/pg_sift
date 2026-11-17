/*
 * col/autocut/autocut_int.h - what the files of the automatic cut share
 * the statics one file of the decision calls from another
 */
#ifndef SIFT_COL_AUTOCUT_INT_H
#define SIFT_COL_AUTOCUT_INT_H

#include "storage/buffile.h"
#include "col/setsum.h"

#define AUTOCUT_SBS			256		/* superblocks sampled, at most */

#define AUTOCUT_LEAVES		8		/* leaves read: the candidates and those that may compete with them */

#define AUTOCUT_KEY_BYTES	2.0		/* what a key takes in the key list of a unit */

#define AUTOCUT_LEAF_BYTES	1.0		/* what a row takes in a leaf that is not read from the sample, and has no statistics */

#define AUTOCUT_CHUNK		(2 * BLCKSZ)	/* the bytes a spool keeps in memory or writes at a time */

#define AUTOCUT_CHUNK_COST	(AUTOCUT_CHUNK + 128)	/* what a chunk kept in memory takes of the budget: its block, with the header, and the slot of the array that lists the chunks, which doubles */

/* the bytes of chunks the spools of one decision may keep in memory: maintenance_work_mem; the chunks past it are written to a temporary file */
typedef struct AcutBudget
{
	int64		left;
} AcutBudget;

/* an array of fixed-size records (a multiple of 8 bytes), appended once and read front to back by any number of cursors */
typedef struct AcutSpool
{
	AcutBudget *budget;
	uint32		recsz;
	uint32		per;			/* records of a chunk */
	uint64		n;
	char	  **chunk;			/* the first chunks, kept in memory */
	uint32		nchunk,
				capchunk;
	char	   *tail;			/* the chunk being filled */
	uint32		tailn;
	BufFile    *file;			/* the chunks after those, in order: chunk nchunk + j is block j * (AUTOCUT_CHUNK / BLCKSZ) */
	uint32		nspill;
} AcutSpool;

typedef struct AcutCur
{
	const AcutSpool *s;
	uint64		pos,			/* the first record of the window: the records of one chunk the cursor reads in a row */
				end;
	const char *p;				/* the next record of the window */
	uint32		left,			/* records of the window not read */
				nwin;
	char	   *buf;			/* the chunk of the file the cursor reads, taken at the first need */
	int64		bufchunk;
} AcutCur;

/*
 * the records of several runs of one spool (run r is bound[r] to bound[r + 1]) read as one run: each run ascends by its first nkeys (1 or 2) signed 64-bit fields, and records
 * of equal keys come in the order of the runs, so a run holds the records that come before those of the runs after it. A tournament tree of the runs: the next record costs
 * one match a level.
 */
typedef struct MergeEnt
{
	int64		k[2];			/* the keys of the record the run is at (0 for a key a record has not) */
	int			run;
	bool		end;			/* the run is past its end */
} MergeEnt;

typedef struct AcutMerge
{
	int			nruns,
				nkeys,
				width;			/* the runs, as a power of 2 */
	AcutCur   *cur;
	const void **head;			/* the record each run is at, NULL past its end */
	MergeEnt   *tree;			/* tree[0]: the run to read next; tree[1 ..]: the loser of each match */
	int			last;			/* the run of the record returned last, advanced by the next call */
} AcutMerge;

extern void acut_spool_init(AcutSpool *s, AcutBudget *budget, uint32 recsz);

/* a record is appended by filling the slot and committing it, or by adding a copy of it */
extern void *acut_spool_slot(AcutSpool *s);

extern void acut_spool_commit(AcutSpool *s);

extern void acut_spool_add(AcutSpool *s, const void *rec);

extern void acut_spool_free(AcutSpool *s);

extern void acut_cur_init(AcutCur *c, const AcutSpool *s, uint64 from, uint64 to);

/* the cursor reads records from to (end of range) of its spool next */
extern void acut_cur_seek(AcutCur *c, uint64 from, uint64 to);

extern void acut_cur_free(AcutCur *c);

extern const void *acut_cur_window(AcutCur *c);

/* the record at the cursor, valid until the cursor reads another chunk; NULL past its end */
static inline const void *
acut_cur_next(AcutCur *c)
{
	const void *rec;

	if (c->left == 0)
		return acut_cur_window(c);
	rec = c->p;
	c->p += c->s->recsz;
	c->left--;
	return rec;
}

extern void acut_merge_begin(AcutMerge *m, const AcutSpool *s, const uint64 *bound, int nruns, int nkeys);

extern const void *acut_merge_next(AcutMerge *m);

extern void acut_merge_end(AcutMerge *m);

typedef struct Track
{
	int			c;				/* leaf of the schema */
	int			idx;			/* its column in the records of the sample */
	AttrNumber	hatt;
	uint8		type;
	bool		cand;			/* key-shaped: an equality on its values is tested */
	bool		btree;			/* a b-tree of the table, unique and on this column alone, answers its lookups (a primary key): its zones are not worth keeping against a cut */
	bool		time;			/* a date or timestamp that is not a key, or the start lane of a range or inet column: a range of its values is tested */
	const SiftColDef *shape;	/* the head of the range or network whose lane this is (col/shape.c); NULL for a column's own value */
	int			lane;			/* which lane of the head */
	bool		image;			/* the first lane of a range or network: the image of its head, taken as untracked, is dictionary-coded in the order of the rows, and a unit of the cut by the lane lists it in the order of the lane, so it is priced like the lane */
	uint32		nonnull;		/* the sampled rows that have a value */
	uint64		top;			/* the rows of the most common value in the sample, once units_read has read the cells */
	double		lo[AUTOCUT_SBS];
	double		hi[AUTOCUT_SBS];
	bool		has[AUTOCUT_SBS];
} Track;

/*
 * The sample: its rows are records of 8 bytes of address (the block in the superblock and the offset), 8 bytes for the value of each track (0 for a NULL) and
 * a mask of the tracks that are NULL, in the order of the sampled superblocks and of the heap within each, in a spool that keeps to the budget.
 */
typedef struct Samp
{
	uint32		n;				/* sampled rows */
	uint32		start[AUTOCUT_SBS + 1];	/* the rows of sampled superblock s are start[s] to start[s + 1] */
	int			nsb;
	int			ntr;
	uint32		recsz;
	AcutBudget budget;
	AcutSpool		rows;
} Samp;

#define ROW_TID(r)		(((const uint64 *) (r))[0])
#define ROW_V(r, t)		(((const int64 *) (r))[1 + (t)])
#define ROW_NL(r, ntr, t)	((bool) ((((const uint64 *) (r))[1 + (ntr)] >> (t)) & 1))

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

extern double	acut_units_read(Track *t, Samp *s, double nsb, double rows_per_unit, double keyed_room, bool kept, double *ideal, bool *fired);

extern double	acut_ranges_read(const Track *t, Samp *s, double nsb, double rows_per_unit, double *ideal, bool *fired);

extern double	acut_leaf_bytes(Relation heap, const SiftColDef *d, AttrNumber hatt);

extern double	acut_layout_bytes(const Track *tr, int ntr, int nleaves, Samp *s, int key, int key2, bool units, double *plain, double nsb, double heap_pages, double rows_per_unit);

extern double	acut_mid_rows(Relation heap, BlockNumber nblocks, double nsb);

#endif
