/*
 * col/v1_engine/store_ops_rollup.h - the statements a Format 1 unit answers from the rollup of its rows (rollup_columns, docs/COL-PREAGG.md) for SiftAggV6 (col/v1_engine/store_ops_preagg.c):
 * a GROUP BY of two leaves, or tests of several leaves, which no histogram of one leaf holds. The tuples of the rollup are the groups of the unit by every leaf the statement reads; those its
 * tests keep fold into the groups of their keys.
 */
#ifndef SIFT_V1_STORE_OPS_ROLLUP_H
#define SIFT_V1_STORE_OPS_ROLLUP_H

#include "postgres.h"

#include "col/v1/reader.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

typedef struct SiftV1Rollup SiftV1Rollup;

/*
 * The rollup of a statement grouped by the leaves key[0 .. nkeys - 1] (key_text: a string leaf), with the tests pred[0 .. npred - 1] and the measures of need, in cx: its dims are the keys and
 * the leaves of the tests. NULL when they are fewer than two leaves (a histogram holds them) or more than a rollup holds.
 */
extern SiftV1Rollup *sift_v1_rollup_open(MemoryContext cx, const int *key, const bool *key_text, int nkeys, const SiftV1Pred *pred, int npred, const SiftSrcRollupNeed *need);

/* The tuples of the rollup of the rows of the unit as the groups of h (their counts and measures; no key); false when the unit holds no rollup with every dim */
extern bool sift_v1_rollup_read(SiftV1Rollup *ru, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, SiftSrcHist *h);

/* Whether the rows of tuple t of the last read pass the tests */
extern bool sift_v1_rollup_kept(const SiftV1Rollup *ru, uint32 t);

/* Row row of the next key vectors is the keys of tuple t */
extern void sift_v1_rollup_put(SiftV1Rollup *ru, uint32 t, int row);

/* The key vectors of rows 0 .. n - 1 put since the last call, valid until the next one */
extern SiftVec *const *sift_v1_rollup_keys(SiftV1Rollup *ru, int n);

#endif							/* SIFT_V1_STORE_OPS_ROLLUP_H */
