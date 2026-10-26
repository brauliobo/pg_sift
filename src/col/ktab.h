/*
 * col/ktab.h - the key table of a segment: for a leaf of key_columns, the distinct keys of the whole segment in ascending order,
 * the rows of each and, for SIFT_KTAB_MEAS integer leaves, their non-NULL count, exact sum, smallest and largest value: the histogram of the key (SiftCHistDim) of the
 * whole segment, so that `WHERE k = x` is a search of its keys, not a read of the cell lists of its units.
 */
#ifndef SIFT_COL_KTAB_H
#define SIFT_COL_KTAB_H

#include "core/host.h"

#include "col/preagg.h"

#define SIFT_KTAB_HEADER_BYTES 16
#define SIFT_KTAB_MAX_GROUPS	16384	/* the keys of a leaf in a segment beyond which it has no table */
#define SIFT_KTAB_PERMILLE		30		/* the tables of a segment together take at most this share of its bytes */

typedef struct SiftCKtab SiftCKtab;

/* spec->ktn and ktm: the leaves of spec->set that get a table when option key_table is on, and the measures of each (the writer picks the same) */
extern void sift_ktab_spec(const SiftColSchema *s, SiftCPaggSpec *spec);
/* The accumulator of a segment writer or of sift.col_verify: key[c] marks the leaves to tabulate; NULL when none */
extern SiftCKtab *sift_ktab_begin(const SiftColSchema *s, const bool *key, MemoryContext mcxt);
/* The next unit's leaves in the writer's order (SiftCPaggCol per leaf of the schema) */
extern void sift_ktab_add(SiftCKtab *t, const SiftCPaggCol *cols, uint32 nrows);
/* The tables that fit 3% of `bytes`, the size of the segment so far, appended to out one after the other, fewer measures before none */
extern void sift_ktab_finish(SiftCKtab *t, Size bytes, StringInfo out);
/* Is the stored table what the units added so far give: the same keys, rows and, for the measures it holds, counts, sums, minima and maxima */
extern bool sift_ktab_check(const SiftCKtab *t, const SiftCHistDim *stored);
/* The table at p as a histogram in cx; its bytes, 0 when it is not a table of this schema */
extern uint32 sift_ktab_decode(const SiftColSchema *s, const uint8 *p, uint32 len, MemoryContext cx, SiftCHistDim *out);

#endif
