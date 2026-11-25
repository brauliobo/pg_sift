/*
 * col/f2/v6/proj/host.h - what the executors of the projections (f2_v6_proj_exec.h, f2_v6_proj_sort_exec.h) ask of the relation they read, for a sift_col index and a sift_table alike: the parts of
 * every partition with the host's view of each one's visibility, the rows that have died in a part since its projection part was written, and the rows of everything a projection does not stand
 * for. A sift_col host has the heap, whose visibility map settles the addresses of a part and whose tuples vet every row a scan hands out; a sift_table has neither (its visibility is native and
 * complete). Every read is of one SiftPlanSnapshot the caller pins, so the parts a projection stands for and the rest of the relation are the same version of it whatever commits meanwhile.
 */
#ifndef SIFT_COL_F2_V6_PROJ_HOST_H
#define SIFT_COL_F2_V6_PROJ_HOST_H

#include "access/skey.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "col/f2/v6/proj/pred.h"
#include "col/f2/cache.h"
#include "col/f2/proj/proj.h"
#include "col/f2/store/store.h"
#include "col/f2/del/pend.h"

typedef struct SiftV6ProjHost
{
	Relation	rel;			/* the sift_col index, or the sift_table */
	SiftF2Store *store;
	Relation	heap;			/* a sift_col: its heap; NULL for a sift_table */
	Snapshot	snap;
	ScanKey		keys;			/* a sift_col: the keys of the statement's scan, which decide its predicates on the rows the projection does not stand for */
	int			nkeys;
	const SiftProjPred *preds;	/* the same predicates for the cells of a projection; a sift_table's own scan has no keys, so these decide its rows too */
	int			npreds;
} SiftV6ProjHost;

/*
 * The parts of every partition one after the other with the facts the rules of f2_proj.h are made of: the partition each is in, the host's view of its visibility, the attributes the
 * column patches of the part touch (a sift_table), the pending deletes of its partition the snapshot sees, and for an AGG part on a sift_col its heap blocks that are not all-visible.
 */
typedef struct SiftV6BaseParts
{
	SiftPartDesc *part;
	uint32	   *part_id;
	int		   *part_seg;		/* the partition of each part */
	SiftProjHostVis *vis;
	uint64	   *cd_bloom;
	int			n;
	int		   *seg_start;		/* nseg + 1 entries: the first part of each partition */
	const SiftPendDelIndex **seg_pdel;	/* of each partition: NULL when the snapshot sees no pending delete in it */
	BlockNumber *dirty_blk;		/* the not all-visible blocks of every part, one part after the other */
	uint32	   *dirty_off;		/* n + 1 entries */
	int			nseg;
} SiftV6BaseParts;

extern SiftV6BaseParts sift_v6_host_base(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, SiftProjKind kind);

/* The rows of part i that the cells of its projection part still count and the snapshot does not see: the deletion vector's, the pending deletes', the heap's, each row once; fn takes a row of the relation */
extern void sift_v6_host_overlay_rows(const SiftV6ProjHost *h, const SiftV6BaseParts *b, int i, const SiftProjLink *link, SiftProjRowFn fn, void *arg);

/* The addresses the deletion vector of part i has dead for the snapshot; NULL when none (a SORT part is read with them left out) */
extern SiftPendDelIndex *sift_v6_host_dv_dead(const SiftV6ProjHost *h, const SiftV6BaseParts *b, int i);

/* A row the scan found visible: its address and one cell for each column of the relation, of which only the leaves asked for are set; valid until the callback returns */
typedef void (*SiftV6RowFn) (void *arg, uint64 tid, const SiftCell *row);

/*
 * Every visible row of the relation that is not in a part of the generations given (ascending): the parts the projection does not stand for, the slabs, whatever the statement's
 * predicates accept, all of the plan given. false: a row could not be read as cells (a column a schema change left in another shape), and the projection must not be used.
 */
extern bool sift_v6_host_scan_rest(const SiftV6ProjHost *h, const SiftPlanSnapshot *plan, const uint64 *skip_gens, int nskip, const int *leaves, int nleaves, SiftV6RowFn fn, void *arg);

#endif							/* SIFT_COL_F2_V6_PROJ_HOST_H */
