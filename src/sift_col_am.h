/*
 * sift_col_am.h - the glue of the sift_col index access method (docs/COL.md section 12, L5): the per-relation cache, the conversion
 * of index tuples into column rows, and the entry points of the other glue files.
 */
#ifndef SIFT_COL_AM_H
#define SIFT_COL_AM_H

#include "access/amapi.h"

#include "sift_col.h"
#include "sift_col_enc.h"
#include "sift_col_mut.h"

/* The relcache entry's amcache: leaves, shredder, insert hint and a scratch context for aminsert */
typedef struct SiftColRel
{
	SiftColSchema *schema;
	SiftColIndexMeta meta;		/* cols point into schema->cols; stats zero */
	int			natts;
	int		   *first;			/* [natts] id of the first leaf of attribute i */
	int		   *nleaves;		/* [natts] */
	bool	   *json;			/* [natts + 1] the attribute is a shredded jsonb */
	SiftColShredAdapter *shred;
	SiftCcPolicy *policy;		/* [ncols] compression policy of each leaf (the compression option) */
	SiftCMutHint hint;
	MemoryContext rowcxt;		/* reset after every aminsert */
} SiftColRel;

/* The storage class of a type the engine holds natively (false: not natively); the scale of a numeric(p, s) leaf (false: not representable) */
extern bool sift_col_leaf_store(Oid typid, SiftColStore *store);
extern bool sift_col_numeric_scale(Form_pg_attribute att, uint8 *scale);

/* Built on first use from the index definition (ERROR for an unsupported type or a jsonb attribute without paths) */
extern SiftColRel *sift_col_rel(Relation index);

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
extern Datum sift_col_key_numeric(int scale, int64 key);

/* The leaves of one index tuple (values/isnull as the AM callbacks receive them), allocated in mcxt */
extern void sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row,
							  MemoryContext mcxt);

/* sift_col_am.c */
extern void sift_col_init(void);
extern IndexBuildResult *sift_col_ambuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void sift_col_ambuildempty(Relation index);
extern char *sift_col_ambuildphasename(int64 phase);
extern bool sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
							  IndexUniqueCheck checkUnique, bool indexUnchanged, struct IndexInfo *indexInfo);

/* sift_col_maint.c */
extern IndexBulkDeleteResult *sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
													IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern void sift_col_maint_after_seal(Relation index);
extern bool sift_col_maint_make_room(Relation index);
extern int	sift_col_maint_promote(Relation index, bool wait);
extern bool sift_col_maint_merge_step(Relation index, bool wait);
extern PGDLLEXPORT void sift_col_maint_worker_main(Datum main_arg);

#endif							/* SIFT_COL_AM_H */
