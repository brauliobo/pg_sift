/*
 * col/am.h - the glue of the sift_col index access method: the per-relation cache, the conversion
 * of index tuples into column rows, and the entry points of the other glue files.
 */
#ifndef SIFT_COL_AM_H
#define SIFT_COL_AM_H

#include "access/amapi.h"
#include "executor/execdesc.h"
#include "datatype/timestamp.h"

#include "col/pg.h"
#include "col/seg.h"
#include "col/enc.h"
#include "col/preagg.h"
#include "col/wbuf.h"

/* The relcache entry's amcache: leaves, shredder, insert hint and a scratch context for aminsert */
#define SIFT_COL_CUT_MAX 4

typedef struct SiftColRel
{
	SiftColSchema *schema;
	SiftColIndexMeta meta;		/* cols point into schema->cols; stats zero */
	int			natts;
	int		   *first;			/* [natts] id of the first leaf of attribute i */
	int		   *nleaves;		/* [natts] */
	bool	   *json;			/* [natts + 1] the attribute is a shredded jsonb */
	bool	   *name;			/* [natts + 1] the attribute is a name (a fixed buffer, not a varlena) */
	bool	   *ret;			/* [natts + 1] the attribute is a text, varchar or uuid column named by return_columns */
	SiftColShredAdapter *shred;
	SiftCcPolicy *policy;		/* [ncols] compression policy of each leaf (the compression option) */
	SiftCPaggSpec pagg;			/* the pre-aggregates of every unit the index writes (the preagg option, docs/COL-PREAGG.md) */
	SiftCWbufHint hint;
	int			cut[SIFT_COL_CUT_MAX];	/* the leaves of the option key_cut, most significant first; ncut 0: units are cut by superblock */
	int			ncut;
	uint32		cut_rows;		/* rows a key-cut unit aims at (the option key_cut_rows), 0: as many as an average superblock holds */
	MemoryContext rowcxt;		/* reset after every aminsert */
} SiftColRel;

/* Are the units this index writes now cut by key? The option names the key, and sift.col_format_minor lets minor 17 be written (a lowered setting writes superblock units again, for a rollback) */
static inline bool
sift_col_key_cut(const SiftColRel *r)
{
	return r->ncut > 0 && sift_col_format_minor >= SIFT_COL_FORMAT_PRESENCE;
}

/*
 * The on-demand refresh of the rollups (col/rollup.c): a scan asks for the segment gen of index to be rewritten with the rollups the index writes now.
 * Writes no page; false when the request was not taken (switched off, a standby). The GUC sift.col_rollup_on_demand.
 */
extern bool sift_col_rollup_on_demand;
extern bool sift_cluster_advice;
extern bool sift_col_rollup_worker;
extern bool sift_col_rollup_request(Relation index, uint64 gen);
/* A NOTICE for each cap of the options (cube(...) groups per unit, key_columns) that leaves 10% of the units or more without a histogram or the keys of a leaf; after a build and a refresh */
extern void sift_col_report_caps(Relation index);
extern void sift_col_report_key_cut(Relation index);
/* The default of key_columns (col/keycols.c): before a build, the columns chosen and written to the option (true: the build keeps the choice and says so); after it, those that hold a key list or bitmap in half of the units, and a NOTICE */
#define SIFT_COL_AUTO_KEEP_PCT	50		/* a column that holds no key list or bitmap in this share of the units, or more, is not kept; the cube answers a column it holds in as many */
extern bool sift_col_auto_keys(Relation heap, Relation index);
typedef enum SiftAutoCut
{
	SIFT_AUTOCUT_NONE,
	SIFT_AUTOCUT_CUT,			/* key_cut = 'auto: col' was written */
	SIFT_AUTOCUT_ADVISED		/* a key is worth cutting by, but the index would grow: the NOTICE says so */
} SiftAutoCut;
extern SiftAutoCut sift_col_auto_cut(Relation heap, Relation index);
extern bool sift_col_options_key_cut_auto(Relation index);
/* a column name that looks rewritten often (a timestamp of the last change, a counter, a version): an UPDATE of it is not a HOT update while the index covers it (col/build.c) */
extern bool sift_col_volatile_name(const char *name);
#define SIFT_COL_AUTO_ADD_PCT	40		/* a merge of every segment adds a key list to a column the cube holds in fewer of the units than this; the report drops one it holds in SIFT_COL_AUTO_KEEP_PCT or more, so a column that sits between is not added and dropped in turn */
extern bool sift_col_auto_keys_merge(Relation index, const SiftSegEntry *in, int n);
extern void sift_col_pagg_rewrite(Relation index, const SiftSegEntry *in, int n, SiftCPaggSpec *spec);
/* paths = 'auto' (col/keycols.c, col/suggest.c): before a build, the paths read from a sample of the jsonb documents and written to the option as 'auto: spec' */
#define SIFT_COL_AUTO_PATHS_SAMPLE	2000
extern void sift_col_auto_paths(Relation heap, Relation index);
/* preagg = 'auto' or none (col/keycols.c): before a build of an index with more plain columns than the cube holds, the columns ranked by data and written to the option as 'auto: a, b' */
extern void sift_col_auto_cube(Relation heap, Relation index);
/* rollup_dims = 'auto' (col/keycols.c): the columns whose combinations fit the rollup of the rows of a unit, written to the option as 'auto: a, b' */
extern void sift_col_auto_rows(Relation heap, Relation index);
/* array_lengths = auto (col/keycols.c): written as on for an index with an array column, before the build */
extern void sift_col_auto_arrlen(Relation index);
/* Before a build with order_array_columns unset or 'auto': the columns that get the global order array, written to the option */
extern void sift_col_auto_gorder(Relation heap, Relation index);
extern void sift_col_auto_point(Relation heap, Relation index);
extern void sift_col_set_option(Relation index, const char *name, const char *value);
extern char *sift_col_suggest_spec(Relation heap, AttrNumber attno, const char *col, int sample, int *ncols, int *ngroups);
extern const char *sift_col_options_paths_raw(Relation index);
extern bool sift_col_paths_auto(const char *opt);
extern bool sift_col_paths_redecide(Relation index);
extern void sift_col_auto_keys_report(Relation index);
/* The types of the columns the default of key_columns looks at (sift_cset_pres_leaf_ok), in the words of its NOTICE */
#define SIFT_KEY_TYPES "int2, int4, int8 or date"
/* The columns the default of key_columns looks at: a foreign key or *_id column of those types that is not unique, and a column that is unique alone */
extern bool sift_col_key_shaped(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d);
/* ... of the heap column hatt itself (the keys of a sift_table's columns: the leaf's attno) */
extern bool sift_col_key_attr(Relation heap, AttrNumber hatt, const SiftColSchema *schema, const SiftColDef *d);
extern bool sift_col_leaf_unique(Relation heap, Relation index, const SiftColDef *d);
#ifdef SIFT_TESTING
extern int	sift_test_col_rollup_panic;
extern int	sift_test_col_rollup_panic_armed;
extern bool sift_test_col_merge_panic;
extern int	sift_test_col_refresh_sleep_ms;		/* sift.test_col_refresh_sleep_ms: the refresh of a segment sleeps this long once the new segment is written */
extern int	sift_test_col_refresh_sleep_armed;
extern int	sift_test_col_floor_sbs;		/* sift.test_col_floor_sbs: the superblocks a promotion or merge must reach for sift.col_unit_floor to apply (0 = 16) */
#endif

/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a preagg option ("default", "off", "cube(dim: measure, ...; ...)"), as far as it can tell without the index */
extern void sift_col_validate_preagg(const char *value);
extern void sift_col_validate_rollup(const char *value);
extern void sift_col_validate_rollup_dims(const char *value);
extern void sift_col_validate_calendar_columns(const char *value);
extern void sift_col_validate_key_columns(const char *value);
extern void sift_col_validate_order_columns(const char *value);
extern void sift_col_validate_order_array_columns(const char *value);
extern void sift_col_validate_order_array_group(const char *value);
extern void sift_col_validate_hll_columns(const char *value);
extern void sift_col_validate_point_columns(const char *value);
extern void sift_col_validate_point_tid_columns(const char *value);
extern void sift_col_validate_count_columns(const char *value);
extern void sift_col_validate_return_columns(const char *value);
/* Whether attribute a (leaves of it, a shredded jsonb or not) is a text, varchar or uuid column a tuple scan can return */
extern bool sift_col_returns_str(TupleDesc desc, int a, int nleaves, bool json);
/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a compression option ("policy" or "policy; column=policy; ...") */
extern void sift_col_validate_compression(const char *value);
/* The storage class of a type the table AM holds as one leaf (false: not natively); the scale of a numeric(p, s) leaf that is one INT leaf, p <= 18 and 0 <= s <= 18 (false: it is not) */
extern bool sift_col_leaf_store(Oid typid, SiftColStore *store);
extern bool sift_col_numeric_scale(Form_pg_attribute att, uint8 *scale);
/*
 * The leaves of one attribute of a sift_col index (col/types.c): defs[0 .. result) with the type, storage class, scale, lanes, lane and path set;
 * the caller sets id and attno (parent is -1). A range or network column also gets the lanes of its containment operators (col/shape.c). defs has room for SIFT_COL_MAX_LANES. arrlen: an array keeps its lengths as two lanes (option array_lengths). ERROR for a type the engine does not hold.
 */
/* An array type proper (not int2vector, oidvector, a range or a domain): only its image has lengths */
extern bool sift_col_is_array_type(Oid typid);
extern int	sift_col_att_leaves(Form_pg_attribute att, SiftColDef *defs, int arr);
/* The values of one datum for the leaf def and the leaves after it (its lanes): v[0 .. max(def->lanes, 1)); strings are palloc'd or point into the datum */
extern void sift_col_datum_values(const SiftColDef *def, Datum d, bool name, SiftColValue *v);

/* Built on first use from the index definition (ERROR for an unsupported type or a jsonb attribute without paths) */
extern SiftColRel *sift_col_rel(Relation index);
/*
 * The same in mcxt, for a caller that holds it across a lock or an expression of the index: a reload of the relcache entry (any lock taken
 * meanwhile may cause one) frees the one sift_col_rel returns, which stays only for what the caller reads before it does either.
 */
extern SiftColRel *sift_col_rel_private(Relation index, MemoryContext mcxt);

/* Key-cut units */
extern bool sift_col_index_keyed(Oid indexoid);
extern uint32 sift_colk_unit_rows(uint32 aim);
extern uint32 sift_colk_run_rows(Relation index, SiftColBatch *const *in, int n);
/* the rows of a unit of a key that fills at least two such units and is sorted by a second leaf (below ~2,000 the per-unit bytes outgrow what a narrower zone saves) */
#define SIFT_COLK_SECOND_ROWS	4096
extern uint32 sift_colk_aim_rows(Relation index);
extern uint64 sift_colk_heap_sbs(Relation index);
/* Called with NULL for the writer of the next output segment, and after each unit with the writer in hand: returns the writer to go on with (NULL: it was finished, the next unit asks for one) */
typedef SiftColSegWriter *(*SiftColCutFn) (void *arg, SiftColSegWriter *w);
extern int	sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows, MemoryContext cx);
extern int	sift_colk_merge(SiftColSegWriter *w, const SiftSegEntry *ent, SiftColSegment *const *seg, int n, SiftColFreezeFn freeze, void *freeze_arg, const SiftColSchema *schema,
							const int *leaf, int nleaf, uint32 unit_rows, uint64 memory_kb, MemoryContext cx, SiftColCutFn cutfn, void *cutarg);

/*
 * numeric(p <= 18, s) leaves hold the number times 10^s as their INT value (SiftColDef.scale). A predicate constant is scaled with
 * sift_col_numeric_key: NEAREST for an equality that the constant has at most s digits for, DOWN for the lower bound of a > or <= and UP for
 * a < or >= (so the key range holds exactly the values that satisfy the predicate); a constant with more digits than s never equals a value.
 * sift_col_key_numeric turns a key back into the numeric of scale s. NaN is stored as NULL with the overflow flag (the row stays a candidate).
 * The constant must be finite and its scaled value fit int64 (ERROR otherwise, like numeric_int8).
 */
typedef enum SiftColRound
{
	SIFT_COL_ROUND_NEAREST,
	SIFT_COL_ROUND_DOWN,
	SIFT_COL_ROUND_UP
} SiftColRound;
extern int64 sift_col_numeric_key(int scale, Datum value, SiftColRound round);
/* A finite constant whose scaled value is below 10^18, which every numeric(p <= 18, s) leaf stays under (so sift_col_numeric_key cannot overflow) */
extern bool sift_col_numeric_key_fits(int scale, Datum value);
extern Datum sift_col_key_numeric(int scale, int64 key);

/*
 * The other forms of numeric and the wide types. The constant of a predicate on a numeric of two lanes is scaled and
 * rounded like the narrow one and comes back as the two lane keys: 0 when it is inside the 128-bit range, -1 / 1 when it is below / above it
 * (the keys are then the least / greatest integer, which no value of a 38-digit column reaches), 2 for a NaN or an infinity. A numeric image is
 * the canonical bytes of the number (col/wide.h) and its display scale; an interval is the lane keys of its total and its month and day counts.
 */
extern int	sift_col_numeric_wide_key(int scale, Datum value, SiftColRound round, int64 *hi, int64 *lo);
extern Datum sift_col_w128_numeric(SiftCI128 v, int scale);
extern uint8 *sift_col_numeric_image(Datum value, uint32 *len, int32 *dscale);
extern Datum sift_col_image_numeric(const uint8 *img, uint32 len, int32 dscale);
extern void sift_col_interval_lanes(const Interval *iv, int64 *hi, int64 *lo, int64 *md);
extern Interval *sift_col_lanes_interval(int64 hi, int64 lo, int64 md);

/* The leaves of one index tuple (values/isnull as the AM callbacks receive them), allocated in mcxt */
extern void sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row,
							  MemoryContext mcxt);
/* The same with the values of the document read as a query's SQL reads them (sift_col_shred_row_rc) */
struct SiftShredRecheck;
extern void sift_col_make_row_rc(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt,
								 struct SiftShredRecheck *rc);

/* col/widescan.c: the hooks of the column source for wide integers (two INT lanes): the pair test of a predicate, and the zone pruning of one */
struct SiftColSegment;
extern bool sift_colw_is_wide_int(const SiftColDef *d);
extern bool sift_colw_is_wide_pred(const SiftColDef *d, const SiftColPred *p);
extern bool sift_colw_filter(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel);
extern void sift_colw_prune(struct SiftColSegment *seg, const SiftColPred *p, uint64 *keep);

/* col/amscan.c: the bitmap scan of the sift.col_*_ops operator classes (docs/COL-SCAN.md) */
extern bool sift_enable_col_scan;
extern double sift_plan_scale;
extern void sift_col_scan_init(void);
/* The bytes a row takes while the rows of a key-cut build, promotion or merge are held to be sorted (an estimate: a reference and a value of every leaf) */
extern uint64 sift_colk_row_bytes(const SiftColSchema *schema);
extern IndexScanDesc sift_col_ambeginscan(Relation index, int nkeys, int norderbys);
extern void sift_col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern int64 sift_col_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool sift_col_amgettuple(IndexScanDesc scan, ScanDirection dir);
/* After ExecutorStart: the index-only scans of sift_col indexes in the plan form their tuples from the columns the plan reads */
extern void sift_col_ioss_want(QueryDesc *queryDesc);
extern void sift_col_stmt_start(QueryDesc *queryDesc);
extern bool sift_col_amcanreturn(Relation index, int attno);
extern void sift_col_amendscan(IndexScanDesc scan);
extern void sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
									Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages);

/* col/am.c */
extern void sift_col_init(void);
extern IndexBuildResult *sift_col_ambuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void sift_col_ambuildempty(Relation index);
extern char *sift_col_ambuildphasename(int64 phase);
extern bool sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
							  IndexUniqueCheck checkUnique, bool indexUnchanged, struct IndexInfo *indexInfo);

/* col/maint.c */
extern IndexBulkDeleteResult *sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
													IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
/*
 * Merges the segments in[0..n) (written for schema; all of the page store rel, an index or a table) into one, dropping their dead rows.
 * format is the column format minor written (a sift_table writes SIFT_COL_FORMAT_VERS at least: its groups may carry versions). The rows of a table that are not final keep their versions, except those freeze (NULL: none) says are final now (sift_cseg_decode_unit_freeze).
 */
extern void sift_col_merge_segments(Relation rel, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg,
									const SiftSegEntry *in, int n, int format, SiftColFreezeFn freeze, void *freeze_arg);
/* Is more than half of the segment's rows dead (the merge policy's "mostly dead" test) */
extern bool sift_col_mostly_dead(Relation rel, const SiftSegEntry *e);
extern void sift_col_maint_after_seal(Relation index);
extern bool sift_col_maint_make_room(Relation index);
extern int	sift_col_maint_promote(Relation index, bool wait);
extern bool sift_col_maint_merge_step(Relation index, bool wait);
extern PGDLLEXPORT void sift_col_maint_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_col_rewrite_worker_main(Datum main_arg);
/* sift.col_auto_merge* (col/am.c): the rewrite VACUUM asks the worker for when the churn has scattered the index (col/maint.c) */
extern PGDLLIMPORT bool sift_col_auto_merge;
extern PGDLLIMPORT int sift_col_auto_merge_dirty_pct;
extern PGDLLIMPORT double sift_col_auto_merge_fragmentation;
extern PGDLLIMPORT double sift_col_auto_merge_buffer_pct;
extern PGDLLIMPORT double sift_col_auto_merge_wal_pct;
extern PGDLLIMPORT int sift_col_auto_merge_max_active;
extern PGDLLIMPORT int sift_col_auto_merge_partition_kb;
extern PGDLLIMPORT int sift_col_auto_merge_max_workers;
extern PGDLLIMPORT int sift_col_unit_floor;
/* The VACUUM that churn, and scans that check many rows in the heap, ask a worker for under sift.col_auto_merge (col/vacreq.c) */
extern void sift_col_vacuum_notice(Relation heap, Relation index);
extern void sift_col_build_settle(Relation index);
/* what the automatic rewrite waits for (sift.col_maintenance_status) */
#define SIFT_COL_GATE_HELD	1
#define SIFT_COL_GATE_WAL	2
/* not a gate: the rewrite is due for the peers a build left, which the WAL budget does not hold (sift_col_rewrite_tick starts it) */
#define SIFT_COL_PEERS		4
extern bool sift_col_autovacuum_enabled(Relation heap);
extern bool sift_col_table_churn(Oid heapoid, int64 *changed, int64 *threshold, TimestampTz *last);
extern void sift_col_vacuum_tick(Oid indexoid, Oid heapoid, int64 vetted);
extern void sift_col_rewrite_tick(Relation index);
extern PGDLLEXPORT void sift_col_vacuum_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_col_vacuum_scan_worker_main(Datum main_arg);

extern PGDLLIMPORT int sift_cdict_cache_kb;	/* kB of parsed segment dictionaries a backend keeps (col/dictcache.c); sift.test_col_dict_cache */

/* Testing builds: sift.test_col_scan_jitter_us sleeps up to that long at the points of a scan where another backend (VACUUM, DML, maintenance)
 * can change what the scan has read and not yet read: after open, after a unit is loaded, between the all-visible mask and the liveness,
 * before the heap checks. Widens the races deterministically in place of host load. */
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_col_jitter_us;
extern PGDLLIMPORT bool sift_test_col_verify_inval;	/* sift.test_col_verify_inval: sift.col_verify's heap check invalidates the index's relcache entry at every tuple (and clobbers the cached state it held) */
extern PGDLLIMPORT bool sift_test_col_rel_inval;	/* sift.test_col_rel_inval: the build and the recheck invalidate the index's relcache entry at every tuple (sift_col_test_inval) */
extern void sift_col_test_inval(Relation index);	/* does so when it is on, after it overwrote the cached state the entry held */
#ifdef SIFT_TESTING
typedef struct SiftCDictCacheStats
{
	uint64		loads;			/* dictionaries parsed from a segment */
	uint64		hits;			/* dictionaries found in the cache */
	uint64		head_loads;		/* segment heads (schema, unit directory) read from a segment */
	uint64		head_hits;		/* segment heads found in the cache */
	uint64		zone_loads;		/* zone arrays of a leaf read from a segment */
	uint64		zone_hits;		/* zone arrays found in the cache */
} SiftCDictCacheStats;
extern SiftCDictCacheStats sift_cdict_cache_stats;
extern Size sift_cdict_cache_bytes(void);
#endif
extern PGDLLIMPORT bool sift_test_col_paranoid;	/* sift.test_col_paranoid: rows on all-visible pages are checked against the heap too, and a disagreement is logged */
extern void sift_col_jitter(void);
#define SIFT_COL_JITTER()	sift_col_jitter()
#else
#define SIFT_COL_JITTER()	((void) 0)
#endif

/* the planner hook opens a planning, within which the statistics of an index are validated once (col/am.c leaf_cache_entry); true when this call opened it */
extern bool sift_col_plan_begin(void);
extern void sift_col_plan_end(bool started);
extern MemoryContext sift_col_plan_memo(uint64 *plan);
#ifdef SIFT_TESTING
extern int64 sift_col_key_sorts;	/* the sorts of an IN list by the cost estimates */
#endif

#endif							/* SIFT_COL_AM_H */
