/*
 * col/build/build_int.h - what the files of the index build share
 * the statics one file of the build calls from another
 */
#ifndef SIFT_COL_BUILD_DIR_INT_H
#define SIFT_COL_BUILD_DIR_INT_H

#ifdef SIFT_TESTING
#endif
#include "col/build.h"

struct ColBuild
{
	Relation	index;
	SiftColRel *r;
	MemoryContext cxt;
	MemoryContext pagecxt;		/* the rows of the current heap page */
	MemoryContext tmpcxt;		/* one tuple */
	SiftWriteTarget *wt;
	Size		budget;			/* memory a segment's writer may hold */
	Size		run_cap;		/* key_cut: the most the rows of a run may take (run_budget) */
	uint64		share;			/* key_cut: the rows the participant is expected to scan */
	uint64		held_rows;		/* key_cut: the rows in held */
	Size		trimmed;		/* key_cut: the bytes the trims of the batches in held freed */
	SiftColBatch *batch;		/* the rows of the current superblock, flushed at the next one */
	MemoryContext heldcxt;		/* the batches in held */
	List	   *held;			/* an index with the option key_cut: the batches of the superblocks since the last run, written sorted by the key as a segment when they pass the budget, and at the end (col/order.c) */
	SiftColSegWriter *w;
	MemoryContext wcxt;			/* the writer of the current segment: freed with it */
	SiftColRow *rows;			/* the current page, deep copies */
	int			nrows;
	int			maxrows;
	BlockNumber page;
	SiftSegEntry *segs;
	uint8	   *lvl;			/* key_cut: how many times the rows of segs[i] were merged (a run is level 0) */
	uint32		aim;			/* key_cut: the aim of the units of the runs, which the merges of them keep */
	int			nsegs;
	int			maxsegs;
	bool		shared;			/* a participant of a parallel build */
	uint64		total_rows;		/* the heap's estimate of the rows of the index */
	double		indexed;
	double		reported;
};

extern uint64	colb_next_gen(ColBuild *b);

#endif