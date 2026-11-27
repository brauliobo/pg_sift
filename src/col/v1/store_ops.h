/*
 * col/v1/store_ops.h - Format 1 behind the storage seam of the unified engine (core/store/ops.h): a sift_col index or a sift_table of Format 1 is read, never written, through it.
 */
#ifndef SIFT_COL_V1_STORE_OPS_H
#define SIFT_COL_V1_STORE_OPS_H

#include "postgres.h"

#include "utils/snapshot.h"

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "utils/jsonb.h"

#include "agg/batch_source.h"
#include "agg/vexpr/str.h"
#include "col/v1/reader.h"
#include "core/filter.h"
#include "core/store/ops.h"

struct SiftV1ReaderOps;

/* The unit reader of a sift_table (is_table) or of a sift_col index */
extern const struct SiftV1ReaderOps *sift_v1_reader_of(bool is_table);

/* Registers Format 1 with the storage seam (from the load of the library) */
extern void sift_v1_store_ops_init(void);

/* The zone-pruned aggregate scan, the dictionary answer and the parallel scan of Format 1 (v1_store_ops_zone.c, v1_store_ops_par.c) */
extern const SiftZoneOps sift_v1_zone_ops;
extern const SiftDictOps sift_v1_dict_ops;
extern const SiftStoreParOps sift_v1_par_ops;

/* The found flags of a connected index (col/v1/conn/found.c), and the leaf of the flag a scan reads as the column SIFT_V6_FOUND_ATTNO(c) */
extern const SiftConnOps sift_v1_conn_ops;
extern const SiftSrcLeaf *sift_v1_found_leaf(Relation index, const SiftSrcLayout *lay, int attno);

/* The keys of a document of the unified scan (col/v1_engine/store_ops_json.c): the JSON types the reader tests one at a time, in the order of the index a row's type is, and the value of a key from a document (NULL: none) */
#define SIFT_V1_NJSON_TYPES 7
typedef struct SiftV1JsonType
{
	uint32		test;			/* SIFT_SRC_JT_* */
	const char *name;			/* what jsonb_typeof gives; NULL for a missing key, whose jsonb_typeof is NULL */
} SiftV1JsonType;
extern const SiftV1JsonType sift_v1_json_types[SIFT_V1_NJSON_TYPES];
extern Jsonb *sift_v1_json_member(Datum doc, const char *key);

/* The optional sub-vtables of the Format 1 store (col/v1_engine/store_ops_ext.c, v1_store_ops_jsonb.c) */
extern const SiftOrderOps v1_order_ops;
extern const SiftPreaggOps v1_preagg_ops;
extern const SiftJsonbOps v1_jsonb_ops;
extern void sift_v1_unnest_run(const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, Snapshot snap, struct SiftV6ScanStats *stats, const struct SiftV6StarRun *star);
extern const SiftHybridOps v1_hybrid_ops;
extern const SiftScopeOps sift_v1_scope_ops;

/* The predicates of a scan of a sift_table: the clauses of the statement's index quals over its attributes, and the executor whose parameters they read */
struct SiftStoreQuals
{
	List	   *clauses;
	EState	   *estate;
};

/* The statistics of the leaves of an index as the planner reads them (col/v1_engine/store_card.c), and the first leaf of its column col (0-based) */
struct SiftCardStatsView;
extern const struct SiftCardStatsView *sift_v1_card_view(const SiftSrcLayout *lay, const SiftSrcStats *st);
extern int	sift_v1_card_leaf(const SiftSrcLayout *lay, int col);

/* The leaf that holds attribute att alone (NULL: it has several, or is a document, a declared path or a list), and the type of the attribute */
extern const SiftSrcLeaf *sift_v1_att_leaf(const SiftSrcLayout *lay, int att);
extern Oid	sift_v1_att_typid(const SiftSrcLayout *lay, int att);
/* the column of the heap row attribute att of the layout is; 0: an expression */
extern AttrNumber sift_v1_att_heap_column(const SiftSrcLayout *lay, int att);

/* The superblocks [sb_lo, sb_hi) of the directory view the leader of a parallel scan exported (the view_export of the reader); NULL: the whole relation as it is now */
typedef struct SiftV1Range
{
	const void *view;
	uint32		sb_lo;
	uint32		sb_hi;
} SiftV1Range;

/*
 * Rows of the leaf of the document path that have none of these JSON types (SIFT_SRC_JT_*) are not given. key: the test is doc ? 'k' of the key of the leaf, which a document that is no object
 * answers of itself: the rows the leaf has no value of are read again from the heap where the leaf cannot tell such a document from a missing key (rootless: an index with segments that do not flag
 * the string roots, all of them; else those that overflowed)
 */
typedef struct SiftV1TypeTest
{
	int			leaf;
	uint32		types;
	bool		key;
	bool		rootless;
} SiftV1TypeTest;

#define SIFT_V1_MAX_TYPE_TESTS	8

/* A consumer that answers a unit from what the unit stores: offer is true when it did, its rows are then not read; rd and src are the reader and its state, at the unit */
typedef struct SiftV1UnitSink
{
	bool		(*offer) (void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit);
	void	   *arg;
} SiftV1UnitSink;

#define SIFT_V1_MAX_ELEM_COLS	16	/* the columns of the elements of a shredded array a source reads (SIFT_V6_XELEM_COLS of the engine) */

/* The shredded array of a scan (an SCL_ARRAY leaf of the lengths with the child leaves of the columns of its elements) that the source reads of each unit besides the columns of its input; the child types are those of the vectors it gives */
typedef struct SiftV1ElemSpec
{
	bool		objects;		/* the elements are the objects of a jsonb array: one with a value in none of the columns may be no object */
	int			group_leaf;
	int			nchild;
	int			child_leaf[SIFT_V1_MAX_ELEM_COLS];
	SiftVecType child_type[SIFT_V1_MAX_ELEM_COLS];
	bool		child_kind[SIFT_V1_MAX_ELEM_COLS];	/* the column is the kind of the member (SIFT_ELEM_KIND_VALUE) the leaf holds, not its value: a row that has an element without one cannot tell a missing member from a JSON null */
} SiftV1ElemSpec;

/*
 * The elements of the rows of the last batch of a source opened with SiftV1Scan.elem; a batch is the rows of one unit. layout has the rows of the batch as its parents and the elements of the rows
 * the batch keeps (its mask) as its elements, child_data[c] and child_valid[c] the values of column c of the spec for them. poison: the rows whose array the leaves cannot hold (no array, an array
 * that was cut, an element that the child leaves cannot hold): their array is read from the heap row. doc_valid: the rows whose array is not NULL; a NULL array has no elements.
 */
typedef struct SiftV1ElemBatch
{
	SiftArrayLayout layout;
	const uint64 *poison;
	const uint64 *doc_valid;
	const void *child_data[SIFT_V1_MAX_ELEM_COLS];
	const uint64 *child_valid[SIFT_V1_MAX_ELEM_COLS];
} SiftV1ElemBatch;

/*
 * What narrows a source besides the quals: the superblocks it reads, the units that hold a row of keep, the rows a text bridge holds (the score column of the input, SIFT_V6_SCORE_ATTNO, is theirs),
 * the JSON types of leaves, the ctid range of the statement (SiftV6Spec.tid_range)
 */
typedef struct SiftV1Scan
{
	const SiftV1Range *range;
	const SiftFilter *keep;
	const SiftV1ElemSpec *elem;
	const struct SiftTidBridge *bridge;
	int			ntypes;
	SiftV1TypeTest types[SIFT_V1_MAX_TYPE_TESTS];
	const SiftV1UnitSink *cube;	/* answers the units whose pre-aggregates stand for their rows (NULL: every unit is read) */
	const struct SiftV6Bq *texts;	/* the batch quals of the fold: a unit no string of whose dictionary passes one of their text quals is not read (NULL: none) */
	bool		tid_range;
	uint64		tid_lo;
	uint64		tid_hi;
	const SiftF2Bound *bound;	/* a row Top-N by the leaf of index attribute bound_att: the units that cannot beat it are not read (sift_col_set_bound); NULL: none */
	int			bound_att;
} SiftV1Scan;

/* scan (NULL: none) bounded by the ctid range of spec */
struct SiftV6Spec;
extern SiftV1Scan sift_v1_spec_scan(const SiftV1Scan *scan, const struct SiftV6Spec *spec);

/* The cube reader of an aggregate scan (col/v1_engine/store_ops_preagg.c): NULL when the statement is not one the stored cubes answer or the scan reads fewer rows than a unit's; it lives in cx.
 * It folds the units it answers into a->group, or, for a participant of a parallel scan, into the group sift_v1_cube_set_group gives it before each morsel */
struct SiftSrcFoldArgs;
extern SiftV1UnitSink *sift_v1_cube_begin(const struct SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx);
extern void sift_v1_cube_set_group(SiftV1UnitSink *sink, struct SiftV6Group *g);

/* The JSON type tests of the scan keys of an index scan (the other keys are the quals sift_srcfold_scankey_quals gives) */
extern SiftV1Scan sift_v1_key_scan(Relation index, ScanKey keys, int nkeys);

/* The batches of the columns of in over the snapshot, only the rows that pass quals (NULL: all) and scan (NULL: no more); the source and what it holds open live in parent */
extern SiftV6Source *sift_v1_source_open(MemoryContext parent, Snapshot snap, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftV1Scan *scan);

/* The elements of the rows of the last batch of a source opened with a SiftV1Scan.elem */
extern const SiftV1ElemBatch *sift_v1_source_elems(SiftV6Source *src);

/* The view of the string code of a dictionary: a string of more than 12 bytes is copied into cx */
extern void sift_v1_strview(MemoryContext cx, SiftStrView *sv, const SiftSrcDict *dict, uint32 code);

/* The vector types the leaves of the shredded array of a layout are read as: false when the array or one of the columns of its elements is not one the elements of a source are read from (the array is read from the heap row) */
extern bool sift_v1_elem_spec(const SiftSrcLayout *lay, int group_leaf, int nchild, const int *child_leaf, const SiftVecType *child_type, const bool *child_kind, SiftV1ElemSpec *out);

/*
 * The float8 of a member of a map of a document (v1_store_ops_member.c) as a column of a source: the leaf of the values of the map named by the keys of path (a document attribute att; -1: the index holds
 * no such map), and its reader, which gives the member of each row of a unit, or marks the row poison (its value is read from the heap row with sift_v1_member_doc).
 */
typedef struct SiftV1Member SiftV1Member;
extern int	sift_v1_member_value_leaf(const SiftSrcLayout *lay, int att, const List *path);
extern SiftV1Member *sift_v1_member_new(MemoryContext cx, const SiftSrcLayout *lay, int value_leaf, const List *path);
extern int	sift_v1_member_leaf(const SiftV1Member *m, int part);
extern void sift_v1_member_bind(SiftV1Member *m, int part, int slot);
extern void sift_v1_member_unit(SiftV1Member *m, const SiftSrcVec *read);
extern void sift_v1_member_batch(SiftV1Member *m);
extern void sift_v1_member_put(SiftV1Member *m, uint32 o, uint32 n, double *data, uint64 *valid);
extern const uint64 *sift_v1_member_poison(const SiftV1Member *m);
extern bool sift_v1_member_doc(const SiftV1Member *m, Datum doc, double *value);

/* The elements reader of a source (v1_store_ops_elem.c), which reads the leaves of spec of each unit through the slots of the source's own reads */
typedef struct SiftV1Elems SiftV1Elems;
extern SiftV1Elems *sift_v1_elems_new(MemoryContext cx, const SiftV1ElemSpec *spec, const SiftSrcLayout *lay);
extern const SiftV1ElemBatch *sift_v1_elems_batch(SiftV1Elems *e, const SiftSrcVec *group, const SiftSrcVec *const *child, const uint32 *ords, int n, const uint64 *mask,
												  MemoryContext cx);

#endif							/* SIFT_COL_V1_STORE_OPS_H */
