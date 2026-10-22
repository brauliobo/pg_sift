/*
 * col/am.h - the glue of the sift_col index access method (docs/COL.md section 12, L5): the per-relation cache, the conversion
 * of index tuples into column rows, and the entry points of the other glue files.
 */
#ifndef SIFT_COL_AM_H
#define SIFT_COL_AM_H

#include "access/amapi.h"
#include "datatype/timestamp.h"

#include "col/pg.h"
#include "col/seg.h"
#include "col/enc.h"
#include "col/preagg.h"
#include "col/wbuf.h"

/* The relcache entry's amcache: leaves, shredder, insert hint and a scratch context for aminsert */
typedef struct SiftColRel
{
	SiftColSchema *schema;
	SiftColIndexMeta meta;		/* cols point into schema->cols; stats zero */
	int			natts;
	int		   *first;			/* [natts] id of the first leaf of attribute i */
	int		   *nleaves;		/* [natts] */
	bool	   *json;			/* [natts + 1] the attribute is a shredded jsonb */
	bool	   *name;			/* [natts + 1] the attribute is a name (a fixed buffer, not a varlena) */
	SiftColShredAdapter *shred;
	SiftCcPolicy *policy;		/* [ncols] compression policy of each leaf (the compression option) */
	SiftCPaggSpec pagg;			/* the pre-aggregates of every unit the index writes (the preagg option, docs/COL-PREAGG.md) */
	SiftCWbufHint hint;
	MemoryContext rowcxt;		/* reset after every aminsert */
} SiftColRel;

/*
 * The on-demand refresh of the rollups (col/rollup.c): a scan asks for the segment gen of index to be rewritten with the rollups the index writes now.
 * Writes no page; false when the request was not taken (switched off, a standby). The GUC sift.col_rollup_on_demand.
 */
extern bool sift_col_rollup_on_demand;
extern bool sift_col_rollup_worker;
extern bool sift_col_rollup_request(Relation index, uint64 gen);
/* A NOTICE for each cap of the options (cube(...) groups per unit, set_limit) that leaves 10% of the units or more without a histogram or the keys of a set; after a build and a refresh */
extern void sift_col_report_caps(Relation index);
#ifdef SIFT_TESTING
extern int	sift_test_col_rollup_panic;
extern int	sift_test_col_rollup_panic_armed;
extern bool sift_test_col_merge_panic;
extern int	sift_test_col_refresh_sleep_ms;		/* sift.test_col_refresh_sleep_ms: the refresh of a segment sleeps this long once the new segment is written */
extern int	sift_test_col_refresh_sleep_armed;
#endif

/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a preagg option ("default", "off", "cube(dim: measure, ...; ...)"), as far as it can tell without the index */
extern void sift_col_validate_preagg(const char *value);
extern void sift_col_validate_rollup(const char *value);
extern void sift_col_validate_rollup_dims(const char *value);
extern void sift_col_validate_calendar_columns(const char *value);
extern void sift_col_validate_set_columns(const char *value);
/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a compression option ("policy" or "policy; column=policy; ...") */
extern void sift_col_validate_compression(const char *value);
/* The storage class of a type the table AM holds as one leaf (false: not natively); the scale of a numeric(p, s) leaf that is one INT leaf, p <= 18 and 0 <= s <= 18 (false: it is not) */
extern bool sift_col_leaf_store(Oid typid, SiftColStore *store);
extern bool sift_col_numeric_scale(Form_pg_attribute att, uint8 *scale);
/*
 * The leaves of one attribute of a sift_col index (col/types.c): defs[0 .. result) with the type, storage class, scale, lanes, lane and path set;
 * the caller sets id and attno (parent is -1). defs has room for SIFT_COL_MAX_LANES. ERROR for a type the engine does not hold.
 */
extern int	sift_col_att_leaves(Form_pg_attribute att, SiftColDef *defs);
/* The values of one datum for the leaf def and the leaves after it (its lanes): v[0 .. max(def->lanes, 1)); strings are palloc'd or point into the datum */
extern void sift_col_datum_values(const SiftColDef *def, Datum d, bool name, SiftColValue *v);

/* Built on first use from the index definition (ERROR for an unsupported type or a jsonb attribute without paths) */
extern SiftColRel *sift_col_rel(Relation index);
/*
 * The same in mcxt, for a caller that holds it across a lock or an expression of the index: a reload of the relcache entry (any lock taken
 * meanwhile may cause one) frees the one sift_col_rel returns, which stays only for what the caller reads before it does either.
 */
extern SiftColRel *sift_col_rel_private(Relation index, MemoryContext mcxt);

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
 * The other forms of numeric and the wide types (docs/COL-LEAFTYPES-32.md). The constant of a predicate on a numeric of two lanes is scaled and
 * rounded like the narrow one and comes back as the two lane keys: 0 when it is inside the 128-bit range, -1 / 1 when it is below / above it
 * (the keys are then the least / greatest integer, which no value of a 38-digit column reaches), 2 for a NaN or an infinity. A numeric image is
 * the canonical bytes of the number (col/wide.h) and its display scale; an interval is the lane keys of its total and its month and day counts.
 */
extern int	sift_col_numeric_wide_key(int scale, Datum value, SiftColRound round, int64 *hi, int64 *lo);
extern Datum sift_col_wide_numeric(int scale, int64 hi, int64 lo);
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
extern bool sift_colw_filter(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel);
extern void sift_colw_prune(struct SiftColSegment *seg, const SiftColPred *p, uint64 *keep);

/* col/amscan.c: the bitmap scan of the sift.col_*_ops operator classes (docs/COL-SCAN.md) */
extern bool sift_enable_col_scan;
extern void sift_col_scan_init(void);
extern IndexScanDesc sift_col_ambeginscan(Relation index, int nkeys, int norderbys);
extern void sift_col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern int64 sift_col_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool sift_col_amgettuple(IndexScanDesc scan, ScanDirection dir);
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
extern PGDLLIMPORT double sift_col_auto_merge_min_churn_pct;
extern PGDLLIMPORT int sift_col_auto_merge_max_active;
extern PGDLLIMPORT int sift_col_auto_merge_partition_kb;
extern PGDLLIMPORT int sift_col_auto_merge_max_workers;

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

#endif							/* SIFT_COL_AM_H */
