/*
 * col/f2/f2_v6_proj_sort_exec.h - the rows of a statement from a SIFT_PROJ_SORT projection: ORDER BY ... LIMIT k and key-range scans answered in the order of the projection, for a sift_col index and
 * a sift_table alike. The rows of every projection part the projection stands for (a sealed part with the deletion vector it was written from, every address visible to every snapshot) are
 * read lazily in the part's own order and merged with the rows of everything it does not stand for (the parts written since, the parts with a deletion vector, the partitions with deletes
 * pending, the slabs), which the host's scan hands out under the snapshot's visibility and which are kept in a bounded heap. The rows come out in the order of the projection's sort columns:
 * ascending or descending, NULLS FIRST or LAST, as its spec says. The caller asks for an order the projection has (f2_v6_proj.h: sift_v6_sort_use), and for a text column only when the collation
 * of the statement compares bytes (f2_v6_proj_key.h: sift_v6_proj_collation_bytewise), since the projection keeps its strings in the order of their bytes.
 *
 * A part with key marks is sought: the predicates on its sort columns become ranges of the key space and only the granules the marks say can hold them are read, found by binary search over the
 * marks (col/f2/f2_v6_proj_seek.h), then the part is read no further than the row past the last its first sort column can accept (an ascending column) or the rows the limit needs. A row that
 * holds an exception tag (a value its column could not keep) in a column a predicate tests is decided by the caller's vet function on the relation's own row before it counts toward the limit,
 * so every row handed out satisfies the statement's predicates; with vet_all the statement has quals no cell can judge and every row is vetted.
 *
 * A part the projection stands for through an overlay (SPS_OVERLAY) is read with the addresses its deletion vector has dead for the snapshot and its partition's pending deletes left out.
 * A statement that holds the leading sort column to a list (k0 = ANY (..)) reads one sought stream of every part for each value and merges the streams in the order of the statement.
 */
#ifndef SIFT_COL_F2_V6_PROJ_SORT_EXEC_H
#define SIFT_COL_F2_V6_PROJ_SORT_EXEC_H

#include "col/f2/f2_v6_proj_host.h"
#include "col/f2/f2_v6_proj.h"

typedef struct SiftV6SortScan SiftV6SortScan;

/* Whether the row at tid is visible in the relation (a HOT chain followed from its root) and satisfies the statement's quals: for a row whose cells cannot judge them */
typedef bool (*SiftV6SortVetFn) (void *arg, uint64 tid);

/*
 * q is a row query: wants_rows, the columns it reads (nread, read: leaves), its ORDER BY (norder, order) and its limit (limit, -1: none). h->preds are its predicates, tested on the cells of every
 * row; their columns are read too. NULL: no projection answers the statement, read the relation (nothing was changed). Allocates in cx.
 */
extern SiftV6SortScan *sift_v6_proj_sort_begin(const SiftV6ProjHost *h, const SiftV6ProjQuery *q, SiftV6SortVetFn vet, void *vet_arg, bool vet_all, MemoryContext cx);

/* The next row: its address and one cell for each column of the relation, of which the columns the query reads are set (the others are NULL); valid until the next call. false after the last. */
extern bool sift_v6_proj_sort_next(SiftV6SortScan *s, uint64 *tid, const SiftCell **row);

extern const char *sift_v6_proj_sort_name(const SiftV6SortScan *s);

/* For EXPLAIN: projection parts read and how many of them were sought by their marks, base parts read from the relation, rows the relation's scan found, granules left out and read so far */
typedef struct SiftV6SortStats
{
	int			nparts;
	int			nseeked;
	int			nbase;
	int64		nrest_rows;
	uint32		granules_skipped;
	uint32		granules_read;
} SiftV6SortStats;

extern SiftV6SortStats sift_v6_proj_sort_stats(const SiftV6SortScan *s);

extern void sift_v6_proj_sort_end(SiftV6SortScan *s);

#endif							/* SIFT_COL_F2_V6_PROJ_SORT_EXEC_H */
