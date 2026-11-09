/*
 * col/build/build_int.h - what the files of the index build share
 * the statics one file of the build calls from another
 */
#ifndef SIFT_COL_BUILD_DIR_INT_H
#define SIFT_COL_BUILD_DIR_INT_H

#ifdef SIFT_TESTING
#endif
#include "col/build.h"

typedef struct ColSpill ColSpill;

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
	SiftColFill *fill;				/* an index with connections: the carried values of every segment are filled from the parent tables (col/conn.h) */
	MemoryContext wcxt;			/* the writer of the current segment: freed with it */
	SiftColRow *rows;			/* the current page, deep copies */
	int			nrows;
	int			maxrows;
	BlockNumber page;
	SiftSegEntry *segs;
	uint8	   *lvl;			/* key_cut: how many times the rows of segs[i] were merged (a run is level 0) */
	uint32		aim;			/* key_cut: the aim of the units of the runs, which the merges of them keep */
	ColSpill   *spill;			/* key_cut: the rows go to a file for each range of the key (col/build/spill.c) */
	uint64		spill_run_rows;	/* the rows of a run of the ranges: those the first run held, or what fits SPILL_RUN_BYTES of them */
	uint32		spill_aim;		/* the aim of the units of the runs of the ranges: the first run's, whose batches held a superblock's rows */
	int64	   *ksample;		/* key_cut: the sorted keys of the rows of superblocks spread over the table, taken before the scan (col/build/spill.c) */
	uint32		nksample;
	uint32		unit_base;		/* the unit the next run of the ranges starts at */
	bool		spill_final;	/* the ranges are being read back */
	bool		spill_tried;	/* the first full run decided whether the build goes by ranges */
	int			nsegs;
	int			maxsegs;
	bool		shared;			/* a participant of a parallel build */
	uint64		total_rows;		/* the heap's estimate of the rows of the index */
	double		indexed;
	double		reported;
};

extern uint64	colb_next_gen(ColBuild *b);
extern Size colb_run_budget(ColBuild *b, Size held);
extern void colb_flush_batch(ColBuild *b);
extern void colb_add_row(ColBuild *b, const SiftColRow *row);
extern void colb_write_run(ColBuild *b, bool last);
extern void colb_spill_sample(ColBuild *b, Relation heap, IndexInfo *info);
extern bool colb_spill_start(ColBuild *b);
extern void colb_spill_batch(ColBuild *b, const SiftColBatch *batch);
extern void colb_spill_finish(ColBuild *b);

#endif