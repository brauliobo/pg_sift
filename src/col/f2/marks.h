/*
 * col/f2/marks.h - the key marks (SEC_KEY_MARKS), the column statistics (SEC_COL_STATS) and the read ahead of format major 2 as the writer of a part and the scan of an index use them: the glue
 * between the portable files (col/f2/key/marks.h, key_plan.h, col_stats.h, f2_prefetch.h) and the leaves, the rows of a part in memory and the opened part.
 *
 * The marks cover the whole of key_order: an integer, a date, a timestamp, a boolean, an exact numeric held as a scaled integer, a float (as a float8), the bytes of a text and the 16 bytes of a uuid, each
 * in the order the rows of a part are sorted in.
 */
#ifndef SIFT_F2_MARKS_H
#define SIFT_F2_MARKS_H

#include "col/f2/f2.h"
#include "col/f2/stats.h"
#include "col/f2/key/plan.h"
#include "col/granule/granule.h"
#include "agg/filter_adapt.h"
#include "col/f2/lazy.h"
#include "col/f2/prefetch/prefetch.h"

/* The spec of the marks from the cells and the chunk hints of the key columns (flags: SIFT_KEY_*, NULL: ascending); the same for the writer and the scan */
extern bool sift_f2_key_spec(const SiftCellKind *kinds, const uint8 *hints, const uint8 *flags, int n, SiftKeySpec *spec);
extern bool sift_f2_rel_key_spec(const SiftF2Rel *f, SiftKeySpec *spec);

/* The relation as the shared functions of the index scan read it: leaves, kinds, the key and the time columns, all of the store's */
extern SiftF2Rel sift_f2_store_rel(const struct SiftF2Store *s);

/*
 * The section for the part whose rows are in order, granule g being rows [gstart[g], gstart[g + 1]): NULL when the part gets none (a NaN is among the floats, a number overflowed its scale, the rows
 * are not sorted by the key or the marks do not ascend). The section is palloc'd.
 */
extern void *sift_f2_key_marks_build(const SiftRows *rows, int ng, const int *gstart, uint64 *len);

/* The marks of an opened part, read once and kept with it; NULL when the part has none */
extern const SiftKeyMarks *sift_part_key_marks(Relation rel, const SiftPart *part, const SiftF2Rel *f);

/* The key ranges the predicates of a scan leave (SIFT_KEY_PLAN_FULL when the key has no byte order or no predicate is on it) */
extern void sift_f2_key_plan(const SiftF2Rel *f, const SiftColDef *cols, const SiftColPred *preds, int npreds, SiftKeyPlan *plan);

/* The sections of a part joined of parts that have them, as they are (the granules follow one another): the marks laid after each other and the statistics added; ext_flags gets what was written */
extern void sift_f2_marks_join(Relation rel, SiftPart *const *src, int n, SiftSecInput *secs, int *nsec, uint8 *ext_flags);

/* The statistics of a part in the making (sift_col_stats_begin): a granule of column c at a time, then the section (palloc'd) */
extern void sift_f2_col_stats_granule(SiftColStatsBuilder *b, int c, const SiftColIn *col, int nrows, uint32 nnull, const SiftChunkRef *ref);
extern void *sift_f2_col_stats_finish(SiftColStatsBuilder *b, int nc, SiftDictEnc *const *dicts, uint32 ng, uint64 *len);

/* The statistics of an opened part (false: it has none) */
extern bool sift_part_col_stats(Relation rel, const SiftPart *part, SiftColStats *cs);

/*
 * The filter of a scan set for the part it is about to read: a cold part (level 2 or in the object storage of the tier) is read with the weight of a byte from disk, a warm one with that of a byte
 * from shared buffers; and the predicates ranked from what the part says of their columns before a granule has taught the filter anything: that is not redone once the filter has seen a granule.
 */
extern void sift_f2_seed_filter(Relation rel, const SiftPart *part, const SiftPartDesc *d, const SiftColPred *preds, int npreds, SiftAdaptiveFilter *af);

/* The read ahead of the pages of the granules [spans) of a part (col/f2/prefetch/prefetch.h): the pages asked of the storage */
extern uint64 sift_f2_prefetch_spans(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const uint32 spans[][2], int nspans);

#endif							/* SIFT_F2_MARKS_H */
