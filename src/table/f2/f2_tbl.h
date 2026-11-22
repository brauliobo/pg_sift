/*
 * table/f2/f2_tbl.h - the sift_table access method on format major 2: the interfaces between the files of table/f2/ and the engine they sit on (col/f2/, core/f2/).
 *
 *   f2_ops.c      the table of the format and the host of the shared engine
 *   f2_am.c       slots, new relation files, TRUNCATE, copies, size estimate, index builds
 *   f2_scan.c     sequential, parallel, TID range, bitmap, TABLESAMPLE scans and TID fetches
 *   f2_dml.c      INSERT, COPY, ON CONFLICT, DELETE, UPDATE
 *   f2_mvcc.c     what a snapshot sees of a row, SERIALIZABLE, the visibility a BM25 index asks for
 *   f2_lock.c     the row locks and EvalPlanQual
 *   f2_vacuum.c   VACUUM and its freeze, ANALYZE
 *   f2_ddl.c      ALTER TABLE without a rewrite, the publication guard
 *   f2_blob.c     wide values
 *   f2_options.c  the per-table options
 *   f2_upgrade.c  sift.upgrade_table(), the one entry point of the table that is not a callback
 *
 * A TID is (vblock, offset), offset 1..SIFT_TBL_ROWS_PER_VBLOCK, vblock from the metapage counter and never reused: the address of a row is the same in every part, slab and
 * secondary index until the row is gone. No file of table/f2/ includes a file of Format 1.
 */
#ifndef SIFT_TBL_F2_H
#define SIFT_TBL_F2_H

#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "nodes/parsenodes.h"
#include "nodes/tidbitmap.h"
#include "storage/predicate.h"
#include "utils/snapshot.h"

#include "col/f2/compact.h"
#include "col/f2/delete.h"
#include "col/f2/delvec.h"
#include "col/f2/f2.h"
#include "col/f2/f2_batch.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_defaults.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_store.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "col/f2/pend_del.h"
#include "core/compat.h"
#include "core/f2/fsm.h"
#include "core/xid_cache.h"
#include "table/dispatch.h"
#include "table/f2/f2_blob.h"
#include "table/f2/f2_rowlock.h"
#include "table/wal.h"

#define SIFT_TBL_ROWS_PER_VBLOCK	MaxHeapTuplesPerPage
#define SIFT_TBL_VBLOCK_RESERVE		1024	/* vblocks taken from the metapage at once: one Block 0 lock per 297,984 rows */
#define SIFT_TBL_VBLOCK_BULK_RESERVE 8192	/* the same for a bulk insert: one Block 0 lock per 2,383,872 rows */
#define SIFT_TBL_OFFMASK_WORDS		((SIFT_TBL_ROWS_PER_VBLOCK + 64) / 64)
#define SIFT_TBL_SCAN_UNIT_GRANULES	16
#define SIFT_TBL_PARALLEL_SLACK		64

#ifndef SIFT_TBL_LOG_MAX_BYTES
#define SIFT_TBL_LOG_MAX_BYTES		((uint64) (MaxAllocSize / 2))
#endif

/* ---- addresses ---- */

static inline uint64
sift_tbl_tid_pack(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static inline void
sift_tbl_tid_unpack(ItemPointer tid, uint64 addr)
{
	ItemPointerSet(tid, (BlockNumber) ((addr & SIFT_PD_TID_MASK) >> 16), (OffsetNumber) (addr & 0xFFFF));
}

typedef struct SiftTblTidPool
{
	uint64		next_vblock;
	uint64		end_vblock;
	OffsetNumber next_off;
} SiftTblTidPool;

struct SiftTblBulkState;

/* The host-private part of a SiftF2Store of a table (SiftF2Store.host_data) */
typedef struct SiftTblF2Extra
{
	SiftTblTidPool pool;
	bool		initialized;
	bool		has_wide;		/* some attribute may hold a value over SIFT_TBL_WIDE_THRESHOLD */
	bool		has_partial_or_expr_unique;	/* a unique index has a predicate or an expression: its check of a conflict reads the values of the row, so a speculative insert writes the stage at once */
	Bitmapset  *key_attrs;		/* the attributes of the unique and primary key indexes, for the lock mode of an UPDATE */
	Bitmapset  *hot_blocking_attrs;	/* the attributes of every index but the summarising ones: an UPDATE that changes one is no column delta */
	Bitmapset  *summarized_attrs;	/* the attributes of the summarising indexes (BRIN) */
	Bitmapset  *protected_attrs;	/* the attributes of the sort key and those with more than one leaf (sift_col_delta_protected_attrs) */
	struct SiftTblBulkState *bulk;	/* the cells and the memory of a bulk insert, made by the first one (f2_bulk_ins.c) */
	SiftSchemaDefaults *defaults;	/* the fast defaults and the dropped attributes of the tuple descriptor (f2_defaults_tbl.c); NULL: none */
} SiftTblF2Extra;

static inline SiftTblF2Extra *
sift_tbl_extra(const SiftF2Store *store)
{
	return (SiftTblF2Extra *) store->host_data;
}

/* ---- the store of a table (f2_ddl.c, f2_options.c) ---- */

/* Builds the leaves, the key, the time columns and the part spec of a table from its tuple descriptor and its options (the host of sift_f2_store_open) */
extern void sift_tbl_f2_define_leaves(Relation rel, SiftF2Store *store);

/* ---- schema evolution (f2_ddl.c) ---- */

typedef enum SiftColMapMode
{
	SIFT_CMAP_DIRECT = 0,
	SIFT_CMAP_MISSING = 1,
	SIFT_CMAP_WIDEN_N64 = 2,
	SIFT_CMAP_RECAST = 3
} SiftColMapMode;

typedef struct SiftF2ColMap
{
	uint16		cur_ncols;
	bool		has_missing;
	int16	   *phys_col;
	SiftColMapMode *mode;
	uint8	   *old_scale;
	uint8	   *phys_kind;
	Datum	   *missing_val;
	bool	   *missing_null;
	SiftCell   *missing_cell;
} SiftF2ColMap;

extern SiftF2ColMap *sift_tbl_f2_build_col_map(const SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx);

/* The same map as the columns a compaction reads a part through (SiftF2HostOps.build_col_remap): NULL when the part has the columns of the store; checks the defaults of the part against the catalog first */
extern const SiftColRemap *sift_tbl_f2_build_col_remap(const SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx);

/* ---- fast defaults and dropped columns of a part (f2_defaults_tbl.c, col/f2/f2_defaults.h) ---- */

/* The defaults and the dropped attributes of the tuple descriptor, for the parts written from now on (SiftPartSpec.defaults); nothing when it has neither */
extern void sift_tbl_f2_defaults_attach(Relation rel, SiftF2Store *store);

/* SEC_SCHEMA_DEFAULTS of a part decoded (ERROR, data corrupted, when it is damaged); NULL when the part has none */
extern SiftSchemaDefaults *sift_tbl_f2_part_defaults(Relation rel, const SiftPart *part);

/* ERROR when the part was written under another fast default of an attribute than the catalog has now */
extern void sift_tbl_f2_defaults_verify(Relation rel, const SiftF2Store *store, const SiftPart *part);

/* The chunk to synthesize for leaf `leaf` of a part that does not have it: the constant of its fast default, or all null; no byte of the part is read */
extern void sift_tbl_f2_col_synth(const SiftF2Store *store, int leaf, SiftSynth *out);

/* The identities of the columns a part is written with, for its SEC_SCHEMA (SiftPartSpec.col_ids, written by col/granule/part_write.c when the host is a table); made in the context of the store */
extern SiftPartColIdentity *sift_tbl_f2_column_identities(const SiftF2Store *store, int *n);

/* Writes every slab of the table out as parts: a schema change then meets no row staged under the old tuple descriptor */
extern void sift_tbl_f2_flush_all_slabs(Relation rel);
extern void sift_tbl_f2_ddl_prepare(Relation rel);

/* SEC_SCHEMA of a part: the identities after the sort flags and the promotions (SIFT_SCHEMA_EXT_ATTNUM); NULL when the part has none (written by a column index) */
extern const SiftPartColIdentity *sift_tbl_f2_schema_identities(const char *schema, uint64 len, int *n);
extern uint32 sift_tbl_f2_identities_hash(const SiftPartColIdentity *ids, int n);

/* The statements around an ALTER TABLE, a RENAME or a SET SCHEMA of a table (the utility hook): the checks and the fence before, the publication guard after */
extern void sift_tbl_f2_ddl_before(Relation rel, AlterTableStmt *stmt);
extern void sift_tbl_f2_ddl_after(Relation rel, Node *parsetree);
extern void sift_tbl_f2_ddl_rename_column(Relation rel, const char *oldname, const char *newname);

/* EXT-04: ERROR 0A000 when the table is in a logical publication and pg_sift is not preloaded */
extern void sift_tbl_f2_guard_publication(Relation rel);

/* ---- options (f2_options.c) ---- */

#define SIFT_TBL_F2_OPTIONS			10	/* documented: the five of the storage and the five of the analytics that sift_col has under the same names */
#define SIFT_TBL_F2_HIDDEN_OPTIONS	13	/* of Format 1, accepted so that the scripts and the dumps of a table that was upgraded keep running */

extern const char *const sift_tbl_f2_option_names[];
extern const char *const sift_tbl_f2_hidden_option_names[];
extern int	sift_tbl_f2_option_count(void);
extern int	sift_tbl_f2_hidden_option_count(void);
extern void sift_tbl_f2_load_options(Relation rel, SiftF2Store *store, MemoryContext cx);
extern void sift_tbl_f2_apply_key_order(Relation rel, SiftF2Store *store);

/* sift.table_set_option on a table of format major 2: the value checked (false, with why) and, for an option of Format 1, the notice that Format 2.0 ignores it (not while restoring a binary upgrade) */
extern bool sift_tbl_f2_option_check_set(Relation rel, const char *name, const char *value, const char **why);

/* sift.table_reset_option: the name is one of the documented options or of Format 1 (no notice: a script that resets what it set before is not told off) */
extern bool sift_tbl_f2_option_known(const char *name);

/* sift.table_option_names() of a table of format major 2: the documented options */
extern Datum sift_tbl_f2_option_names_array(void);
extern char *sift_tbl_f2_option_default(const char *name, const char **setting);
extern char *sift_tbl_f2_options_rename_column(const char *spec, const char *oldname, const char *newname);
extern bool sift_tbl_f2_option_names_column(const char *spec, const char *colname);

/* ---- addresses and vblocks (f2_dml.c) ---- */

extern bool sift_tbl_f2_attr_equal(Form_pg_attribute att, Datum a, bool anull, Datum b, bool bnull);
extern void sift_tbl_f2_tid_next(Relation rel, SiftF2Store *store, ItemPointer tid);
extern void sift_tbl_f2_tid_reset(SiftF2Store *store);

/* Room for nrows addresses in the reserved range, taking SIFT_TBL_VBLOCK_BULK_RESERVE vblocks from the metapage when it has not (once for a bulk insert call, not for a row) */
extern void sift_tbl_f2_tid_ensure(Relation rel, SiftF2Store *store, uint64 nrows);

/* ---- bulk insert (f2_bulk_ins.c) ---- */

/* The rows of a multi_insert call (the direct route of col/f2/f2_batch.h: no publication, no row trigger, SIFT_BATCH_MIN_SLOTS rows or more) staged for the cold tier */
extern void sift_tbl_f2_bulk_insert(Relation rel, SiftF2Store *store, TupleTableSlot **slots, int nslots, CommandId cid);

/* The stage of the relation from here on writes its buffers as bounded parts of the cold tier (a bulk insert, or an UPDATE that holds a partition); the stage */
extern SiftTxnStage *sift_tbl_f2_stage_bulk(Relation rel, SiftF2Store *store);

/* The rows of a statement as one staged batch; frozen: written as frozen rows (a rewrite), else with the xid and the command of the statement */
typedef struct SiftTblF2Bulk
{
	Relation	rel;
	SiftF2Store *store;
	SiftTxnStage *stage;
	MemoryContext cx;
	bool		frozen;
	CommandId	cid;
	uint64		nrows;
} SiftTblF2Bulk;

extern void sift_tbl_f2_bulk_begin(SiftTblF2Bulk *b, Relation rel, CommandId cid, bool frozen);
extern void sift_tbl_f2_bulk_add(SiftTblF2Bulk *b, TupleTableSlot *slot, ItemPointer tid);
extern void sift_tbl_f2_bulk_end(SiftTblF2Bulk *b);

/* EXT-05: ERROR 54000 when a logical replication record of the change would pass SIFT_TBL_LOG_MAX_BYTES */
extern void sift_tbl_f2_log_guard(Relation rel, uint64 newlen, uint64 oldlen);

/* ---- extents ---- */

typedef struct SiftExtVec
{
	SiftExtent *ext;
	int			n;
	int			cap;
} SiftExtVec;

extern void sift_ext_vec_add(SiftExtVec *v, const SiftExtent *e);

/* ---- granules and parts (col/f2/f2_gbuf.h, f2_scan.c) ---- */


/* Finds the row at an address; for a dirty snapshot and an address a running insert has staged: false with *waiting set and the snapshot told whom to wait for (table/f2/f2_spec.h) */
extern bool sift_tbl_f2_locate_or_wait(Relation rel, SiftF2Store *store, uint64 tid, Snapshot snapshot, SiftF2Loc *loc, bool *waiting);

/* The slot of a row a running insert staged and has not written: every attribute null, the address set (the check of a unique index looks at whether the row is there) */
extern void sift_tbl_f2_staged_stub(TupleTableSlot *slot, uint64 tid);
/* The cursor of a scan, for ANALYZE: the number of granules of the table, the granule at an ordinal made current (false when no row of it is visible) and the next visible row of it */
extern uint64 sift_tbl_f2_scan_ngranules(TableScanDesc scan);
extern bool sift_tbl_f2_scan_goto_granule(TableScanDesc scan, uint64 ordinal);
extern bool sift_tbl_f2_scan_next_in_granule(TableScanDesc scan, TupleTableSlot *slot);
extern uint32 sift_tbl_f2_scan_granule_rows(TableScanDesc scan);

/* ---- visibility (f2_mvcc.c) ---- */

typedef struct SiftTblVis
{
	Relation	rel;
	Snapshot	snap;
	SiftXidCache xc;
	FullTransactionId snap_xmin;
	bool		serializable;	/* a row the snapshot cannot see may be a conflict out */
} SiftTblVis;

extern void sift_tbl_f2_vis_begin(SiftTblVis *v, Relation rel, Snapshot snap);

/* The rows of the granule the snapshot sees, as a bitmap of the granule's rows in sel; returns how many. pdel: the pending deletes of the partition the snapshot sees (or NULL) */
extern uint16 sift_tbl_f2_vis_granule(SiftTblVis *v, SiftF2GranuleBuf *gb, const SiftPendDelIndex *pdel, uint64 *sel);
extern bool sift_tbl_f2_vis_row(SiftTblVis *v, SiftF2GranuleBuf *gb, uint16 row, const SiftPendDelIndex *pdel);

/* The pending deletes of the partition the snapshot sees, as an index (NULL: none) */
extern SiftPendDelIndex *sift_tbl_f2_pending_index(Relation rel, const SiftPartitionEntry *entry, Snapshot snap);

/* SERIALIZABLE */
extern void sift_tbl_f2_predicate_scan(Relation rel, Snapshot snap);
extern void sift_tbl_f2_predicate_point(Relation rel, uint64 tid, Snapshot snap);
extern void sift_tbl_f2_conflict_in(Relation rel, uint64 tid);

/* The column delta (xmin, cid) is there for the snapshot of the visibility (f2_col_delta.h); the same rule as for the insert of a row, a snapshot of any kind */
extern bool sift_tbl_f2_delta_visible(SiftTblVis *v, TransactionId xmin, CommandId cid);

/* The rows of the virtual block whose offsets are set in offmask that the snapshot does not see are cleared from it (a BM25 index asks before it scores a row) */
extern void sift_tbl_f2_filter_page(Relation rel, Snapshot snap, BlockNumber vblock, uint64 *offmask);
extern uint64 sift_tbl_f2_nvblocks(Relation rel);

/* ---- locks (f2_lock.c) ---- */

extern void sift_tbl_f2_locks_init(void);
extern TM_Result sift_tbl_f2_lock_for_modify(Relation rel, uint64 tid, LockTupleMode mode, bool wait, Snapshot snap, CommandId cid, TM_FailureData *tmfd);
extern void sift_tbl_f2_record_update(Relation rel, uint64 old_tid, uint64 new_tid, CommandId cid);
/* The deleter of a row on disk and whether it was found: the xmax that VACUUM and the index deletion compare with the horizon */
extern void sift_tbl_f2_record_delete(Relation rel, uint64 tid, CommandId cid, bool changing_part);

/* ---- the callbacks of the table access method ---- */

extern const TableAmRoutine sift_tbl_v2_methods;
extern const SiftTableFormatOps sift_tbl_v2_ops;

extern const TupleTableSlotOps *sift_tbl_f2_slot_callbacks(Relation rel);
extern void sift_tbl_f2_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti);
extern void sift_tbl_f2_relation_nontransactional_truncate(Relation rel);
extern void sift_tbl_f2_relation_copy_data(Relation rel, const RelFileLocator *newrlocator);
extern void sift_tbl_f2_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, TransactionId OldestXmin, SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff,
												  MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead);
extern uint64 sift_tbl_f2_relation_size(Relation rel, ForkNumber forkNumber);
extern bool sift_tbl_f2_relation_needs_toast_table(Relation rel);
extern void sift_tbl_f2_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac);
extern double sift_tbl_f2_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, bool allow_sync, bool anyvisible, bool progress,
												 BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
extern void sift_tbl_f2_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, Snapshot snapshot, struct ValidateIndexState *state);

extern TableScanDesc sift_tbl_f2_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags);
extern void sift_tbl_f2_scan_end(TableScanDesc scan);
extern void sift_tbl_f2_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode);
extern bool sift_tbl_f2_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
extern void sift_tbl_f2_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid);
extern bool sift_tbl_f2_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
extern Size sift_tbl_f2_parallelscan_estimate(Relation rel);
extern Size sift_tbl_f2_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
extern void sift_tbl_f2_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
extern struct IndexFetchTableData *sift_tbl_f2_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM);
extern void sift_tbl_f2_index_fetch_reset(struct IndexFetchTableData *data);
extern void sift_tbl_f2_index_fetch_end(struct IndexFetchTableData *data);
extern bool sift_tbl_f2_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead);
extern bool sift_tbl_f2_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
extern bool sift_tbl_f2_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
extern void sift_tbl_f2_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);
extern bool sift_tbl_f2_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
extern TransactionId sift_tbl_f2_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
#if PG_VERSION_NUM >= 180000
extern bool sift_tbl_f2_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages, uint64 *exact_pages);
#else
extern bool sift_tbl_f2_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres);
extern bool sift_tbl_f2_scan_bitmap_next_tuple_17(TableScanDesc scan, TBMIterateResult *tbmres, TupleTableSlot *slot);
#endif
extern bool sift_tbl_f2_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate);
extern bool sift_tbl_f2_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate, TupleTableSlot *slot);

extern void sift_tbl_f2_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate);
extern void sift_tbl_f2_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate, uint32 specToken);
extern void sift_tbl_f2_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded);
extern void sift_tbl_f2_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate);
extern TM_Result sift_tbl_f2_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart);
extern TM_Result sift_tbl_f2_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
										  TM_FailureData *tmfd, LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes);
extern TM_Result sift_tbl_f2_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
										uint8 flags, TM_FailureData *tmfd);
extern void sift_tbl_f2_finish_bulk_insert(Relation rel, SiftTamOptions options);

extern void sift_tbl_f2_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy);
extern bool sift_tbl_f2_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream);
extern bool sift_tbl_f2_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows, TupleTableSlot *slot);

extern void sift_tbl_f2_vacuum_dead_tids(void *vac_ctx, const uint64 *tids, uint32 ntids);

/* The rows of OldTable (any format, as of now) written frozen into the empty NewTable: a rewrite under an exclusive lock (CLUSTER, VACUUM FULL, sift.upgrade_table); returns their number */
extern double sift_tbl_f2_rewrite_rows(Relation OldTable, Relation NewTable);

/* The first writer of a relation that has no blocks yet: the layout of Format 2.0 in block 0 (a table made with its layout is not asked) */
extern void sift_tbl_f2_init_layout(Relation rel);

/* ---- sift.upgrade_table (f2_upgrade.c) ---- */

extern Datum sift_upgrade_table(PG_FUNCTION_ARGS);

#endif							/* SIFT_TBL_F2_H */
