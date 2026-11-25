/*
 * col/f2/f2_store_scan_int.h - what the files of the scan of a sift_table share: the scan, the part it reads and the verdict of a predicate on a stored value (col/f2/f2_store_scan.c).
 */
#ifndef SIFT_COL_F2_STORE_SCAN_INT_H
#define SIFT_COL_F2_STORE_SCAN_INT_H

#include "postgres.h"

#include "lib/ilist.h"

#include "col/f2/f2.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_pred.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/f2_view.h"
#include "col/f2/key_plan.h"
#include "col/f2/pend_del.h"
#include "col/f2/pin_win.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"

typedef enum Verdict
{
	V_NO,
	V_YES,
	V_MAYBE
} Verdict;

/* What decides, per host, which rows of a granule are there for the snapshot: granule (when not NULL) is called when the granule is loaded, row for each of its rows */
typedef struct RowVis
{
	void		(*granule) (SiftStorePart *p);
	bool		(*row) (SiftStorePart *p, uint16 r);
} RowVis;

struct SiftStoreScan
{
	const SiftStoreScanOpts *o;
	SiftF2Store *store;
	const RowVis *vis;
	MemoryContext cx;
	dlist_head	live;			/* the parts that hold a pin */
	MemoryContextCallback cb;	/* gives the pins back when the scan is abandoned */
	SiftF2Rel	f;
	SiftF2KeySet ks;
	SiftKeyPlan plan;
	SiftF2TimeWin tw;
	SiftTblVis	tvis;			/* a sift_table */
	SiftColDeltaScan *delta;	/* a sift_table */
	const SiftPendDelIndex *staged_px;	/* the deletes of the transaction not yet in a pending delete, as a set: a sift_col scan reads them from the stage, a parallel table scan is given them */
	struct MorselPart *morsel;	/* a parallel table scan: the part the last run of granules was of */
	bool	   *want;
	SiftPredSet *psets;			/* the constants of each IN predicate, hashed when the list is long */
	int		   *check_cols;		/* the wanted leaves that are not NaN leaves: the ones whose unsure cells a table scan refuses */
	int			ncheck;
	bool		wide;
	bool		possible;
	SiftF2Sink	sink;			/* sift_f2_store_scan_sink: where the granules a thread left go */
	void	   *sink_arg;
	bool		replaying;		/* the granules of the window are being read: none goes back to it */
	bool	   *offer_decided;	/* a sift_table with a granule sink: what no zone or skipping index of the table decides, per predicate (never a predicate) */
	const SiftZone **offer_zones;
	bool	   *offer_exceptions;
	const SiftPlanSnapshot *view;
	int			next;			/* the next partition of the view */
	List	   *run;			/* the SiftStorePart of the partitions of the run last given */
	List	   *pxs;			/* the SiftPendDelIndex of those partitions */
};

/* What a part holds while it is read: the run holds a SiftStorePart for every part of its partitions, only the one being read holds this */
typedef struct SiftPartRead
{
	MemoryContext gcx;
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	int			span;
	uint32		g;				/* the next granule of the span */
	const int16 *phys;			/* a sift_table with a granule sink: the column of the part that holds each leaf as it is (SiftF2GranOffer.phys) */
	SiftF2GranuleBuf *gb;
	const uint64 *tids;
	int		   *active;			/* the check_cols of the scan that carry tags in the granule loaded */
	int			nactive;
	ColView    *views;
	bool		pend_hit;
	bool		any_dead;
	uint64		dead[SIFT_DV_MASK_WORDS];
	uint64		live[SIFT_DV_MASK_WORDS];
	uint64		maybe[SIFT_DV_MASK_WORDS];
} SiftPartRead;

struct SiftStorePart
{
	SiftStoreScan *s;
	dlist_node	node;			/* in SiftStoreScan.live while the part is open */
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	const SiftPendDelIndex *px;
	SiftF2PartPin *pin;			/* held while the part is read, outside its context: an abandoned scan gives it back */
	MemoryContext cx;
	SiftPartRead *r;
	bool		started;
};

/* The views of the wanted columns of the granule loaded, from the columns the part holds, the column deltas and the cells a part lacks */
extern void sift_store_scan_load_views(SiftStorePart *p);

#endif							/* SIFT_COL_F2_STORE_SCAN_INT_H */
