/*
 * table/scan_int.h - shared between the files of the scans of a sift_table (scan.c and the files that hold the rest of it).
 */
#ifndef SIFT_SCAN_INT_H
#define SIFT_SCAN_INT_H

#include "table/table.h"

typedef struct SiftTblParallel
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 next;		/* next unit to hand out: the write buffer pages, then the superblocks of the column groups */
	uint64		nunits;
	uint64		nwbuf;			/* units that are write buffer pages */
	SiftDirSnapshot dir;		/* the directory every participant reads */
	int			nsegs;
	struct
	{
		uint64		gen;
		BlockNumber hdr;
		uint64		vbase;
		uint32		vcap;
		uint32		npages;
	}			seg[SIFT_MAX_SEGMENTS];
} SiftTblParallel;

typedef struct SiftTblScan
{
	TableScanDescData base;
	SiftTblRel *st;
	BlockNumber sample_blk;		/* TABLESAMPLE: the vblock being sampled, its rows (sorted by offset) in brows, the last offset */
	OffsetNumber sample_max;
	MemoryContext rowcxt;		/* the rows of the TABLESAMPLE block, until the next one */
	TupleDesc	desc;			/* of the rows returned: the slot's, which is the old one while ALTER TABLE rewrites */
	bool		snap_registered;
	int			nseg;
	SiftTblSeg *seg;			/* private copy of the write buffers (page tables complete) */
	uint32		dpos;			/* write buffer pages passed, in the segments' order: the page in the copy is number dpos - 1 */
	uint32		ndpages;
	bool		fresh;			/* at the start or at the end: a forward scan goes on from the first row, a backward one from the last */
	SiftTblSeg *cur;			/* segment of the page copy */
	BlockNumber curv;			/* vblock of the page copy */
	Page		copy;
	uint32		nrows;
	uint32		ri;
	const SiftTblRec *rec;		/* the write buffer record of the version last returned */
	SiftDirSnapshot *dir;		/* the directory the scan reads (its own copy, or the leader's) */
	SiftTblGroupScan *g;
	bool		in_groups;		/* the versions come from g */
	bool		groups_done;
	SiftTblParallel *par;
	bool		range;
	ItemPointerData mintid;
	ItemPointerData maxtid;
	/* bitmap */
	HeapTuple  *brows;
	int			nbrows;
	int			bri;
	int			nbcap;
	/* analyze */
	bool		an_started;
	uint64		an_next;		/* visits so far */
	uint64		an_p;			/* physical blocks of the table */
	uint64		an_m;			/* visits the sampler will make at most */
	uint64		an_wbuf;		/* write buffer pages */
	uint64	   *an_ranges;		/* [an_nranges][2] vblocks [from, to) of the groups */
	int			an_nranges;
	uint64		an_v;			/* vblocks of the groups */
	HeapTuple	arows[SIFT_TBL_MAX_OFF];	/* the rows of the group vblock read, with the weight of each in the estimate */
	TransactionId axmin[SIFT_TBL_MAX_OFF];	/* and the version of each */
	CommandId	acmin[SIFT_TBL_MAX_OFF];
	uint64		an_cur;			/* next vblock of the chunk (of the groups) to read */
	uint64		an_end;
	int			narows;
	int			ari;
	double		aweight;
} SiftTblScan;

extern bool scan_next_version(Relation rel, SiftTblScan *s, SiftTblRowVer *rv);
extern void scan_store(Relation rel, SiftTblScan *s, SiftTblRowVer *rv, TupleTableSlot *slot);

/* The group scan lives as long as the scan, whatever context the caller runs in (ALTER TABLE reads in a per-tuple one) */
static inline SiftTblGroupScan *
scan_gscan(SiftTblScan *s, Snapshot snapshot)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(s));
	SiftTblGroupScan *g = sift_tbl_gscan_begin(s->base.rs_rd, s->desc, s->st, s->dir, snapshot);

	MemoryContextSwitchTo(old);
	return g;
}

#endif							/* SIFT_SCAN_INT_H */
