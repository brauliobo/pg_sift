/*
 * col/v1/spool.h - the entries of an order array that do not fit the memory a segment writer may take: (key, address) records kept in memory up to a bound, then on temp
 * files as sorted runs, read back in order by a merge. A segment holds its entries until its last unit is in, and the block of the order they make is built in one pass
 * over them in order, so the memory of the writer follows the bound and not the rows of the segment. Portable (core/host.h only: BufFile).
 */
#ifndef SIFT_COL_SPOOL_H
#define SIFT_COL_SPOOL_H

#include "core/host.h"

#define SIFT_ESET_ROWS		16384		/* the entries a set holds in memory before it writes a run (256 kB) */
#define SIFT_ESET_FANIN		64			/* the runs a merge reads at once */
#define SIFT_ESET_LEVELS	4			/* a level holds the runs of FANIN of the one below: FANIN^LEVELS runs of ESET_ROWS entries, far above the rows of a segment */
#define SIFT_ESET_BLOCK		512			/* the entries a run's reader takes from its file at a time (8 kB), and the unit a run's room on the file is rounded up to */
#define SIFT_ESET_COMPONENT	(UINT64CONST(1) << 30)	/* the bytes of a component file of a BufFile: a position past it is the component and the offset in it */

/* The record of a row: its key and the code of its heap address (sift_cpoint_tid_code); the key of a row with no value is 0 */
typedef struct SiftEnt
{
	int64		key;
	uint64		code;
} SiftEnt;

typedef struct SiftEntRun
{
	BufFile    *file;				/* the entries in order, from byte `off` of the file, or NULL: they are mem */
	SiftEnt    *mem;
	uint64		off;
	uint64		n;
	SiftEnt		first;				/* the least and the greatest entry: runs that do not overlap are read one after the other, without a merge */
	SiftEnt		last;
} SiftEntRun;

/* The runs of a set on its file: FANIN at most at each level, all appended to one file, so what a set holds does not follow its entries */
typedef struct SiftEntSpill
{
	SiftEntRun	lev[SIFT_ESET_LEVELS][SIFT_ESET_FANIN + 1];
	uint32		nlev[SIFT_ESET_LEVELS];
	SiftEntRun	flat[SIFT_ESET_FANIN];	/* sealed: the runs to read, at most FANIN */
	BufFile    *file;
	uint64		flen;				/* the end of the file */
} SiftEntSpill;

/*
 * The entries of one region of a leaf (its values, or its NULL rows). Add them in any order, seal the set, then read them in the order of (key, code) as often as needed. With
 * `map` the keys are replaced at the seal by map[key] (the rank of a string), which is not known while the entries come, so they are kept as they are until then.
 */
typedef struct SiftEntSet
{
	MemoryContext cxt;
	SiftEnt    *buf;				/* the entries since the last run was written */
	uint32		n;
	uint32		cap;
	uint32		maxrows;			/* the entries it holds in memory before it writes a run */
	bool		presort;			/* a run is sorted as it is written: the keys are not mapped at the seal */
	bool		sealed;
	uint64		total;
	BufFile    *raw;				/* without presort: the entries as they came, written by chunks */
	uint64		nraw;
	SiftEntSpill *sp;				/* NULL until the first run is written to a file */
	SiftEntRun	solo;				/* sealed with no run on a file: the entries in `buf` */
	const SiftEntRun *sealed_run;
	uint32		nsealed;
} SiftEntSet;

typedef struct SiftEntCur
{
	const SiftEntRun *run;
	uint32		nrun;
	bool		seq;				/* the runs do not overlap: they are read in the order of their first entries, no tree */
	uint32		cur;				/* seq: the position in order of the run being read */
	uint32		order[SIFT_ESET_FANIN];	/* the runs that have entries, by their first entry */
	uint32		nheap;				/* how many */
	uint32		nlive;				/* those that still have entries */
	SiftEnt		head[SIFT_ESET_FANIN];	/* the entry each is at (a tournament tree over them: lose[i] is the loser of node i, lose[0] the winner) */
	uint32		lose[SIFT_ESET_FANIN];
	SiftEnt    *base[SIFT_ESET_FANIN];	/* the entries of a run in reach: its block, or all of them for a run in memory */
	uint64		left[SIFT_ESET_FANIN];	/* those of its file not yet read */
	uint64		at[SIFT_ESET_FANIN];	/* the byte of its file the next of them is at */
	uint32		pos[SIFT_ESET_FANIN];
	uint32		cnt[SIFT_ESET_FANIN];
	SiftEnt    *blk;
} SiftEntCur;

/* maxrows: the entries the set holds in memory before it writes a run, at most SIFT_ESET_ROWS: the owner sizes it so that its sets stay within its memory budget */
extern void sift_eset_init(SiftEntSet *s, MemoryContext cxt, bool presort, uint32 maxrows);

extern void sift_eset_grow(SiftEntSet *s);

static inline void
sift_eset_add(SiftEntSet *s, int64 key, uint64 code)
{
	if (s->n == s->cap)
		sift_eset_grow(s);
	s->buf[s->n++] = (SiftEnt) {key, code};
	s->total++;
}

/* The entries are in order from here on: map NULL, or the key of each entry replaced by map[key] first (only for a set made without presort) */
extern void sift_eset_seal(SiftEntSet *s, const int64 *map);
/* Closes the files and drops what the set holds; it can be filled again */
extern void sift_eset_free(SiftEntSet *s);

extern void sift_ecur_open(SiftEntCur *c, const SiftEntSet *s);
/* The next at most m entries in order; fewer only at the end */
extern uint32 sift_ecur_read(SiftEntCur *c, SiftEnt *out, uint32 m);
extern void sift_ecur_close(SiftEntCur *c);

/* The values of a block, then its NULL rows, as one run of entries in order */
typedef struct SiftEntSrc
{
	const SiftEntSet *vals;
	const SiftEntSet *nulls;
	SiftEntCur	cur;
	bool		innulls;
} SiftEntSrc;

extern void sift_esrc_open(SiftEntSrc *r, const SiftEntSet *vals, const SiftEntSet *nulls);
extern uint32 sift_esrc_read(SiftEntSrc *r, SiftEnt *out, uint32 m);
extern void sift_esrc_close(SiftEntSrc *r);

/*
 * A block of bytes built in order: all of it in head while it is small, else head (what comes before the part on the file, the header and the directory of an order block), the
 * file (the chunks, written as they are made) and tail (what ends the block). A block of an order array is as long as its segment has rows, so it is not held whole in memory.
 */
#define SIFT_BLOB_MEM		262144

typedef struct SiftBlob
{
	MemoryContext cxt;
	StringInfoData head;
	BufFile    *file;
	uint64		flen;
	StringInfoData tail;
} SiftBlob;

extern void sift_blob_init(SiftBlob *b, MemoryContext cxt);
extern void sift_blob_free(SiftBlob *b);

static inline uint64
sift_blob_len(const SiftBlob *b)
{
	return (uint64) b->head.len + b->flen + (uint64) b->tail.len;
}

#endif
