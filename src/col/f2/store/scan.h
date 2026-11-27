/*
 * col/f2/store/scan.h - the scan that serves the aggregates, the scopes, the projections and the hybrid scan of a Format 2.0 relation from its SiftF2Store, for a sift_col index and a sift_table alike.
 *
 * A sift_col index is scanned by its own driver (col/f2/scan/scan.c, which asks the heap about visibility); a sift_table by this one, whose rows are decided by the table's own versions (SEC_VERS, the
 * deletion vectors and the pending deletes) so that no heap page is read. Both hand the rows that every predicate accepted exactly to a SiftF2Sink (col/f2/sink/sink.h).
 */
#ifndef SIFT_F2_STORE_SCAN_H
#define SIFT_F2_STORE_SCAN_H

#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "utils/snapshot.h"

#include "col/col.h"
#include "col/f2/sink/sink.h"
#include "col/f2/store/store.h"

struct SiftTidBridge;

typedef struct SiftStoreScanOpts
{
	Relation	rel;			/* the sift_table, or the sift_col index when scan is set */
	SiftF2Store *store;
	Snapshot	snap;
	IndexScanDesc scan;			/* sift_col: the index scan, with its keys given to the rescan; NULL for a sift_table */

	const SiftColPred *preds;	/* a sift_table: the predicates, on leaves of the store, that every row handed to the sink satisfies */
	int			npreds;
	const int  *leaves;			/* the leaves the sink reads */
	const bool *nan_leaves;		/* a sift_table: [store->ncols], the leaves whose NaN the sink folds; a NaN in another leaf of a row it takes is refused */
	int			nleaves;
	const uint64 *skip_gens;	/* the parts (by generation, ascending) a projection or the GPU stands for: they are not read */
	int			nskip_gens;
	const struct SiftTidBridge *bridge;	/* a hybrid scan: the granules it misses are skipped and the rows not in it dropped */
	uint64	   *bridge_seen;
	uint64	   *bridge_skipped;
	const struct SiftPlanSnapshot *plan;	/* the manifest the caller made its own plan from: read as it is; NULL: the current one */
	bool		emit_maybe;		/* the sink takes the rows no predicate could decide on the stored values (SiftF2Batch.maybe) */
	struct SiftPinWin *pinwin;	/* a sift_table scan: the granules settled for the snapshot go to the window instead of the sink (col/f2/pin_win.h) */
	void		(*pinwin_done) (void *sink_arg);	/* after the last granule, with the scan still open: the owner of the window runs it and the scan replays what the threads left */
	SiftF2GranSink gran_sink;	/* a sift_table: offered each granule before it is loaded, as sift_f2_scan_set_gran_sink offers those of an index; NULL: none */
	void	   *gran_arg;
} SiftStoreScanOpts;

/* The rows of the relation to the sink: granules of up to 8192 rows, as sift_f2_scan_sink hands them */
extern void sift_f2_store_scan_sink(const SiftStoreScanOpts *opts, SiftF2Sink sink, void *sink_arg);

/*
 * The same rows pulled, a granule at a time and so with no more in memory than the granules in hand, for a sift_table and a sift_col index alike (the rows of an index are those its deletion vectors
 * and pending deletes leave: the reader asks the heap about the rest). A scan hands out runs of parts: a run is the parts of one partition, or, ordered, of every partition that starts at the same
 * lower bound, so that the rows of the runs come in key order and a reader that merges the parts of a run by the key (store->key_cols, store->spec.sort_flags) has the rows of the relation in key order.
 * The predicates, the leaves and the other fields of the options are those of sift_f2_store_scan_sink; scan is not used.
 */
typedef struct SiftStoreScan SiftStoreScan;
typedef struct SiftStorePart SiftStorePart;

/* In the current memory context, which must outlive the scan */
extern SiftStoreScan *sift_f2_store_scan_open(const SiftStoreScanOpts *opts);

/* The parts of the next run (a List of SiftStorePart, NIL after the last), whose granules the caller has taken before it asks again: the parts of the run before are closed by this call */
extern List *sift_f2_store_scan_next_run(SiftStoreScan *s, bool ordered);

/* The next granule of the part that has a row for the batch; valid until the next call for the part; false at its end. A part opens at its first call and holds no more than its granule */
extern bool sift_f2_store_part_next(SiftStorePart *p, SiftF2Batch *b);

extern void sift_f2_store_scan_close(SiftStoreScan *s);

/*
 * A parallel scan of a sift_table, as the one of a sift_col index (col/f2/sink/sink.h): the leader lists the units of the scan (the granules of a part the key ranges and the zone maps leave, each with
 * the descriptor of its part) and the deletes of its own transaction that are not written yet (the staged_dead of the plan, which every participant is given in staged_dead of the morsel begin);
 * every participant reads runs of granules [first, first + n) of a unit with the table's own versions, and hands the rows each predicate accepted exactly to the sink (closing the scan gives back the part it holds open). A transaction's rows not
 * written yet are written by the listing, so the units hold them.
 */
extern void sift_f2_store_par_plan(SiftStoreScan *s, SiftF2ParPlan *out);
extern void sift_f2_store_scan_morsel_begin(SiftStoreScan *s, const uint64 *staged_dead, uint32 nstaged);
extern void sift_f2_store_scan_morsel(SiftStoreScan *s, const SiftParPartRef *ref, uint32 first, uint32 n, SiftF2Sink sink, void *arg);

/* The key of a quals' comparison whose operand is a parameter: the value it has when the scan starts is the argument of key */
typedef struct SiftStoreRuntimeKey
{
	int			key;
	ExprState  *expr;
} SiftStoreRuntimeKey;

/* The quals of the statement's scan of a table: as the keys of an index scan (the predicates of a projection are made from them) and as the predicates on the leaves the store scan decides; possible false: no row qualifies. With runtime keys (nrt > 0) the predicates are those of the last sift_f2_store_bind_quals, made in cx */
typedef struct SiftStoreQuals
{
	ScanKeyData *keys;
	int			nkeys;
	SiftColPred *preds;
	int			npreds;
	bool		possible;
	MemoryContext cx;
	int			nrt;
	SiftStoreRuntimeKey *rt;
} SiftStoreQuals;

/* The same from the keys of an index scan of the table's attributes; out->keys is keys */
extern void sift_f2_store_key_preds(Relation rel, const SiftF2Store *store, const ScanKeyData *keys, int nkeys, MemoryContext cx, SiftStoreQuals *out);

/* quals: comparisons, IN lists and null tests of INDEX_VAR attributes (the attributes of the table) against constants or parameters, as agg/plan/plan.c keeps them; allocated in cx */
extern void sift_f2_store_compile_quals(Relation rel, const SiftF2Store *store, const List *quals, List *jsonqual, MemoryContext cx, SiftStoreQuals *out);

/* The values of the parameters of the quals as the scan that starts now has them, and the predicates made from them: before each start of the scan, as ExecIndexEvalRuntimeKeys is for an index scan */
extern void sift_f2_store_bind_quals(Relation rel, const SiftF2Store *store, SiftStoreQuals *q, ExprContext *econtext);

/*
 * What decides, from the descriptors and the predicates alone, whether a partition or a part can hold a row the scan wants, on the time columns: window is the closed range the predicates leave a
 * time column (use false: none); dirty: the column deltas of the partition patched that time column, so the bounds of its parts do not hold for it and nothing is pruned by them. The key_order columns
 * are never patched by a column delta (they are protected), so the marks and the key ranges prune whatever the deltas are.
 */
typedef struct SiftStoreTimeWindow
{
	bool		use;
	int64		lo;
	int64		hi;
} SiftStoreTimeWindow;

extern bool sift_store_time_may_hold(const SiftStoreTimeWindow *w, bool bounds_valid, int64 tmin, int64 tmax, bool dirty);

/* Whether the parts of generation gen are left out (gens ascending) */
extern bool sift_store_gen_skipped(const uint64 *gens, int n, uint64 gen);

#endif							/* SIFT_F2_STORE_SCAN_H */
