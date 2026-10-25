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

/* Per leaf (ncols entries, memoized per backend, valid until the index changes or the memo of 8192 indexes is dropped): what the zones and dictionaries of the immutable segments say, no chunk read */
extern const SiftColLeafStats *sift_col_leaf_stats(Relation index);
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
/* What the planner takes of an index with its meta (sift_col_index_meta): the memos of the three functions above for the directory it read */
typedef struct SiftColLeafMemo
{
	const SiftColLeafStats *leaves;
	const SiftColZones *zones;
	const SiftColPreStats *pre;
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
extern Oid	sift_col_am_oid(void);

extern PGDLLIMPORT bool sift_enable_col;
extern PGDLLIMPORT int sift_col_format_minor;	/* the minor of the segments written: 4, or 3 to stay readable by 3.0 */

/* ---------------------------------------------------------------------------
 * 4. Shred adapter (col/jsonb.c): the index's jsonb attributes, shredded by col-json's col/shred.h
 * ------------------------------------------------------------------------- */

typedef struct SiftColShredAdapter SiftColShredAdapter;

/* Parses the "paths" reloption against the index; NULL when it has none. ERROR for a specification the engine cannot hold (deeper nesting than one list level, ...) */
extern SiftColShredAdapter *sift_col_shred_create(MemoryContext mcxt, Relation index);
/* The same for a paths specification over the first natts attributes of a tuple descriptor (a sift_table's declared paths) */
extern SiftColShredAdapter *sift_col_shred_create_for(MemoryContext mcxt, TupleDesc desc, int natts, const char *paths);
/* Leaves the specification declares for index attribute attno, in the order sift_col_shred_row fills them (0: not shredded); parent is relative to the first */
extern int	sift_col_shred_nleaves(const SiftColShredAdapter *a, int attno);
extern void sift_col_shred_leaf(const SiftColShredAdapter *a, int attno, int i, SiftColDef *def);
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

typedef struct SiftColOptions
{
	int32		vl_len_;
	int			write_buffer_limit;	/* kB */
	int			paths;			/* offset of the shredder's "paths" string, 0 = none */
	int			compression;	/* offset of the "compression" string, 0 = auto */
	int			preagg;			/* offset of the "preagg" string (docs/COL-PREAGG.md), 0 = default */
	int			rollup;			/* offset of the "rollup" string (the dims of the element rollups, docs/COL-PREAGG.md), 0 = automatic */
	int			calendar_columns;	/* offset of the "calendar_columns" string (the date and timestamp columns that get calendar cells in every unit, docs/COL-PREAGG.md section 2; column format minor 8), 0 = none */
	int			rollup_dims;	/* offset of the "rollup_dims" string (the columns of the rollup of the rows of a unit, docs/COL-PREAGG.md; column format minor 8), 0 = none */
	bool		rollup_on_demand;	/* a SELECT that scans a unit for lack of its rollup asks for a refresh (docs/COL-PREAGG.md section 7) */
	int			return_columns;	/* offset of the "return_columns" string (the text, varchar and uuid columns an index-only scan returns), 0 = none */
	int			key_columns;	/* offset of the "key_columns" string, 0 = not set */
	int			key_cut;		/* offset of the "key_cut" string, 0 = none */
	int			key_cut_rows;	/* rows a key-cut unit aims at, 0 = as many as an average superblock holds */
	int			order_columns;	/* offset of the "order_columns" string, 0 = none */
	int			hll_columns;	/* offset of the "hll_columns" string, 0 = none */
	int			point_columns;	/* offset of the "point_columns" string, 0 = none */
	int			point_tid_columns;	/* offset of the "point_tid_columns" string, 0 = none */
	int			dict_lookup;	/* SIFT_COL_DICT_*: which dictionaries of many strings are stored with the order of their strings, to be searched in place (column format minor 14) */
	int			order_array_columns;	/* offset of the "order_array_columns" string, 0 = none */
	bool		cube_minmax;	/* the cube of every unit written from now on also holds min and max of each integer measure per group (column format minor 7) */
	int			superunit;		/* units per superunit summary of the segments written from now on (column format minor 8); 0: none, else 2 to 256 */
	int			dead_rewrite_pct;	/* percent of dead rows above which maintenance rewrites a lone segment (maintenance only, not part of the format) */
	bool		vacuum_truncate;	/* VACUUM gives the free blocks at the end of the file back (maintenance only, not part of the format) */
	bool		cluster_advice;	/* sift.cluster_advice() may recommend an order for the table (advice only, not part of the format) */
} SiftColOptions;

/* A unit with a dead row answers from its rows, not from its pre-aggregates: a lone segment is rewritten once a tenth of its rows is dead */
#define SIFT_COL_DEAD_PCT	10

extern const SiftColOptions *sift_col_options(Relation index);
extern const char *sift_col_options_paths(Relation index);	/* "" when none; the shredder parses it */
extern const char *sift_col_options_compression(Relation index);	/* "" when none (auto) */
extern const char *sift_col_options_preagg(Relation index);	/* "" when none (default) */
extern const char *sift_col_options_rollup(Relation index);	/* "" when none (automatic) */
extern const char *sift_col_options_rollup_dims(Relation index);	/* "" when none */
extern const char *sift_col_options_calendar_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_key_columns(Relation index);	/* NULL when the option is not set (CREATE INDEX before its default is chosen, or an index written without it) */
extern const char *sift_col_options_point_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_point_tid_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_key_cut(Relation index);	/* "" when none */
extern const char *sift_col_options_order_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_order_array_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_hll_columns(Relation index);	/* "" when none */
extern const char *sift_col_options_return_columns(Relation index);	/* "" when none */

extern PGDLLEXPORT Datum sift_col_handler(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_index_info(PG_FUNCTION_ARGS);	/* (index regclass): rows, segments, bytes, units, simd level name, ... */
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
