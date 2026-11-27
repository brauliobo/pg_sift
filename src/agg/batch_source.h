/*
 * agg/batch_source.h - what a storage format is asked for and gives back to the operators of SiftAggV6: the columns of a scan (SiftV6Input), the batches of rows (SiftV6Batch) and the source
 * that opens them (SiftV6Source). A format (col/v1/, col/f2/, a test store) includes this file and nothing of agg/; the operators include it through agg/vexpr.h.
 */
#ifndef SIFT_AGG_BATCH_SOURCE_H
#define SIFT_AGG_BATCH_SOURCE_H

#include "postgres.h"

#include "access/attnum.h"
#include "nodes/execnodes.h"

#include "agg/vclass.h"
#include "agg/vec.h"
#include "agg/vexpr/vexpr.h"

#define SIFT_V6_MAX_COLS	SIFT_VEXPR_MAX_REGS
#define SIFT_V6_MAX_MASKS	SIFT_VEXPR_MAX_MASKS

/* A column the scan decodes for the operator: the attribute of the index and how it decodes */
typedef struct SiftV6InCol
{
	AttrNumber	attno;			/* of the index, 1-based, as SiftV6Col.attno */
	Oid			typid;
	SiftVecType type;
	int16		scale;
	uint8		prec;
	const List *path;			/* attno is a jsonb document's leaf (SIFT_V6_JLEAF_VATT + its leaf), the column the float8 of the member these keys lead to: the document's heap row gives it when the leaf cannot */
} SiftV6InCol;

/* A boolean the scan decodes as a bitmap: a boolean column, or the presence of a key in a jsonb column */
typedef struct SiftV6InMask
{
	AttrNumber	attno;
	char	   *key;			/* NULL: the boolean column itself */
} SiftV6InMask;

/* The columns and booleans of one scan, in the order the batches give them */
typedef struct SiftV6Input
{
	Oid			indexoid;		/* the table when is_table: its attributes are the columns, as the attributes of an index are */
	bool		is_table;
	int			nkeys;			/* the columns of the index: the attribute of the table each holds, 0 for an expression; the attributes of a table (keys is not used) */
	AttrNumber	keys[INDEX_MAX_KEYS];
	int			ncols;
	SiftV6InCol col[SIFT_V6_MAX_COLS];
	int			nmasks;
	SiftV6InMask mask[SIFT_V6_MAX_MASKS];
	EState	   *estate;			/* the execution the program runs in (the values of PARAM_EXEC): NULL while a path is made */
} SiftV6Input;

/* One batch of a scan: rows the visibility of the heap and the deletions of the index leave, the columns of the input as vectors (col[i] is input column i), the booleans as bitmaps */
typedef struct SiftV6Batch
{
	uint16		n;
	const SiftVec *col;
	const SiftVexprMaskIn *masks;
	const uint64 *mask;			/* the rows taken, NULL: all n */
	const uint64 *tag;			/* a number per row that names it to fetch (a row id of the source), NULL when the operator reads no other column */
} SiftV6Batch;

typedef struct SiftV6Source SiftV6Source;
typedef struct SiftV6Vexpr SiftV6Vexpr;

/* The granules (units) a source read, and those its zones and its summaries of key columns ruled out before it read them; the rows of the granules it read, and of those the rows it checked against the heap */
typedef struct SiftV6GranCounts
{
	uint64		read;
	uint64		pruned_zone;
	uint64		pruned_skip;
	uint64		rows;
	uint64		vetted;
} SiftV6GranCounts;

struct SiftV6Source
{
	/* the next batch, valid until the next call; false at the end. The rows come in the order of the parts when the operator asked for a sorted scan */
	bool		(*next) (SiftV6Source *s, SiftV6Batch *out);
	/* the rows of the partition of this key that have not been given are not wanted (NULL: the source cannot pass over them) */
	void		(*skip) (SiftV6Source *s, const int64 *part_key, uint8 part_nulls);
	/* the columns attnos[0 .. n-1] of the table for the row of the tag, as the Datums of their types */
	bool		(*fetch) (SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull);
	void		(*close) (SiftV6Source *s);
	/* adds the granules the source read so far to out (NULL: the format counts none) */
	void		(*granules) (SiftV6Source *s, SiftV6GranCounts *out);
};

/* The rows of the batch that pass quals (NULL: the batch's own), written to out as a bitmap of the batch rows; returns how many */
extern uint16 sift_v6_vexpr_filter(SiftVexprCtx *ctx, const SiftV6Vexpr *quals, const SiftV6Batch *b, uint64 *out);

/* A copy of the program in cx: the quals an operator hands to a source live as long as the source, not as long as the call */
extern SiftV6Vexpr *sift_v6_vexpr_dup(MemoryContext cx, const SiftV6Vexpr *quals);

/* A source calls this on a batch before sift_v6_vexpr_filter: the instants of the timestamps of the batch are the zone a program that reads one covers */
extern void sift_v6_vexpr_cover(const SiftV6Vexpr *quals, const SiftV6Input *in, const SiftV6Batch *b);

#endif							/* SIFT_AGG_BATCH_SOURCE_H */
