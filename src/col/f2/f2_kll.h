/*
 * col/f2/f2_kll.h - the quantile sketches of a part of format major 2 (SEC_KLL): the columns the option sketches = 'kll(...)' names, the sketches a part flush builds from its rows, the read of
 * the section of a part, and the compaction that adds the sketches of its source parts instead of reading their rows. A section keeps one sketch per column for the whole part; an entry
 * of a column that had no value is there too, so a part that lacks the entry was written before the option.
 */
#ifndef SIFT_F2_KLL_H
#define SIFT_F2_KLL_H

#include "core/host.h"

#include "col/f2/part_format.h"
#include "col/granule/granule.h"
#include "col/kll.h"
#include "core/f2/manifest.h"

struct SiftColRel;

#define SIFT_F2_KLL_MAX_COLS	256

/* The share of the rows of a query that may be dead, pending or without a sketch for the answer to stay a sketch's: 1%, not a setting */
#define SIFT_F2_KLL_DIRTY_PPM	10000

typedef struct SiftF2KllCols
{
	int			n;
	uint16		col[SIFT_F2_KLL_MAX_COLS];	/* the leaf, ascending */
	uint16		k[SIFT_F2_KLL_MAX_COLS];
} SiftF2KllCols;

typedef struct SiftF2KllBuild
{
	MemoryContext cxt;
	int			n;
	const uint16 *col;
	SiftKll    *sk;
} SiftF2KllBuild;

typedef struct SiftF2KllSec
{
	uint8	   *data;
	uint32		len;
} SiftF2KllSec;

/* The validator of the sketches option of sift_col (the hll groups are checked by the HLL one) */
extern void sift_f2_kll_validate(const char *value);

/* The columns of the kll groups of the option, as leaves of the index; ERROR for a name the index lacks or a type with no order to sketch */
extern void sift_f2_kll_columns(Relation index, const struct SiftColRel *r, const char *sketches, SiftF2KllCols *out);

/* The columns of the hll groups of the option as a list of hll_columns ("a, b"; empty for none) */
extern char *sift_f2_kll_hll_columns(const char *sketches);

/* int2, int4, int8, float4, float8, numeric, date, timestamp, timestamptz: the value as a float8 (a date as days, a timestamp as microseconds, of the PostgreSQL epoch); false for NaN and for an infinite date or timestamp, which are no values */
extern bool sift_f2_kll_type_ok(Oid type);
extern bool sift_f2_kll_datum(Oid type, Datum d, double *out);

/* One sketch for each column, seeded from the generation of the part so a rebuild gives the same bytes */
extern SiftF2KllBuild *sift_f2_kll_begin(MemoryContext cxt, const SiftF2KllCols *cols, uint64 part_gen);
extern void sift_f2_kll_free(SiftF2KllBuild *b);

/* The values of a column of the rows a part is written from (SiftRows.cols[c]): an integer cell divided by 10^scale (a numeric is one), a float cell as it is; NULL cells and NaN are no values */
extern void sift_f2_kll_add_col(SiftF2KllBuild *b, int slot, const SiftColIn *c, uint32 nrows);
extern void sift_f2_kll_add_datums(SiftF2KllBuild *b, int slot, Oid type, const Datum *vals, const bool *isnull, uint32 n);

/* Adds SEC_KLL to the sections of the part written (optional: flags 0, so a reader of format major 2 that does not know it skips it) */
extern void sift_f2_kll_add_section(SiftF2KllBuild *b, SiftSecInput *secs, int *nsec);

/* The section of a part, checked (ERROR when it is damaged); false when the part has none */
extern bool sift_f2_kll_load(Relation rel, const SiftPart *part, SiftF2KllSec *out);
/* The sketch of col of the part into dst; false when the section has no entry for it */
extern bool sift_f2_kll_merge_col(const SiftF2KllSec *sec, uint16 col, SiftKll *dst);

/*
 * The compaction of a part into a new one: true when the sketches of the source were added to the build, false when its rows must be fed to sift_f2_kll_add_rows instead (it has dead rows, whose
 * values a sketch cannot give back, or no sketch of one of the columns). Nothing is added when false.
 */
extern bool sift_f2_kll_merge_part(SiftF2KllBuild *b, Relation rel, const SiftPart *part, const SiftPartDesc *desc);

#endif							/* SIFT_F2_KLL_H */
