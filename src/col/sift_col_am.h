/*
 * sift_col_am.h - the glue of the sift_col index access method: the per-relation cache, the conversion
 * of index tuples into column rows, and the entry points of the other glue files.
 */
#ifndef SIFT_COL_AM_H
#define SIFT_COL_AM_H

#include "access/amapi.h"

#include "sift_col_pg.h"
#include "sift_col_seg.h"
#include "sift_col_enc.h"
#include "sift_col_preagg.h"
#include "sift_col_wbuf.h"

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
 * The on-demand refresh of the rollups (sift_col_rollup.c): a scan asks for the segment gen of index to be rewritten with the rollups the index writes now.
 * Writes no page; false when the request was not taken (switched off, a standby). The GUC sift.col_rollup_on_demand.
 */
extern bool sift_col_rollup_on_demand;
extern bool sift_col_rollup_worker;
extern bool sift_col_rollup_request(Relation index, uint64 gen);
#ifdef SIFT_TESTING
extern int	sift_test_col_rollup_panic;
extern int	sift_test_col_rollup_panic_armed;
#endif

/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a preagg option ("default", "off", "cube(dim: measure, ...; ...)"), as far as it can tell without the index */
extern void sift_col_validate_preagg(const char *value);
extern void sift_col_validate_rollup(const char *value);
/* Raises ERRCODE_INVALID_PARAMETER_VALUE unless value is a compression option ("policy" or "policy; column=policy; ...") */
extern void sift_col_validate_compression(const char *value);
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

/* Testing builds: sift.test_col_scan_jitter_us sleeps up to that long at the points of a scan where another backend (VACUUM, DML, maintenance)
 * can change what the scan has read and not yet read: after open, after a unit is loaded, between the all-visible mask and the liveness,
 * before the heap checks. Widens the races deterministically in place of host load. */
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_col_jitter_us;
extern PGDLLIMPORT bool sift_test_col_paranoid;	/* sift.test_col_paranoid: rows on all-visible pages are checked against the heap too, and a disagreement is logged */
extern void sift_col_jitter(void);
#define SIFT_COL_JITTER()	sift_col_jitter()
#else
#define SIFT_COL_JITTER()	((void) 0)
#endif

#endif							/* SIFT_COL_AM_H */
