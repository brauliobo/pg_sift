/*
 * col/v1/v1_store_ops.h - Format 1 behind the storage seam of the unified engine (core/store_ops.h): a sift_col index or a sift_table of Format 1 is read, never written, through it.
 */
#ifndef SIFT_COL_V1_STORE_OPS_H
#define SIFT_COL_V1_STORE_OPS_H

#include "postgres.h"

#include "utils/snapshot.h"

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"

#include "agg/batch_source.h"
#include "col/v1/v1_reader.h"
#include "core/filter.h"
#include "core/store_ops.h"

struct SiftV1ReaderOps;

/* The unit reader of a sift_table (is_table) or of a sift_col index */
extern const struct SiftV1ReaderOps *sift_v1_reader_of(bool is_table);

/* Registers Format 1 with the storage seam (from the load of the library) */
extern void sift_v1_store_ops_init(void);

/* The zone-pruned aggregate scan, the dictionary answer and the parallel scan of Format 1 (v1_store_ops_zone.c, v1_store_ops_par.c) */
extern const SiftZoneOps sift_v1_zone_ops;
extern const SiftDictOps sift_v1_dict_ops;
extern const SiftParOps sift_v1_par_ops;

/* The optional sub-vtables of the Format 1 store (col/v1_engine/v1_store_ops_ext.c, v1_store_ops_jsonb.c) */
extern const SiftOrderOps v1_order_ops;
extern const SiftPreaggOps v1_preagg_ops;
extern const SiftJsonbOps v1_jsonb_ops;
extern const SiftHybridOps v1_hybrid_ops;

/* The predicates of a scan of a sift_table: the clauses of the statement's index quals over its attributes, and the executor whose parameters they read */
struct SiftStoreQuals
{
	List	   *clauses;
	EState	   *estate;
};

/* The leaf that holds attribute att alone (NULL: it has several, or is a document, a declared path or a list), and the type of the attribute */
extern const SiftSrcLeaf *sift_v1_att_leaf(const SiftSrcLayout *lay, int att);
extern Oid	sift_v1_att_typid(const SiftSrcLayout *lay, int att);

/* The superblocks [sb_lo, sb_hi) of the directory view the leader of a parallel scan exported (the view_export of the reader); NULL: the whole relation as it is now */
typedef struct SiftV1Range
{
	const void *view;
	uint32		sb_lo;
	uint32		sb_hi;
} SiftV1Range;

/* Rows of the leaf of the document path that have none of these JSON types (SIFT_SRC_JT_*) are not given */
typedef struct SiftV1TypeTest
{
	int			leaf;
	uint32		types;
} SiftV1TypeTest;

#define SIFT_V1_MAX_TYPE_TESTS	8

/* A consumer that answers a unit from what the unit stores: offer is true when it did, its rows are then not read; rd and src are the reader and its state, at the unit */
typedef struct SiftV1UnitSink
{
	bool		(*offer) (void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit);
	void	   *arg;
} SiftV1UnitSink;

/* What narrows a source besides the quals: the superblocks it reads, the units that hold a row of keep, the rows a text bridge holds (the score column of the input, SIFT_V6_SCORE_ATTNO, is theirs), the JSON types of leaves */
typedef struct SiftV1Scan
{
	const SiftV1Range *range;
	const SiftFilter *keep;
	const struct SiftTidBridge *bridge;
	int			ntypes;
	SiftV1TypeTest types[SIFT_V1_MAX_TYPE_TESTS];
	const SiftV1UnitSink *cube;	/* answers the units whose pre-aggregates stand for their rows (NULL: every unit is read) */
} SiftV1Scan;

/* The cube reader of an aggregate scan (col/v1_engine/v1_store_ops_preagg.c): NULL when the statement is not one the stored cubes answer or the scan reads fewer rows than a unit's; it lives in cx */
struct SiftSrcFoldArgs;
extern SiftV1UnitSink *sift_v1_cube_begin(const struct SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx);

/* The JSON type tests of the scan keys of an index scan (the other keys are the quals sift_srcfold_scankey_quals gives) */
extern SiftV1Scan sift_v1_key_scan(Relation index, ScanKey keys, int nkeys);

/* The batches of the columns of in over the snapshot, only the rows that pass quals (NULL: all) and scan (NULL: no more); the source and what it holds open live in parent */
extern SiftV6Source *sift_v1_source_open(MemoryContext parent, Snapshot snap, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftV1Scan *scan);

#endif							/* SIFT_COL_V1_STORE_OPS_H */
