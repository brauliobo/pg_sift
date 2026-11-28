/*
 * col/f2/sink/ext_int.h - the state of a pull (col/f2/sink/ext.h) and its host: an index walks the partitions, parts, granules and slabs of its manifest and leaves the visibility of its rows to the
 * consumer (ext.c); a sift_table reads the granules of its store, whose versions, column deltas and deletes decide what the snapshot sees (ext_table.c). Both give a granule the same way: its rows,
 * those live, those poison and the layout and element columns of each group, from which the slices are cut.
 */
#ifndef SIFT_COL_F2_SINK_EXT_INT_H
#define SIFT_COL_F2_SINK_EXT_INT_H

#include "postgres.h"

#include "col/f2/col_delta/idx.h"
#include "col/f2/del/pend.h"
#include "col/f2/pull_prune.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "col/f2/sink/ext.h"
#include "core/xid_cache.h"

/* the elements of one array column of the granule being read (a group of the spec) */
typedef struct PullGroup
{
	bool		has_layout;
	SiftArrayLayout lay;
	uint64	   *doc_valid;
	void	   *child_data[SIFT_F2_PULL_MAX_CHILD];
	uint64	   *child_valid[SIFT_F2_PULL_MAX_CHILD];
	SiftVecType child_type[SIFT_F2_PULL_MAX_CHILD];
	SiftGranuleDict *child_dict[SIFT_F2_PULL_MAX_CHILD];
	uint32		n_elems;
} PullGroup;

/* How the pull reaches the granules of its host: the next one (false at the end), the columns of a slice of it and the payload of an exception of a row of it (at: the row in the granule) */
typedef struct SiftF2PullHost
{
	bool		(*advance) (SiftF2Pull *p);
	void		(*fill_slice) (SiftF2Pull *p, uint16 off, uint16 cnt);
	const char *(*payload) (SiftF2Pull *p, int i, uint32 at, uint32 *len);
	void		(*end) (SiftF2Pull *p);	/* NULL: nothing to give back */
	bool		visible;		/* the rows of live are those the snapshot sees: the consumer asks no heap about them */
} SiftF2PullHost;

typedef struct PullTable PullTable;

struct SiftF2Pull
{
	Relation	index;			/* the index, or the sift_table */
	const SiftF2PullHost *host;
	PullTable  *tbl;			/* a sift_table: its scan and the granule it gives */
	const SiftCellKind *kinds;	/* per leaf of the host */
	const bool *key_leaf;		/* per leaf: the leaf of a top-level key of a jsonb document */
	Snapshot	snap;
	SiftXidCache xc;
	const SiftPlanSnapshot *plan;
	SiftF2PullSpec spec;
	MemoryContext cx;			/* the scan */
	MemoryContext ecx;			/* the partition entry: its pending deletes and its slab */
	MemoryContext pcx;			/* the part: its header, directory and deletion vector */
	MemoryContext gcx;			/* the granule or the slab: its record, ids and decoded elements */
	const SiftPendDelIndex *staged_px;

	int			ent;			/* the next partition entry */
	int			pi;				/* the next part of its list */
	bool		slab_pending;
	const SiftPartitionEntry *e;
	const SiftPlanPartList *list;
	SiftPendDelIndex *px;

	SiftPart   *part;
	const SiftPartDesc *pd;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	SiftTidIndexEntry *tix;
	SiftColDeltaPartMap *cd;	/* what the column deltas of the part say the snapshot sees of it */
	const SiftColDeltaGranMap *gm;	/* of the granule being read: NULL when the snapshot sees no patch of it, and for a slab */
	uint32		g;				/* the granule being read */
	SiftF2PullPrune prune;
	SiftPartitionEntry unit_e;	/* a pull of units: the partition and the part of the unit being read, the part kept open for the next unit of it */
	SiftPlanPartList unit_list;
	SiftPartDesc unit_pd;
	SiftPart   *unit_part;
	SiftF2PullStats stats;

	uint32		n;				/* the rows of the granule or slab being sliced, and the next slice's first */
	uint32		off;
	bool		is_slab;
	SiftLazyGranule lg;
	SiftGranuleView gv;
	SiftRowBuf *rb;
	const uint64 *tids;
	uint64	   *live;
	uint64	   *poison;
	PullGroup	grp[SIFT_F2_PULL_MAX_GROUPS];
	SiftGranVecCursor cur[SIFT_F2_PULL_MAX_COLS];	/* the cursor of each column of the granule, opened by its first slice: the chunk is opened once for all of them */
	bool		cur_on[SIFT_F2_PULL_MAX_COLS];
	SiftGranVecCursor child_cur[SIFT_F2_PULL_MAX_GROUPS * SIFT_F2_PULL_MAX_CHILD];	/* the strings of an element column live as long as its cursor */
	int			nchild_cur;

	SiftVec		vec[SIFT_F2_PULL_MAX_COLS];
	uint64	   *valid[SIFT_F2_PULL_MAX_COLS];
	int64	   *bools[SIFT_F2_PULL_MAX_COLS];	/* a text column of a bool leaf: the booleans of the slice its `->>` texts are made of (NULL: any other column) */
	SiftVexprMaskIn masks[SIFT_F2_PULL_MAX_MASKS];
	uint64	   *mbits[SIFT_F2_PULL_MAX_MASKS];
	SiftF2ElemStream es[SIFT_F2_PULL_MAX_GROUPS];
	SiftF2ExtBatch out;
	uint16		slice_off;		/* the first row of the slice in the granule or the slab */
	uint8	   *xtag[SIFT_F2_PULL_MAX_COLS];	/* the JSON type each row of the granule holds of a leaf of a key of a document, read when a payload is asked of the column */
};

/* ext.c: the `->>` text of a boolean of a bool leaf; a value a vector of a column cannot hold (the rows of the granule are then the consumer's, or the scan has no answer); every row of the granule poison */
extern SiftStrView sift_f2_pull_bool_text(int64 b);
extern void sift_f2_pull_unholdable(SiftF2Pull *p);
extern void sift_f2_pull_poison_granule(SiftF2Pull *p);
extern bool sift_f2_pull_holds_payload(uint8 tag);

/* ext_table.c: the host of a sift_table (p->host), its store scan opened on the leaves of the spec */
extern void sift_f2_pull_table_begin(SiftF2Pull *p);

#endif							/* SIFT_COL_F2_SINK_EXT_INT_H */
