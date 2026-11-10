/*
 * col/pg.h - the PostgreSQL-bound declarations of the column engine: the relcache and catalog accessors of a sift_col index, the
 * planner's index expressions, the shredder adapter, the reloptions and the SQL entry points. col/col.h keeps the host-independent
 * types and the key helpers, so the portable files (col/enc.[ch], col/codec.[ch], docs/COL-PORTS.md) never see this header.
 */
#ifndef SIFT_COL_PG_H
#define SIFT_COL_PG_H

#include "access/tupdesc.h"
#include "fmgr.h"
#include "nodes/nodes.h"

#include "col/col.h"

/*
 * The prices of the scans of sift are written in microseconds of a warm run, and the planner's unit is not one length of time: a unit is 1.75 us in the joins over the heap
 * (a hash join of 100k to 1M rows costs 493 to 553 units a millisecond) and 3.2 us in its scans that aggregate (median of 106 plans of test/plan, 250,000 rows, TIMING OFF,
 * p25 2.2, p75 4.2). Each model of sift has its own factor, fitted from records (planner-pl15-arms-*, planner-pl16-units-*):
 * the scan of a sift_col index is divided by the microseconds per unit of the joins it is nested in (the nested loop of probes crossed the hash join at 0.44 to 0.63 of the
 * probes it was measured at before, 0.65 to 0.95 after); SiftAgg keeps the share of its price against the native plan of the same statement. That share fitted over 102 plans is
 * 0.66 (p25 0.49, p75 1.23), but three plans of a sift_table, priced too low already, are slower than the native plan they lose to (top_rows_per_kind_unindexed_column 27.0 against
 * 22.5 ms, spine_range_join 30.6 against 22.6 ms, spine_expression_key 107 against 17 ms) and change to the scan below 0.9, 0.8 and 0.7: the share stays at 0.93 until the price of the
 * scan of a sift_table is raised, and the fitted 0.66 follows. At 0.98 and above the nested loop of agg_topn_plan_keys (885) is priced below SiftAgg's top-N (907) again.
 * The prices of BM25 are made of PostgreSQL's own costs (pages, cpu_operator_cost): factor 1.
 */
#define SIFT_COST_COL_US_PER_UNIT	1.75
#define SIFT_COST_AGG_SHARE			0.93

/* Per leaf (ncols entries, memoized per backend, valid until the index changes or the memo of 8192 indexes is dropped): what the zones and dictionaries of the immutable segments say, no chunk read */
extern const SiftColLeafStats *sift_col_leaf_stats(Relation index);
/* The compact chunks of the order blocks of leaf over the segments, read from their directories (the statistics only say which blocks have one) */
extern double sift_col_gorder_compact_chunks(Relation index, int leaf);
/* The sample of the unit zones of the integer leaves (same memo and validity as sift_col_leaf_stats) */
extern const SiftColZones *sift_col_zones(Relation index);

/*
 * How many units of the immutable segments hold the sections the options ask now (same memo as the leaf statistics, which a change of the options
 * also renews): dim[leaf] the histogram of the leaf, mm[leaf] with min and max, roll[list] the rollup of the list, rows the rollup of the rows. A unit over a cap or a budget has none.
 */
typedef struct SiftColPreStats
{
	double		units;
	double		dim[SIFT_COL_MAX_COLS];
	double		mm[SIFT_COL_MAX_COLS];
	double		roll[SIFT_COL_MAX_COLS];
	double		rows;			/* the rollup of the rows */
} SiftColPreStats;
extern const SiftColPreStats *sift_col_pre_stats(Relation index);
#ifdef SIFT_TESTING
/* folds the statistics with the shared ring (*incremental: the segments opened) and without it (*full), and tells whether they are the same bytes */
extern bool sift_col_leaf_fold_check(Relation index, int *incremental, int *full);
#endif
/* What the planner takes of an index with its meta (sift_col_index_meta): the memos of the two functions above for the directory it read */
typedef struct SiftColLeafMemo
{
	const SiftColLeafStats *leaves;
	const SiftColZones *zones;
} SiftColLeafMemo;
/*
 * The leaf list and statistics of a sift_col index (allocates in CurrentMemoryContext; the stats read the directory and the visibility map).
 * With memo, the leaf statistics, zones and pre-aggregate counts of the same directory read come with it.
 */
extern SiftColIndexMeta *sift_col_index_meta(Relation index, SiftColLeafMemo *memo);
/* A hash of what decides the sections a unit is written with: the options preagg, rollup and cube_minmax, and sift.col_format_minor */
extern uint32 sift_col_pagg_hash(Relation index);

/* Just the leaf list (cached in the relcache entry: no I/O), same struct with zero stats */
extern const SiftColIndexMeta *sift_col_index_defs(Relation index);
extern const char *sift_col_leaf_name(Relation index, const SiftColDef *def);

/*
 * The expression of index attribute attno (1-based): a Var with varno 1 for a plain column, else the index expression as stored
 * (varno 1, ready for ChangeVarNodes). Allocates in CurrentMemoryContext.
 */
extern Node *sift_col_att_expr(Relation index, int attno);

/* The format's type tag of a PostgreSQL type (OTHER for one the tags do not name), and the type a leaf yields to SQL (desc: the relation the leaf's attno refers to) */
extern uint8 sift_col_type_of_oid(Oid typid);
extern Oid	sift_col_def_typid(TupleDesc desc, const SiftColDef *def);

/* Is relam the sift_col access method (the catalog lookup is cached) */
extern bool sift_col_is_index(Relation index);
extern bool sift_col_is_index_oid(Oid indexoid);
extern Oid	sift_col_am_oid(void);
/*
 * The index of an SQL function that is given its oid, with its table locked first in the same mode (the lock order of the statements: DROP TABLE and
 * ALTER TABLE lock the table and then its indexes, so a function that held the index and asked for the table would deadlock with them); an error where it
 * is no sift_col index
 */
extern Relation sift_col_open_index(Oid indexoid, LOCKMODE lock);

extern PGDLLIMPORT int sift_col_format_minor;	/* the minor of the segments written: 4, or 3 to stay readable by 3.0 */

/* ---------------------------------------------------------------------------
 * 4. Shred adapter (col/jsonb.c): the index's jsonb attributes, shredded by col-json's col/shred.h
 * ------------------------------------------------------------------------- */

typedef struct SiftColShredAdapter SiftColShredAdapter;

/* Parses the "paths" reloption against the index; NULL when it has none. ERROR for a specification the engine cannot hold (deeper nesting than one list level, ...) */
/* The presence leaf of a jsonb document ("<column>#present", true, NULL where the document is SQL NULL): which columns have one is the layout of the index (col/am/rel.c) */
typedef enum SiftColPresent
{
	SIFT_COL_PRESENT_NULLABLE,	/* the jsonb columns whose column of the table may be NULL: what a build writes */
	SIFT_COL_PRESENT_NONE,		/* an index built before the leaf */
	SIFT_COL_PRESENT_ALL		/* every jsonb column of the index */
} SiftColPresent;

extern uint32 sift_col_present_attrs(Relation index, SiftColPresent which);
extern bool sift_col_shred_drop_present(SiftColShredAdapter *a, int attno);
extern SiftColShredAdapter *sift_col_shred_create(MemoryContext mcxt, Relation index, uint32 present);
/* The same for a paths specification over the first natts attributes of a tuple descriptor (a sift_table's declared paths) */
extern SiftColShredAdapter *sift_col_shred_create_for(MemoryContext mcxt, TupleDesc desc, int natts, const char *paths);
/* Leaves the specification declares for index attribute attno, in the order sift_col_shred_row fills them (0: not shredded); parent is relative to the first */
extern int	sift_col_shred_nleaves(const SiftColShredAdapter *a, int attno);
extern void sift_col_shred_leaf(const SiftColShredAdapter *a, int attno, int i, SiftColDef *def);
extern void sift_col_shred_set_base(SiftColShredAdapter *a, int attno, int base);
extern bool sift_col_shred_keeps_image(const SiftColShredAdapter *a, int attno);
/* All the shredded attributes of one tuple (values/isnull as the index build gives them, indexed by attribute) into row->val / row->elem, whose leaf ids start at first[attno] */
extern void sift_col_shred_row(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftColRow *row, const int *first, MemoryContext mcxt);
/* The same read as the SQL of a query reads the values; rc is made by sift_col_shred_recheck_create and told how the query reads each leaf by _set (mode: SiftRecheckCol for a column, SiftRecheckGroup for a list) */
struct SiftShredRecheck;
extern void sift_col_shred_row_rc(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftColRow *row, const int *first, MemoryContext mcxt,
								  struct SiftShredRecheck *rc);
extern struct SiftShredRecheck *sift_col_shred_recheck_create(MemoryContext mcxt, const SiftColShredAdapter *a);
extern void sift_col_shred_recheck_set(struct SiftShredRecheck *rc, const SiftColShredAdapter *a, int attno, int i, uint8 mode);
/* the value column i of a map is read as the cast of the member of one key to the type cast (an Oid) */
extern void sift_col_shred_recheck_member(struct SiftShredRecheck *rc, const SiftColShredAdapter *a, int attno, int i, const char *key, Oid cast);
extern void sift_col_shred_recheck_haskey(struct SiftShredRecheck *rc, const SiftColShredAdapter *a, int attno, int i, const char *key);

/* ---------------------------------------------------------------------------
 * 5. Options and SQL entry points (PG_FUNCTION_INFO_V1 are in core/handler.c)
 * ------------------------------------------------------------------------- */

/* The values of the option dict_lookup: auto stores the dictionaries of the leaves a lookup structure reads (point_columns, key_columns, return_columns) */
enum
{
	SIFT_COL_DICT_AUTO,
	SIFT_COL_DICT_ON,
	SIFT_COL_DICT_OFF
};

/* The values of the option array_lengths: the build writes on for an index with an array column (auto is an index built before it, which keeps the images alone) */
enum
{
	SIFT_COL_ARRLEN_AUTO,
	SIFT_COL_ARRLEN_ON,
	SIFT_COL_ARRLEN_OFF
};

typedef struct SiftColOptions
{
	int32		vl_len_;
	int			write_buffer_limit;	/* kB */
	int			paths;			/* offset of the shredder's "paths" string, 0 = none */
	int			compression;	/* offset of the "compression" string, 0 = auto */
	int			preagg;			/* offset of the "preagg" string (docs/COL-PREAGG.md), 0 = default */
	int			rollup;			/* the older name of rollup_leaves: am_col_amoptions copies its offset to rollup_leaves when that is empty */
	int			rollup_leaves;	/* offset of the "rollup_leaves" string (the dims of the element rollups, docs/COL-PREAGG.md), 0 = automatic */
	int			calendar_columns;	/* offset of the "calendar_columns" string (the date and timestamp columns that get calendar cells in every unit, docs/COL-PREAGG.md section 2; column format minor 8), 0 = none */
	int			rollup_dims;	/* the older name of rollup_columns, copied the same way */
	int			rollup_columns;	/* offset of the "rollup_columns" string (the columns of the rollup of the rows of a unit, docs/COL-PREAGG.md; column format minor 8), 0 = none */
	bool		rollup_on_demand;	/* a SELECT that scans a unit for lack of its rollup asks for a refresh (docs/COL-PREAGG.md section 7) */
	int			return_columns;	/* offset of the "return_columns" string (the text, varchar and uuid columns an index-only scan returns), 0 = none */
	int			key_columns;	/* offset of the "key_columns" string, 0 = not set */
	int			key_cut;		/* offset of the "key_cut" string, 0 = none */
	int			key_cut_rows;	/* rows a key-cut unit aims at, 0 = as many as an average superblock holds */
	int			order_columns;	/* offset of the "order_columns" string, 0 = none */
	int			hll_columns;	/* offset of the "hll_columns" string, 0 = none */
	int			point_columns;	/* offset of the "point_columns" string, 0 = none */
	int			count_columns;	/* offset of the "count_columns" string, 0 = none */
	int			point_tid_columns;	/* offset of the "point_tid_columns" string, 0 = none */
	int			track_changes;		/* offset of the "track_changes" string (the column that identifies a row: the index replaces the older entries of a row when it is updated), 0 = off */
	int			connect;		/* offset of the "connect" string (the parent index this one is connected to, col/conn.h), 0 = none */
	int			array_lengths;	/* SIFT_COL_ARRLEN_*: the arrays of the index keep their cardinality and array_length(x, 1) as two integer lanes */
	int			dict_lookup;	/* SIFT_COL_DICT_*: which dictionaries of many strings are stored with the order of their strings, to be searched in place (column format minor 14) */
	int			order_layout;	/* offset of the "order_layout" string (the layout of the global orders, col/gorder.h), 0 = auto */
	int			order_array_group;	/* offset of the "order_array_group" string (the column the global orders are grouped by), 0 = none */
	int			order_array_columns;	/* offset of the "order_array_columns" string, 0 = none */
	int			key_order;		/* offset of the "key_order" string, 0 = none */
	int			ko_lead;		/* what key_order stands for (am_col_amoptions), offsets of its columns: the first ... */
	int			ko_last;		/* ... the last (order_columns, order_array_columns) ... */
	int			ko_group;		/* ... the first of 'group, key' (order_array_group; a cut by the group and its point directory made count(*) of one group 5 to 18 times dearer and a global ORDER BY .. LIMIT 4 to 12 times) ... */
	bool		ko_auto;		/* ... all of it chosen by the build with key_cut ('auto: a[, b]'): the directory and key_cut are the build's, not key_order's */
	int			ko_key;			/* ... the column of a key_order of one column (key_cut and point_tid_columns); all 0 while key_order is not set */
	bool		cube_minmax;	/* the cube of every unit written from now on also holds min and max of each integer measure per group (column format minor 7) */
	bool		key_table;		/* the segments written from now on keep a key table of each key_columns leaf where it fits 3% of the segment */
	int			superunit;		/* units per superunit summary of the segments written from now on (column format minor 8); 0: none, else 2 to 256 */
	int			dead_rewrite_pct;	/* percent of dead rows above which maintenance rewrites a lone segment, 0: by the bytes of a row (maintenance only, not part of the format) */
	bool		vacuum_truncate;	/* VACUUM gives the free blocks at the end of the file back (maintenance only, not part of the format) */
	bool		cluster_advice;	/* sift.cluster_advice() may recommend an order for the table (advice only, not part of the format) */
} SiftColOptions;

/* dead_rewrite_pct 0, the default: a lone segment is rewritten once the scans of the rows of its dirty units cost more than the rewrite (rewrite_pays in col/maint/maint.c) */
#define SIFT_COL_DEAD_AUTO	0
/* Instructions a scan pays for each dead row of a unit (the unit answers net of them), for each row of a unit more than a tenth dead, and the scans expected between two VACUUMs */
#define SIFT_COL_DEAD_ROW	700
#define SIFT_COL_DEAD_SCAN_ROW	58
#define SIFT_COL_DEAD_SCANS	100
/* Instructions the rewrite takes a live row, and for each byte of it: measured on 6 to 17 bytes a row, with and without order arrays, 300,000 and 1,000,000 rows */
#define SIFT_COL_REWRITE_ROW	800
#define SIFT_COL_REWRITE_BYTE	230

extern const SiftColOptions *sift_col_options(Relation index);
extern const char *sift_col_options_paths(Relation index);	/* "" when none; the shredder parses it */
extern const char *sift_col_options_compression(Relation index);	/* "" when none (auto) */
extern const char *sift_col_options_preagg(Relation index);	/* "" when none (default) */
extern const char *sift_col_options_rollup_leaves(Relation index);	/* "" when none (automatic) */
extern const char *sift_col_options_rollup_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_calendar_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_key_columns(Relation index);	/* NULL when the option is not set (CREATE INDEX before its default is chosen, or an index written without it) */
extern const char *sift_col_options_point_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_point_tid_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_connect(Relation index);	/* "" when none */
extern const char *sift_col_options_row_key(Relation index);	/* "" when off, "auto" before the build has chosen the primary key */
extern const char *sift_col_options_count_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_key_cut(Relation index);	/* "" when none */
extern const char *sift_col_options_order_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_order_array_columns(Relation index);	/* NULL when not set: the build chooses */
extern const char *sift_col_options_order_layout(Relation index);	/* "" when not set (auto) */
extern const char *sift_col_options_order_array_group(Relation index);	/* "" when none */
extern const char *sift_col_options_key_order(Relation index);	/* "" when none */
extern const char *sift_col_options_hll_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_return_columns(Relation index);	/* "" when none */

extern PGDLLEXPORT Datum sift_col_handler(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_index_info(PG_FUNCTION_ARGS);	/* (index regclass): rows, segments, bytes, units, simd level name, ... */
extern PGDLLEXPORT Datum sift_col_extents(PG_FUNCTION_ARGS);	/* (index regclass): where every run of blocks of the file lies */
extern PGDLLEXPORT Datum sift_col_space(PG_FUNCTION_ARGS);	/* (index regclass): the free space of the file and what the shrink still has to move */
extern PGDLLEXPORT Datum sift_col_segments(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_leaves(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_sections(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_verify(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_promote(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_merge(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_maintain(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_maintenance_due(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_maintenance_status(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_index_advice(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_suggest_paths(PG_FUNCTION_ARGS);

#endif							/* SIFT_COL_PG_H */
