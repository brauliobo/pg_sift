/*
 * col/am.h - the glue of the sift_col index access method: the per-relation cache, the conversion
 * of index tuples into column rows, and the entry points of the other glue files.
 */
#ifndef SIFT_COL_AM_H
#define SIFT_COL_AM_H

#include "access/amapi.h"
#include "access/transam.h"
#include "executor/execdesc.h"
#include "datatype/timestamp.h"

#include "col/pg.h"
#include "col/seg.h"
#include "col/enc.h"
#include "col/preagg.h"
#include "core/visibility.h"
#include "col/wbuf.h"
#include "col/conn.h"
#include "col/scope.h"

/* The xid before which every transaction that inserted an entry of the segment committed; false when the segment does not say */
static inline bool
sift_seg_xstamp(const SiftSegEntry *e, TransactionId *xid)
{
	if (!(e->flags & SIFT_SEG_XSTAMP))
		return false;
	*xid = e->reserved;
	return true;
}

static inline void
sift_seg_set_xstamp(SiftSegEntry *e, TransactionId xid)
{
	e->flags &= ~SIFT_SEG_XSTAMP;
	if (TransactionIdIsNormal(xid))
	{
		e->flags |= SIFT_SEG_XSTAMP;
		e->reserved = xid;
	}
}

/* The relcache entry's amcache: leaves, shredder, insert hint and a scratch context for aminsert */
#define SIFT_COL_CUT_MAX 4

typedef struct SiftColRel
{
	SiftColSchema *schema;
	SiftColIndexMeta meta;		/* cols point into schema->cols; stats zero */
	int			natts;
	int			conn_first;	/* the leaves from this id on are the found flags of the connections (SIFT_COLD_FOUND); none when it is ncols */
	int			nconn;			/* connections of the index (col/conn.h) and the leaves of each */
	int			conn_found[SIFT_CONN_MAX_CONNECTIONS];	/* the leaf of the found flag of each connection */
	int			scope_first;	/* the leaves from this id on are the derived keys of the scopes (SIFT_COLD_SCOPE); none when it is ncols */
	int			nscope;			/* scopes of the index (col/scope.h) and the leaf of each */
	int			scope_leaf[SIFT_SCOPE_MAX];	/* the leaf of the derived key of each scope */
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
	bool		rowkey;			/* the option track_changes names a column of the index: its attribute */
	bool		track;			/* the index tracks changes: by a key column (rowkey) or by the address of the rows (track_changes = ctid) */
	int			rowkey_attno;
	bool		journal_known;	/* journal_lineage (col/conn.c) is read: the relcache frees this state with the entry when it invalidates it */
	uint64		journal_lineage;
	MemoryContext rowcxt;		/* reset after every aminsert */
} SiftColRel;

/* The tombstone log of a heap index with track_changes, synced with what is appended (core/tomb.h); NULL when the index keeps none. sift_col_log_create adds the segment */
struct SiftTombLog;
extern struct SiftTombLog *sift_col_log(Relation index);
extern void sift_col_log_cache_stats(int64 *entries, int64 *bytes);
extern void sift_col_log_create(Relation index);
/* The log to append to (made when the index has none): no read of the directory, for the writer of many tombstones */
extern struct SiftTombLog *sift_col_log_appender(Relation index);
/* The tombstones of the replaced entries that a statement holds back (col/supersede.c): written before the log is read, before commit and before a savepoint starts, dropped when the transaction or the savepoint aborts */
extern void sift_col_batch_hook(void);
extern void sift_col_tomb_hold_tid(Relation index, ItemPointer tid);
extern void sift_col_tomb_hold_rec(Relation index, const SiftTomb *rec);
extern void sift_col_tombs_flush(void);
extern void sift_col_tombs_discard(void);
/* The option track_changes (col/rowkey.c): is a column named, the check of the option, and the preparation of a build */
extern bool sift_col_row_key_on(Relation index);
extern bool sift_col_track_ctid(Relation index);
extern bool sift_col_track_on(Relation index);
extern Oid	sift_col_repack_origin(Relation rel);
extern void sift_col_row_key_check(Relation index);
extern void sift_col_row_key_prepare(Relation heap, Relation index);
extern Oid sift_col_row_key_unique(Relation heap, AttrNumber att);
/* Tombstones for the addresses the unique index lists for key that no version is live for (col/rowkey.c) */
extern void sift_col_tomb_dead_keys(Relation index, Relation heap, struct Tuplestorestate *old_rows);
/* The option track_changes (col/track.c): why the table is one a statement trigger cannot hear, the triggers and the baseline, their removal, the reconcile after ALTER INDEX, and whether the log may be taken for complete */
extern const char *sift_col_track_deaf(Relation heap);
extern void sift_col_track_init(void);
extern void sift_col_track_dropping(Oid relid);
extern Oid sift_col_track_lookup(const char *name, bool missing_ok);
extern Oid sift_col_track_function(bool missing_ok);
extern Oid sift_col_track_trigger_index(Oid trigger);
extern void sift_col_track_orphan_fired(void);
extern PGDLLEXPORT void sift_col_track_orphans_main(Datum main_arg);
extern void sift_col_track_delete_trigger(Oid trigger);
extern const char *sift_col_row_key_why(Relation heap, Relation index, AttrNumber *pk);
extern bool sift_col_track_incomplete(Relation index);
extern void sift_col_track_check(Relation heap, Relation index);
extern void sift_col_track_install(Relation heap, Relation index, bool concurrent);
extern Oid sift_col_track_swap_origin(Relation heap, Relation index);
extern void sift_col_track_remove(Relation heap, Relation index);
extern void sift_col_track_deafened(Oid relid);
extern void sift_col_track_reconcile(Relation index);
extern bool sift_col_track_ok(Relation index, Relation heap, Snapshot snap);
extern bool sift_col_track_own(const struct SiftTombLog *log);
extern bool sift_col_track_rebaseline(Relation index, Relation heap);
extern void sift_col_track_ask(Relation index, Relation heap);
extern bool sift_col_track_rearmable(Relation index, Relation heap);
extern bool sift_col_track_rearm(Relation index, Relation heap);
extern PGDLLEXPORT void sift_col_track_worker_main(Datum main_arg);
/* An UPDATE wrote the version at newtid with these index values: the tombstones of the entries of the key that it replaces (col/supersede.c); speculative_stmt: the statement has an ON CONFLICT clause */
extern bool sift_col_updates_supersede(void);
extern bool sift_col_writing(void);
extern CommandId sift_col_stmt_cid(void);
extern void sift_col_supersede(Relation index, Relation heap, const Datum *values, ItemPointer newtid, bool speculative_stmt);
#ifdef SIFT_TESTING
extern PGDLLIMPORT bool sift_test_no_supersede;
extern PGDLLIMPORT bool sift_test_tombs_at_once;
extern PGDLLIMPORT bool sift_test_supersede_scan;
extern PGDLLIMPORT bool sift_test_no_track;
extern PGDLLIMPORT bool sift_test_no_chain_root;
extern PGDLLIMPORT bool sift_test_no_log_reap;
extern PGDLLIMPORT bool sift_test_no_log_hide;
extern PGDLLIMPORT uint64 sift_test_col_log_hidden_count;
extern PGDLLIMPORT bool sift_test_no_heapfree;
extern PGDLLIMPORT bool sift_test_hf_ignore_tombs;
extern PGDLLIMPORT bool sift_test_hf_ignore_stamp;
#endif
/* Pages, ranges and masks the tombstone log made all-visible where the visibility map did not (tests) */
extern PGDLLIMPORT int64 sift_col_hf_taken;
extern PGDLLIMPORT int64 sift_col_hf_asked;
extern PGDLLIMPORT int64 sift_col_hf_block_hits;
/* The addresses of dead rows the probes of compact order blocks listed (tests) */
extern PGDLLIMPORT int64 sift_col_probe_dead;

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
/* What the build says about an index, in one NOTICE (col/build/choices.c): the kinds of rows it derives, as bits */
#define SIFT_CHOICE_AUTO	1
#define SIFT_CHOICE_ADVICE	2
#define SIFT_CHOICE_CAP	4
#define SIFT_CHOICE_ACTION	8
#define SIFT_CHOICE_ALL		15
typedef void (*SiftChoiceFn) (void *ctx, const char *id, const char *value);
extern void sift_col_choices_say(Relation heap, Relation index, int kinds, bool build);
extern const char *sift_col_final_name(Relation index);
extern void sift_col_caps(Relation index, SiftChoiceFn add, void *ctx);
extern char *sift_col_key_cut_note(Relation index);
extern const char *sift_col_key_cut_advised(Relation index);
extern char *sift_col_key_cut_advised_now(Relation heap, Relation index);
extern const char *sift_col_key_cut_hot(Relation heap, Relation index, bool build);
/* The default of key_columns (col/keycols/keycols.c): before a build, the columns chosen and written to the option (true: the build keeps the choice and says so); after it, those that hold a key list or bitmap in half of the units, and a NOTICE */
#define SIFT_COL_AUTO_KEEP_PCT	50		/* a column that holds no key list or bitmap in this share of the units, or more, is not kept; the cube answers a column it holds in as many */
extern bool sift_col_auto_keys(Relation heap, Relation index);
typedef enum SiftAutoCut
{
	SIFT_AUTOCUT_NONE,
	SIFT_AUTOCUT_CUT,			/* key_cut = 'auto: col' was written */
	SIFT_AUTOCUT_ADVISED		/* a key is worth cutting by, but the index would grow: the NOTICE says so */
} SiftAutoCut;
extern bool sift_col_options_key_cut_rows_set(Relation index);
extern bool sift_col_options_order_columns_option(Relation index);
extern bool sift_col_options_order_columns_set(Relation index);
extern bool sift_col_options_key_order_auto(Relation index);
extern SiftAutoCut sift_col_auto_cut(Relation heap, Relation index);
/* The NOTICE that a key is worth a key_cut the build did not write, from the sample of the build (autocut.c) or from the units it wrote (cluster.c); detail: why, and what it costs */
extern bool sift_col_auto_cut_again(Relation heap, Relation index, bool fits);
extern bool sift_col_options_key_cut_auto(Relation index);
extern const char *sift_col_options_key_order_group(Relation index);
/* a column name that looks rewritten often (a timestamp of the last change, a counter, a version): an UPDATE of it is not a HOT update while the index covers it (col/build/build.c) */
extern bool sift_col_volatile_name(const char *name);
#define SIFT_COL_AUTO_ADD_PCT	40		/* a merge of every segment adds a key list to a column the cube holds in fewer of the units than this; the report drops one it holds in SIFT_COL_AUTO_KEEP_PCT or more, so a column that sits between is not added and dropped in turn */
extern bool sift_col_auto_keys_merge(Relation index, const SiftSegEntry *in, int n);
extern bool sift_col_redecide_busy(Relation index);
extern bool sift_col_auto_redecide(Relation index, const SiftSegEntry *in, int n);
extern void sift_col_pagg_rewrite(Relation index, const SiftSegEntry *in, int n, SiftCPaggSpec *spec);
/* paths = 'auto' (col/keycols/keycols.c, col/suggest.c): before a build, the paths read from a sample of the jsonb documents and written to the option as 'auto: spec' */
#define SIFT_COL_AUTO_PATHS_SAMPLE	2000
extern void sift_col_auto_paths(Relation heap, Relation index);
/* preagg = 'auto' or none (col/keycols/keycols.c): before a build of an index with more plain columns than the cube holds, the columns ranked by data and written to the option as 'auto: a, b' */
extern void sift_col_auto_cube(Relation heap, Relation index);
/* rollup_columns = 'auto' (col/keycols/keycols.c): the columns whose combinations fit the rollup of the rows of a unit, written to the option as 'auto: a, b' */
extern void sift_col_auto_rows(Relation heap, Relation index);
/* array_lengths = auto (col/keycols/keycols.c): written as on for an index with an array column, before the build */
extern void sift_col_auto_arrlen(Relation index);
/* Before a build with order_array_columns unset or 'auto': the columns that get the global order array, written to the option */
extern void sift_col_auto_gorder(Relation heap, Relation index, bool layout);
extern const char *sift_col_group_reason(Relation heap, Relation index, const char *name);
extern void sift_col_auto_point(Relation heap, Relation index);
extern bool sift_col_list_has(const char *list, const char *name);	/* an option list of column names ('a, b' or 'auto: a, b') names the column */
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
/* The first column of an index on a table shaped like a scope copy (a tenant or group key before the entity it was copied by): the key the build cuts and orders by, or NULL (col/keycols/keycols.c) */
extern const char *sift_col_scope_key(Relation heap, Relation index, bool build);
#ifdef SIFT_TESTING
extern int	sift_test_col_rollup_panic;
extern int	sift_test_col_rollup_panic_armed;
extern bool sift_test_col_merge_panic;
extern int	sift_test_col_refresh_sleep_ms;		/* sift.test_col_refresh_sleep_ms: the refresh of a segment sleeps this long once the new segment is written */
extern int	sift_test_col_refresh_sleep_armed;
extern int	sift_test_conn_fill;			/* sift.test_conn_fill: 1 = the fill of the found flags reads the parent, 2 = it probes the btree (0 = by price) */
extern int	sift_test_col_floor_sbs;		/* sift.test_col_floor_sbs: the superblocks a promotion or merge must reach for sift.col_unit_floor to apply (0 = 16) */
#endif

/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a preagg option ("default", "off", "cube(dim: measure, ...; ...)"), as far as it can tell without the index */
extern void sift_col_validate_preagg(const char *value);
extern void sift_col_validate_rollup(const char *value);
extern void sift_col_validate_rollup_dims(const char *value);
extern void sift_col_validate_rollup_leaves(const char *value);
extern void sift_col_validate_rollup_columns(const char *value);
extern void sift_col_validate_calendar_columns(const char *value);
extern void sift_col_validate_key_columns(const char *value);
extern void sift_col_validate_order_columns(const char *value);
extern void sift_col_validate_order_array_columns(const char *value);
extern void sift_col_validate_order_layout(const char *value);
extern void sift_col_validate_order_array_group(const char *value);
extern void sift_col_validate_hll_columns(const char *value);
extern void sift_col_validate_point_columns(const char *value);
extern void sift_col_validate_point_tid_columns(const char *value);
extern void sift_col_validate_track_changes(const char *value);
extern void sift_col_validate_connect(const char *value);
extern void sift_col_validate_scope(const char *value);
extern void sift_col_validate_key_order(const char *value);
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
extern int	sift_colk_write(SiftColSegWriter *w, SiftColBatch *const *in, int nin, const SiftColSchema *schema, const int *leaf, int nleaf, uint32 unit_rows, MemoryContext cx, uint32 unit_base);
extern int	sift_colk_merge(SiftColSegWriter *w, const SiftSegEntry *ent, SiftColSegment *const *seg, int n, SiftColFreezeFn freeze, void *freeze_arg, const SiftColSchema *schema,
							const int *leaf, int nleaf, uint32 unit_rows, uint64 memory_kb, MemoryContext cx, SiftColCutFn cutfn, void *cutarg,
							int slice, int nslices, uint32 unit_base, bool resort);

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

/* col/amscan/amscan.c: the bitmap scan of the sift.col_*_ops operator classes (docs/COL-SCAN.md) */
extern bool sift_enable_col_scan;
extern double sift_plan_scale;
extern void sift_col_scan_init(void);
/* The bytes a row takes while the rows of a key-cut build, promotion or merge are held to be sorted (an estimate: a reference and a value of every leaf) */
extern uint64 sift_colk_row_bytes(const SiftColSchema *schema);
extern IndexScanDesc sift_col_ambeginscan(Relation index, int nkeys, int norderbys);
extern void sift_col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
extern int64 sift_col_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool sift_col_amgettuple(IndexScanDesc scan, ScanDirection dir);
extern void sift_col_ammarkpos(IndexScanDesc scan);
extern void sift_col_amrestrpos(IndexScanDesc scan);
/* After ExecutorStart: the index-only scans of sift_col indexes in the plan form their tuples from the columns the plan reads */
extern void sift_col_ioss_want(QueryDesc *queryDesc);
extern void sift_col_stmt_start(QueryDesc *queryDesc);
/* The rows and tombstones the transaction holds back (they reach the pages when it reads the index): before a worker, which has none of its own, reads them */
extern void sift_col_flush_held(void);
extern bool sift_col_amcanreturn(Relation index, int attno);
extern void sift_col_amendscan(IndexScanDesc scan);
extern void sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
									Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages);

/* col/am/am.c */
extern void sift_col_init(void);
extern IndexBuildResult *sift_col_ambuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void sift_col_ambuildempty(Relation index);
extern char *sift_col_ambuildphasename(int64 phase);
extern bool sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
							  IndexUniqueCheck checkUnique, bool indexUnchanged, struct IndexInfo *indexInfo);

/* col/maint/maint.c */
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
extern PGDLLEXPORT void sift_col_read_rewrite_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_col_read_promote_worker_main(Datum main_arg);
/* sift.col_auto_merge* (col/am/am.c): the rewrite VACUUM asks the worker for when the churn has scattered the index (col/maint/maint.c) */
extern PGDLLIMPORT bool sift_col_auto_merge;
extern PGDLLIMPORT int sift_col_auto_merge_dirty_pct;
extern PGDLLIMPORT double sift_col_auto_merge_fragmentation;
extern PGDLLIMPORT double sift_col_auto_merge_buffer_pct;
extern PGDLLIMPORT double sift_col_auto_merge_wal_pct;
extern PGDLLIMPORT int sift_col_auto_merge_pass_mb;
extern PGDLLIMPORT int sift_col_auto_merge_max_active;
extern PGDLLIMPORT int sift_col_auto_merge_partition_kb;
extern PGDLLIMPORT int sift_col_auto_merge_max_workers;
extern PGDLLIMPORT int sift_col_unit_floor;
extern PGDLLIMPORT bool sift_col_legacy_options;	/* sift.test_col_legacy_options: the options 4.3 deprecated are honored (removed in 4.4) */
/* The VACUUM that churn, and scans that check many rows in the heap, ask a worker for under sift.col_auto_merge (col/vacreq.c) */
extern void sift_col_vacuum_notice(Relation heap, Relation index);
extern BlockNumber sift_col_build_gap(Relation heap, Relation index);
extern void sift_col_build_merge(Relation index, BlockNumber gap, uint32 gapn, int workers);
extern void sift_col_merge_slice(Relation index, const SiftSegEntry *in, int n, int i, int nslices, SiftSegEntry *out);
extern void sift_col_merge_switch(Relation index, const SiftSegEntry *in, int n, const SiftSegEntry *out, int nout, TimestampTz start);
/* what the automatic rewrite waits for (sift.col_maintenance_status) */
#define SIFT_COL_GATE_HELD	1
#define SIFT_COL_GATE_WAL	2
/* not a gate: the rewrite is due for the peers a build left, which the WAL budget does not hold (sift_col_rewrite_tick starts it) */
#define SIFT_COL_PEERS		4
extern bool sift_col_autovacuum_enabled(Relation heap);
extern bool sift_col_table_churn(Oid heapoid, int64 *changed, int64 *threshold, TimestampTz *last);
extern void sift_col_vacuum_tick(Oid indexoid, Oid heapoid, int64 vetted, int64 dirty_rows);
extern void sift_col_vacuum_note_insert(Relation index, Relation heap);
extern bool sift_col_rewrite_tick(Relation index, double owed);
extern bool sift_col_promote_tick(Relation index);
extern PGDLLEXPORT void sift_col_vacuum_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_col_vacuum_scan_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_col_vacuum_once_worker_main(Datum main_arg);

extern PGDLLIMPORT int sift_cdict_cache_kb;	/* kB of parsed segment dictionaries a backend keeps (col/dictcache.c); sift.test_col_dict_cache */

/* Testing builds: sift.test_col_scan_jitter_us sleeps up to that long at the points of a scan where another backend (VACUUM, DML, maintenance)
 * can change what the scan has read and not yet read: after open, after a unit is loaded, between the all-visible mask and the liveness,
 * before the heap checks. Widens the races deterministically in place of host load. */
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_col_jitter_us;
extern PGDLLIMPORT bool sift_test_col_verify_inval;	/* sift.test_col_verify_inval: sift.col_verify's heap check invalidates the index's relcache entry at every tuple (and clobbers the cached state it held) */
extern PGDLLIMPORT int sift_test_col_log_max_read;	/* sift.test_col_log_max_read: the newest column format whose tombstone log is opened (0 = the build's own); a newer log is refused */
extern PGDLLIMPORT bool sift_test_col_rel_inval;	/* sift.test_col_rel_inval: the build and the recheck invalidate the index's relcache entry at every tuple (sift_col_test_inval) */
extern PGDLLIMPORT bool sift_test_col_net;	/* sift.test_col_net: VACUUM writes the pre-aggregates net of dead rows (col/maint/net.c, table/net.c); off, the dirty units are left to the delta of sift.agg_dead_delta */
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
extern PGDLLIMPORT int sift_test_col_open_gate;	/* sift.test_col_open_gate: a scan waits for the advisory lock of this number right after it read the directory */
extern PGDLLIMPORT uint64 sift_col_gkept_stat[4];	/* the kept orders of the write buffers (col/scan/gorder.c): reads that found the order as it was, extended it, made it, dropped it */
extern PGDLLIMPORT uint64 sift_col_gdead_stat[3];	/* the kept lists of dead rows (col/seg/dead.c): lists found as they were, lists made; the entries the cut of an OFFSET took off the sources (agg/gord.c) */
extern PGDLLIMPORT bool sift_test_col_gkept_fail;	/* sift.test_col_gkept_fail: extending a kept order fails after the new arrays are made, before the order takes them */
/* Holds a scan, with the directory version it read, until the test releases the lock: what runs meanwhile (VACUUM, DML) is after the open for the scan */
#define SIFT_COL_OPEN_GATE()	sift_test_hold(sift_test_col_open_gate)
#else
#define SIFT_COL_JITTER()	((void) 0)
#define SIFT_COL_OPEN_GATE()	((void) 0)
#endif

/* the planner hook opens a planning, within which the statistics of an index are validated once (col/am/am.c leaf_cache_entry); true when this call opened it */
extern bool sift_col_plan_begin(void);
extern void sift_col_plan_end(bool started);
extern MemoryContext sift_col_plan_memo(uint64 *plan);
#ifdef SIFT_TESTING
extern int64 sift_col_key_sorts;	/* the sorts of an IN list by the cost estimates */
extern int64 sift_col_vet_offsets;	/* the row offsets decoded to check rows against the heap (sift_col_vet) */
#endif

#endif							/* SIFT_COL_AM_H */
