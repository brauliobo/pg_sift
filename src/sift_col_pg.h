/*
 * sift_col_pg.h - the PostgreSQL-bound declarations of the column engine: the relcache and catalog accessors of a sift_col index, the
 * planner's index expressions, the shredder adapter, the reloptions and the SQL entry points. sift_col.h keeps the host-independent
 * types and the key helpers, so the portable files (sift_col_enc.[ch], sift_colcodec.[ch], docs/COL-PORTS.md) never see this header.
 */
#ifndef SIFT_COL_PG_H
#define SIFT_COL_PG_H

#include "access/tupdesc.h"
#include "fmgr.h"
#include "nodes/nodes.h"

#include "sift_col.h"

/* The leaf list and statistics of a sift_col index (allocates in CurrentMemoryContext; the stats read the directory and the visibility map) */
extern SiftColIndexMeta *sift_col_index_meta(Relation index);

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

/* ---------------------------------------------------------------------------
 * 4. Shred adapter (sift_col_shred.c): the index's jsonb attributes, shredded by col-json's sift_shred.h
 * ------------------------------------------------------------------------- */

typedef struct SiftColShredAdapter SiftColShredAdapter;

/* Parses the "paths" reloption against the index; NULL when it has none. ERROR for a specification the engine cannot hold (deeper nesting than one list level, ...) */
extern SiftColShredAdapter *sift_col_shred_create(MemoryContext mcxt, Relation index);
/* Leaves the specification declares for index attribute attno, in the order sift_col_shred_row fills them (0: not shredded); parent is relative to the first */
extern int	sift_col_shred_nleaves(const SiftColShredAdapter *a, int attno);
extern void sift_col_shred_leaf(const SiftColShredAdapter *a, int attno, int i, SiftColDef *def);
/* All the shredded attributes of one tuple (values/isnull as the index build gives them, indexed by attribute) into row->val / row->elem, whose leaf ids start at first[attno] */
extern void sift_col_shred_row(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftColRow *row, const int *first, MemoryContext mcxt);

/* ---------------------------------------------------------------------------
 * 5. Options and SQL entry points (PG_FUNCTION_INFO_V1 are in sift_handler.c)
 * ------------------------------------------------------------------------- */

typedef struct SiftColOptions
{
	int32		vl_len_;
	int			mutable_limit;	/* kB */
	int			paths;			/* offset of the shredder's "paths" string, 0 = none */
	int			compression;	/* offset of the "compression" string, 0 = auto */
} SiftColOptions;

extern const SiftColOptions *sift_col_options(Relation index);
extern const char *sift_col_options_paths(Relation index);	/* "" when none; the shredder parses it */
extern const char *sift_col_options_compression(Relation index);	/* "" when none (auto) */

extern PGDLLEXPORT Datum sift_col_handler(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_index_info(PG_FUNCTION_ARGS);	/* (index regclass): rows, segments, bytes, units, simd level name, ... */
extern PGDLLEXPORT Datum sift_col_segments(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_leaves(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_verify(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_promote(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum sift_col_merge(PG_FUNCTION_ARGS);

#endif							/* SIFT_COL_PG_H */
