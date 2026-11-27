/*
 * col/f2/sink/ext.h - the rows of an index of format major 2 taken one batch at a time by the consumer instead of being pushed to a sink (col/f2/sink/sink.h): every granule of every part and the
 * slab of rows not yet in a part, decoded straight to the vectors of agg/vec.h for the leaves the consumer names, in slices of SIFT_VEC_LEN rows, with the rows the deletion vectors, the pending
 * deletes and the transaction's own deletes say are dead left out of live. Beside the columns it gives, for the leaves of a key of a jsonb document, the rows that have the key, and for an array
 * column (SIFT_COLK_GROUP) the layout of the elements of the rows of the slice and the element columns decoded for the whole granule.
 *
 * The consumer may tell the pull what its rows must satisfy (SiftF2PullSpec.pred, elem_pred): the partitions, parts and granules that hold no such row (by the key ranges, the time bounds, the key marks
 * and the zones of the leaves, col/f2/pull_prune.h) are not read at all. That is a hint: the batches of the granules that are read carry every row of them, and the consumer tests its rows.
 * A spec may name several array columns of a row (one group each), each read as its own layout and element columns. A group that names the lengths of an array and no element column reads no
 * element chunk: the layout is all the consumer gets.
 *
 * Visibility of the heap is the consumer's: live is what the index structures know, and a row of it may be a version the heap has since ended (a delete or an update is the index's only at the next VACUUM).
 */
#ifndef SIFT_COL_F2_SINK_EXT_H
#define SIFT_COL_F2_SINK_EXT_H

#include "postgres.h"

#include "utils/rel.h"
#include "utils/snapshot.h"

#include "agg/vec.h"
#include "agg/vexpr/vexpr.h"
#include "col/f2/f2.h"
#include "col/f2/sink/sink.h"
#include "col/granule/granule.h"

#define SIFT_F2_PULL_MAX_COLS	SIFT_VEXPR_MAX_REGS
#define SIFT_F2_PULL_MAX_MASKS	SIFT_VEXPR_MAX_MASKS
#define SIFT_F2_PULL_MAX_CHILD	16
#define SIFT_F2_PULL_MAX_GROUPS	4

/* A leaf the consumer wants: its decoding. An element leaf of text may ask for SVEC_DICT32: the codes of its dictionary where the granule holds it as one, its strings (SVEC_STRVIEW) where it does not */
typedef struct SiftF2PullCol
{
	int			leaf;
	SiftVecType type;
	int16		scale;
	bool		kind;			/* an element leaf: not its values but, as SVEC_I64 for each element, whether it holds a value (1), the member is a JSON null (7) or it is missing (0); the leaf is also a column of the spec that is not a kind */
} SiftF2PullCol;

/* A boolean the consumer wants: the leaf of a key of a jsonb document, whose chunks keep on which rows the key is there */
typedef struct SiftF2PullMask
{
	int			leaf;
} SiftF2PullMask;

/* An array column: the leaf of its lengths (SIFT_CENC_LIST_LEN) and its element leaves */
typedef struct SiftF2PullGroup
{
	int			leaf;
	int			nchild;
	SiftF2PullCol child[SIFT_F2_PULL_MAX_CHILD];
} SiftF2PullGroup;

typedef struct SiftF2PullSpec
{
	int			ncols;
	SiftF2PullCol col[SIFT_F2_PULL_MAX_COLS];
	int			nmasks;
	SiftF2PullMask mask[SIFT_F2_PULL_MAX_MASKS];
	int			ngroups;
	SiftF2PullGroup group[SIFT_F2_PULL_MAX_GROUPS];
	/*
	 * What the rows the consumer keeps satisfy, ANDed, on the leaves of the rows (npred) and on the element leaves of group 0 (nelem_pred, on the same element). The arrays are the consumer's and
	 * live as long as the pull. elem_pred only when a row none of whose elements passes gives the consumer nothing (an inner join, EXISTS), not for a left join: a granule in which no element
	 * can pass is not read, or is read with no element for the rows the elements cannot hold (the consumer reads their arrays from the heap).
	 */
	int			npred;
	const SiftColPred *pred;
	int			nelem_pred;
	const SiftColPred *elem_pred;
	/* A sink offered each granule of a part the predicates leave, before a byte of it is read (f2_sink.h): when it answers the granule from what the part stores it returns true and the pull skips it */
	SiftF2GranSink gran_sink;
	void	   *gran_arg;
} SiftF2PullSpec;

/*
 * The elements of the rows of a slice. layout is of the slice (its parents are the rows of the slice, its offsets those of the elements of the granule); a child column has a value for each
 * element of the granule: child[c] is the column of the spec, data and valid as of a SiftVec of n_elems values (valid: n_elems bits, NULL: all valid). child_type[c] is how it is decoded: the type of
 * the spec, but SVEC_STRVIEW for a column that asked for SVEC_DICT32 and whose chunk of this granule is not a dictionary; child_dict[c] is then NULL. poison: the rows of the slice one of whose
 * arrays the columns cannot hold (the document was not an array, a list longer than the index keeps, or the rows come from the slab, which keeps no layout): the consumer reads them from the heap.
 */
typedef struct SiftF2ElemStream
{
	SiftArrayLayout layout;
	uint32		n_elems;
	const void *child_data[SIFT_F2_PULL_MAX_CHILD];
	const uint64 *child_valid[SIFT_F2_PULL_MAX_CHILD];
	SiftVecType child_type[SIFT_F2_PULL_MAX_CHILD];
	const SiftGranuleDict *child_dict[SIFT_F2_PULL_MAX_CHILD];
	const uint64 *poison;
	const uint64 *doc_valid;
} SiftF2ElemStream;

typedef struct SiftF2ExtBatch
{
	uint16		n;
	const SiftVec *col;
	const SiftVexprMaskIn *masks;
	const uint64 *tid;
	const uint64 *live;
	const SiftF2ElemStream *elem;	/* one for each group of the spec; NULL when it names none */
} SiftF2ExtBatch;

/* What the predicates of the spec left of the index, and how the element columns were decoded */
typedef struct SiftF2PullStats
{
	int64		partitions_skipped;		/* by the key range of the partition, or by the time bounds of its parts when it has no slab */
	int64		parts_skipped;			/* by the key range or the time bounds of the part */
	int64		granules_key_skipped;	/* of the parts read: outside the key marks, or the zones of the lead key column */
	int64		granules_zone_skipped;	/* by the zones of the leaves of pred */
	int64		granules_elem_skipped;	/* by the zones of the element leaves of elem_pred: no element passes, and no row the layout cannot hold is live: nothing of the granule was read */
	int64		granules_elem_elided;	/* the same, but a live row the layout cannot hold: its columns were read, and no element chunk */
	int64		granules_read;
	int64		child_chunks;			/* the chunks of element columns asked of the storage: none when the spec names the lengths and no element column, or the granule has no element */
	int64		child_dict_granules;	/* element columns decoded as dictionary codes, and as strings in a granule whose chunk is no dictionary */
	int64		child_str_granules;
} SiftF2PullStats;

typedef struct SiftF2Pull SiftF2Pull;

/* A scan of the index over the snapshot (the transaction's staged rows are flushed first); every part and every slab the predicates of the spec leave */
extern SiftF2Pull *sift_f2_pull_begin(Relation index, Snapshot snap, const SiftF2PullSpec *spec);
/* The next slice, valid until the next call; NULL after the last */
extern const SiftF2ExtBatch *sift_f2_pull_next(SiftF2Pull *p);
/* The payload (a jsonb datum, len bytes) of row `row` of the slice just given, of column i of the spec when it is a leaf of a key of a document and the row holds a value of a JSON type the leaf cannot store (it is NULL in the column); NULL for any other row. Valid until the next slice */
extern const char *sift_f2_pull_payload(SiftF2Pull *p, int i, uint16 row, uint32 *len);
extern const SiftF2PullStats *sift_f2_pull_stats(const SiftF2Pull *p);
extern void sift_f2_pull_end(SiftF2Pull *p);

/* Whether the parts of every partition are one sorted run and no row is in a slab: the order a scan in the order of the key gives (the order of the partitions is that of their keys) */
extern bool sift_f2_pull_single_runs(Relation index);

#endif							/* SIFT_COL_F2_SINK_EXT_H */
