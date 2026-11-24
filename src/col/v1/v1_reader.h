/*
 * col/v1/v1_reader.h - the unit reader of Format 1: the rows of a sift_col index or a sift_table table, unit by unit, as the vectors of col/v1/src_exec.h.
 *
 * A reader is opened on one relation with the scan's snapshot and read in superblock order: begin, then next gives the units, and for the unit it gave
 * live (the rows the tombstones leave) and vet (the rows the snapshot sees, when vet_needed) clear the bits of its rows in a bitmap of SIFT_COL_NWORDS(nrow) words, read decodes a leaf, tids names the
 * heap rows. select restricts the units begin starts to a match set. The index reader is col/v1/v1_reader.c, the table's is table/v1/v1_tbl_reader.c; the
 * layout and the statistics of a relation are read without opening a source.
 */
#ifndef SIFT_COL_V1_V1_READER_H
#define SIFT_COL_V1_V1_READER_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

#include "col/pg.h"
#include "col/v1/src_exec.h"
#include "core/filter.h"

typedef struct SiftV1ReaderOps
{
	SiftSrcLayout *(*layout) (Relation rel);
	void		(*stats) (Relation rel, SiftSrcStats *st);
	/* heap: the table the rows belong to; index: the sift_col index over it, NULL for a sift_table table. mcxt holds everything the reader allocates until close */
	void	   *(*open) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	void		(*close) (void *impl);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	/* the units begin starts: those with a row of the filter, which must outlive them (NULL: all rows) */
	void		(*select) (void *impl, const SiftFilter *keep);
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi);
	bool		(*next) (void *impl, SiftSrcUnit *unit);
	void		(*live) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	bool		(*vet_needed) (void *impl, const SiftSrcUnit *unit);
	void		(*vet) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	void		(*read) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
	void		(*tids) (void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
} SiftV1ReaderOps;

extern const SiftV1ReaderOps sift_v1_idx_reader;
extern const SiftV1ReaderOps sift_v1_tbl_reader;

/* The leaf of column def i of a shredded attribute (a sift_col index, or the declared paths of a sift_table): its place in the group tree, its path below the attribute, its kind and vector type; desc names the attribute and gives its type */
extern void sift_src_def_leaf(const SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l);
/* The numbers of SiftSrcStats of the leaves of cols from the zones and dictionaries; wovf[i], wjnull[i]: a write buffer holds an overflow value, a JSON null, of leaf i */
extern void sift_src_leaf_stats(const SiftColDef *cols, int ncols, const SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st);

extern SiftSrcLayout *sift_v1_idx_layout(Relation index);
extern void sift_v1_idx_stats(Relation index, SiftSrcStats *st);
/* A copy of the sample of the unit zones the planner may read; only the integer scalar leaves that are not wide. nunits: the units of the family */
extern const SiftSrcZones *sift_v1_idx_zones(const SiftColZones *zs, const SiftColIndexMeta *meta, double nunits);

#endif							/* SIFT_COL_V1_V1_READER_H */
