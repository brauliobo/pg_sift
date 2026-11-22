/*
 * col/v1/amscan/amscan_int.h - what the files of the index scan share
 * the statics one file of the scan calls from another
 */
#ifndef SIFT_COL_AMSCAN_INT_H
#define SIFT_COL_AMSCAN_INT_H

#include "col/v1/point.h"
#include "col/am/preds.h"

/*
 * One scan of the index, as a bitmap (amgetbitmap) or tuple by tuple (amgettuple): the source, the predicates of the keys and, for amgettuple, the rows of
 * the unit being returned. Everything but the heap relation lives in cxt, which a rescan resets.
 */
#define COL_LAZY_FIRST	16

#define COL_LAZY_MAX	512

typedef struct ColScanOpaque
{
	SiftColScanHeader hdr;		/* first: the routing of the scan callbacks (col/am/format_ops.h) */
	MemoryContext cxt;
	MemoryContext batch;		/* the values of the unit being returned */
	MemoryContext scx;			/* the source, which a rescan keeps for the next probe while the snapshot and the segments are the same (col_scan_source) */
	Relation	heap;
	int64		dirty_rows;		/* the rows of units with a dead row that the ordered sources read (the other source keeps its own count): counted in sift.stats */
	SiftColSource *src;
	Snapshot	ssnap;			/* the snapshot the source was opened with, and the flags */
	uint32		sflags;
	SiftColPred *preds;
	SiftKeyRange keys;			/* the keys the predicates confine the scan to, when they do (col_scan_begin) */
	int			npreds;
	bool		started;
	bool		unsat;			/* a key no row can satisfy */
	bool		recheck;
	int		   *first;			/* first[attno]: the leaf of attribute attno */
	int			natts;
	SiftColDef *defs;
	uint32		n,				/* the rows of the unit */
				pos,
				cap;
	ItemPointerData *tids;
	ItemPointerData *dtids;		/* the rows a point directory of heap addresses lists, returned before the units' (no unit read for them) */
	uint32		ndtids;
	ItemPointerData *ltids;		/* the addresses of the lookup handed over as the scan asks (SIFT_COLO_POINT_LAZY), nlazy of them in the last chunk; NULL once all are */
	uint32		nlazy;
	Datum	   *vals;			/* n * natts, only when the executor wants index tuples */
	bool	   *nulls;
	IndexTuple	itup;
	bool		project;		/* an index-only scan forms its tuples from the columns of want (the attribute numbers of the index the plan reads) only (sift_col_ioss_want) */
	Bitmapset  *want;
	bool		ordered;		/* the plan wants the rows in the order of the first column, in the direction of the scan (ordered.c) */
	bool		ordpair;		/* ... in the order of the first and the second column (ordered: set as well) */
	IndexFetchTableData *fetch;	/* the heap, for a string the index could not keep (an overflow value) */
	TupleTableSlot *slot;
	/*
	 * The index tuple of every row of an index-only scan whose returned columns are all fixed-size and by value is one tuple, formed once (the other columns NULL),
	 * whose values are replaced for each row: nothing is allocated or formed for a row. fast: 0 not decided, 1 yes, -1 no.
	 */
	int			fast;
	IndexTuple	ftup;
	int			nfast;			/* the returned columns: attribute, offset in the tuple, vector of the unit being returned */
	int			fatt[INDEX_MAX_KEYS];
	uint16		foff[INDEX_MAX_KEYS];
	SiftColVec	fvec[INDEX_MAX_KEYS];
	bool		funit;			/* the unit being returned is in fvec (no NULL-valued overflow in it) */
	struct ColOrd *ord;			/* an index-only scan in the order of the first column (ordered.c) */
	struct SiftTombLog *log;	/* the tombstone log of an index with track_changes, as of the start of the scan, when the snapshot is an MVCC one and the log has entries (amscan.c, col_tomb_hides) */
} ColScanOpaque;


/* ordered.c: the scan in the order of the first column, told by the direction of the scan; ordpath.c offers it to the planner */
extern int	sift_col_ordered_pair_mode(Oid indexoid, double tuples);
extern bool sift_col_ordered_whole(Oid indexoid);
extern bool sift_col_ordered_type(Oid type);
extern bool sift_col_ordered_next(IndexScanDesc scan, ScanDirection dir);
extern void sift_col_ordered_end(ColScanOpaque *so);
extern void sift_col_ordered_mark(ColScanOpaque *so);
extern void sift_col_ordered_restore(ColScanOpaque *so);
extern bool sift_col_unordered_next(IndexScanDesc scan);
extern bool sift_col_unordered_batch(IndexScanDesc scan, uint32 *n, const ItemPointerData **tids, const int64 **vals, const uint64 **nulls, const int64 **vals2, const uint64 **nulls2);
extern void sift_col_add_ordered_paths(PlannerInfo *root, RelOptInfo *rel);


/*
 * What the estimates of one index path read of the index: the statistics and zones its cache holds, and the first leaf of each column (a copy: a catalog read of
 * the estimates can reload the relcache entry, and with it the array of the entry)
 */
typedef struct ColEstimate
{
	const SiftColLeafStats *ls;
	const SiftColZones *z;
	int			first[INDEX_MAX_KEYS + 1];
	int			nleaves;
} ColEstimate;

extern double	zones_kept(PlannerInfo *root, IndexPath *path, const ColEstimate *e);

extern double	sets_kept(PlannerInfo *root, IndexPath *path, const ColEstimate *e);

extern double	point_kept(PlannerInfo *root, IndexPath *path, const ColEstimate *e, double units, int *nprobes, double *tid, double *extra);

#endif