/*
 * col/v1/reader.h - the unit reader of Format 1: the rows of a sift_col index or a sift_table table, unit by unit, as the vectors of col/v1/src_exec.h.
 *
 * A reader is opened on one relation with the scan's snapshot and read in superblock order: begin (which skips the units the zones exclude), then next gives the units, and for the unit it gave
 * live (the rows the tombstones leave) and vet (the rows the snapshot sees, when vet_needed) clear the bits of its rows in a bitmap of SIFT_COL_NWORDS(nrow) words, read decodes a leaf, tids names the
 * heap rows. select restricts the units begin starts to a match set. The index reader is col/v1/reader.c, the table's is table/v1/reader.c; the
 * layout and the statistics of a relation are read without opening a source.
 */
#ifndef SIFT_COL_V1_V1_READER_H
#define SIFT_COL_V1_V1_READER_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

#include "agg/batch_source.h"
#include "col/pg.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/src_exec.h"
#include "core/filter.h"

typedef struct SiftV1ReaderOps
{
	SiftSrcLayout *(*layout) (Relation rel);
	void		(*stats) (Relation rel, SiftSrcStats *st);
	/* the pre-aggregates its segments are written with (SiftSrcPreagg.aggs is always set: every unit has the sums of its integer leaves) */
	void		(*preagg) (Relation rel, SiftSrcPreagg *pa);
	/* the leaf the units of an index are cut by, -1: none (a sift_table's units are not) */
	int			(*key_leaf) (Relation rel);
	/* heap: the table the rows belong to; index: the sift_col index over it, NULL for a sift_table table. mcxt holds everything the reader allocates until close.
	 * view: what view_export wrote for another reader of the statement (a parallel worker reads the directory of its leader), NULL: the relation as it is now */
	void	   *(*open) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view);
	Size		(*view_size) (void *impl);
	void		(*view_export) (void *impl, void *buf);
	void		(*close) (void *impl);
	/* adds the units its zones and set summaries ruled out, and the rows it checked against the heap, to out */
	void		(*pruned) (void *impl, SiftV6GranCounts *out);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	/* the units begin starts: those with a row of the filter, which must outlive them (NULL: all rows) */
	void		(*select) (void *impl, const SiftFilter *keep);
	/* a unit whose zones lie outside an entry of prune (which must outlive the scan) is not given; a unit that is given still holds rows the entries exclude */
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
	bool		(*next) (void *impl, SiftSrcUnit *unit);
	void		(*live) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	bool		(*vet_needed) (void *impl, const SiftSrcUnit *unit);
	void		(*vet) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* the rows of bits whose value in the leaf has none of the JSON types (SIFT_SRC_JT_*) are cleared; false when none is left. *known is false, and bits are not to be used, when the unit cannot tell the types apart */
	bool		(*filter_types) (void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits, bool *known);
	void		(*read) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
	/* sift_col_unit_dict_match over the dictionary of text leaf leaf that read gives the unit */
	bool		(*dict_match) (void *impl, const SiftSrcUnit *unit, int leaf, SiftColOp op, const SiftColValue *c, uint64 *lut);
	void		(*tids) (void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
	/* the rows the unit answers for from its pre-aggregates, the live rows the snapshot sees (stored, every heap page all-visible, no dead row or a sidecar net of them), so the two below
	 * answer for them; -1: the unit is read */
	int			(*unit_rows) (void *impl, const SiftSrcUnit *unit);
	/* the zone and the exact sums of a leaf of a unit unit_rows answers for; false when it has none (an overflow value, a leaf the segment lacks) */
	bool		(*unit_agg) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
	/* GROUP BY dim over a unit unit_rows answers for, with the measures need names; the arrays live until the next call; false when the unit holds no histogram of dim or lacks a measure */
	bool		(*unit_hist) (void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h);
	/* the stored segment the unit is of (NULL: a unit of memory, or a reader with no segments), and count(*) of the rows of that segment whose string of leaf passes op against c,
	 * from the row counts of its dictionary, when every unit of it is exact; false: the segment is read */
	const void *(*unit_seg) (void *impl, const SiftSrcUnit *unit);
	bool		(*seg_count) (void *impl, const SiftSrcUnit *unit, int leaf, SiftColOp op, const SiftColValue *c, uint64 *rows);
} SiftV1ReaderOps;

extern const SiftV1ReaderOps sift_v1_idx_reader;
extern const SiftV1ReaderOps sift_v1_tbl_reader;

/* Adds the units the zones and the set summaries of a column source ruled out, and the rows it checked against the heap, to out */
extern void sift_v1_units_pruned(const SiftColSource *src, SiftV6GranCounts *out);

/* The leaf of column def i of a shredded attribute (a sift_col index, or the declared paths of a sift_table): its place in the group tree, its path below the attribute, its kind and vector type; desc names the attribute and gives its type */
extern void sift_src_def_leaf(const SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l);
/* The numbers of SiftSrcStats of the leaves of cols from the zones and dictionaries; wovf[i], wjnull[i]: a write buffer holds an overflow value, a JSON null, of leaf i */
extern void sift_src_leaf_stats(const SiftColDef *cols, int ncols, const SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st);

/* What a stored unit answers (col/v1/reader_unit.c); def is the definition of col in the unit's schema, the maps translate the leaves of the source and the columns of the schema (NULL: the same) */
extern int	sift_v1_unit_rows(SiftColUnit *unit);
extern bool sift_v1_unit_agg(const SiftColUnit *unit, int col, const SiftColDef *def, SiftSrcUnitAgg *agg);
extern bool sift_v1_unit_hist(const SiftColUnit *unit, int col, const SiftColDef *def, MemoryContext mcxt, const int *col_of_leaf, const int *leaf_of_col, const SiftSrcRollupNeed *need,
							  SiftSrcDict *hdict, SiftSrcHist *h);

/* The histograms a spec writes, as the planner sees them */
extern void sift_src_preagg_spec(const SiftCPaggSpec *spec, SiftSrcPreagg *pa);
extern void sift_v1_idx_preagg(Relation index, SiftSrcPreagg *pa);
extern int	sift_v1_idx_key_leaf(Relation index);
/* The column source of an index reader (sift_v1_idx_reader.open): what the global order of a leaf is read through (col/api/source.h) */
extern SiftColSource *sift_v1_idx_colsource(void *impl);
/* sift_v1_idx_reader.open confined to the partitions of the key_cut that may hold keys (NULL: all of them) */
extern void *sift_v1_idx_open_keys(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view, const SiftKeyRange *keys);

extern SiftSrcLayout *sift_v1_idx_layout(Relation index);
extern void sift_v1_idx_stats(Relation index, SiftSrcStats *st);
/* The statistics of the scope units of the index, the fields a read of them prices by its units; false: it has no scope units */
extern bool sift_v1_idx_scope_stats(Relation index, SiftSrcStats *st);
/* A copy of the sample of the unit zones the planner may read; only the integer scalar leaves that are not wide. nunits: the units of the family */
extern const SiftSrcZones *sift_v1_idx_zones(const SiftColZones *zs, const SiftColIndexMeta *meta, double nunits);

#endif							/* SIFT_COL_V1_V1_READER_H */
