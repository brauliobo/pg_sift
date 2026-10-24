/*
 * col/runs.h - the sorted run of a leaf in a unit: the ordinals of the rows of the unit in the order of the
 * leaf's key, so that ORDER BY .. LIMIT reads the first rows of each unit and not all of them.
 */
#ifndef SIFT_COL_RUNS_H
#define SIFT_COL_RUNS_H

#include "core/host.h"

#include "col/preagg.h"

#define SIFT_CRUN_HDR		16
#define SIFT_CRUN_ORD		1		/* the ordinals, ascending by (key, ordinal) */
#define SIFT_CRUN_IDENT		2		/* the keys already ascend with the ordinals and no row is NULL or overflowed: no entries */
#define SIFT_CRUN_HEAD_MAX	32		/* the most entries of a run head at each end (column format minor 18) */
#define SIFT_CRUN_HEAD_ROWS	128		/* a unit gets a head of n / 128 entries at each end, at most SIFT_CRUN_HEAD_MAX: the head stays under 0.25% of a column of the unit */

typedef struct SiftCRun
{
	uint8		kind;
	uint8		width;			/* bytes of an entry: 1, 2 or 4 (ORD only) */
	uint32		n;				/* rows of the unit */
	uint32		nvals;			/* rows with a value: the first entries, then nnull NULL rows ascending, then the rows that overflowed ascending */
	uint32		nnull;
	uint8		heads;			/* H: the unit also stores its first and last H keys of the run with the heap addresses of their rows (0: none) */
} SiftCRun;


/* The option order_columns, resolved against a schema and the names of its leaves: NULL, or why it is not valid */
extern const char *sift_crun_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_crun_syntax(const char *opt);
/* A plain integer-like, date, timestamp, numeric (scaled integer) or float attribute of the top level: what an ORDER BY of the scan can key on */
extern bool sift_crun_leaf_ok(const SiftColSchema *s, const SiftColDef *d);

/* The key a row sorts by: the chunk's key, with -0 and +0 of a float the same key (PostgreSQL compares them equal) */
static inline int64
sift_crun_key(const SiftColDef *def, int64 key)
{
	return (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8) && key == -1 ? 0 : key;
}

/*
 * Appends the run of the leaf to out and returns true, or appends nothing and returns false when the unit has fewer than two rows. addr (the heap
 * address of each row, NULL: no heads) and heads: a unit with no NULL or overflowed row also stores the head of its run (SiftCRun.heads).
 */
extern bool sift_crun_build(const SiftColDef *def, const SiftCPaggCol *c, const uint64 *addr, bool heads, StringInfo out);

/* The entries of the head of a run of n rows with H: nlo from the low end, then nhi from the high end (best first when it is read descending) */
extern void sift_crun_head_counts(uint8 heads, uint32 n, uint32 *nlo, uint32 *nhi);
/* The bytes of the run with its head: the offset of the head is sift_crun_bytes */
extern uint32 sift_crun_total(const SiftCRun *r);

/* Parses the header of a stored run of a unit of nrows rows; false when it is malformed (kind, width, counts, length) */
extern bool sift_crun_head(const uint8 *blob, uint32 len, uint32 nrows, SiftCRun *r);

/* The bytes of a run of nrows rows, or of its header when it has no entries (kind 2) */
extern uint32 sift_crun_bytes(uint8 kind, uint32 nrows);

/* Entries [pos, pos + cnt) of the run as ordinals into out; false when one is not a row of the unit. e: the bytes after the header (cnt entries from pos) */
extern bool sift_crun_entries(const SiftCRun *r, const uint8 *e, uint32 pos, uint32 cnt, uint32 *out);

/* Is the stored blob what the writer makes from the leaf (the verifier) */
extern bool sift_crun_matches(const SiftColDef *def, const SiftCPaggCol *c, const uint64 *addr, const uint8 *blob, uint32 len);

#endif							/* SIFT_COL_RUNS_H */
