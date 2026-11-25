/*
 * agg/stream_exec.h - GROUP BY and Top-N over input that is already in the order of the keys, in the memory of one group: when the
 * keys of a GROUP BY (or of an ORDER BY .. LIMIT) are a prefix of the sort key of the parts, the batches of the parts come in key
 * order, equal keys are adjacent, and a group is finished, and emitted, as soon as the key changes: no hash table, no spill.
 *
 * Input: one source per part that overlaps the others in the key range (usually 1 to 4), each a function giving its next batch of up
 * to SIFT_VEC_LEN rows in key order. The sources are merged by their next row; with one source the merge costs nothing. A key is one or
 * two columns of int4 or int8 (SVEC_I32, SVEC_I64, dense vectors of the batch's rows) with NULLs last in the order of the parts, or
 * first when descending (the PostgreSQL defaults of ASC and DESC). Rows not in the batch mask are skipped.
 *
 * GROUP BY: a group is a SiftStreamGroup: count(*) in rows, the key and its null flags (bit i: key column i is NULL, its key 0), and for each of up to
 * SIFT_STREAM_MAX_VALS aggregate slots a SiftStreamVal: count(value) in n, the exact 128-bit sum, min and max of an integer column, the sum, min and max of a float8 column (min
 * and max in the order of float8, NaN above everything), its NULLs ignored (n 0: sum, min and max are NULL, whatever their fields hold). A slot has its own FILTER bitmap, and a
 * slot with no column counts the rows that pass its filter (count(*) FILTER (WHERE ..)). Top-N: the rows themselves, in order, with the first slot's column as the value. Either
 * stops reading the parts when offset + limit of them are out.
 */
#ifndef SIFT_AGG_STREAM_EXEC_H
#define SIFT_AGG_STREAM_EXEC_H

#include "agg/kern6/kern6.h"

#define SIFT_STREAM_MAX_KEYS	4
#define SIFT_STREAM_MAX_VALS	16
#define SIFT_STREAM_NO_LIMIT	UINT64_MAX

typedef struct SiftStreamBatch
{
	uint16		n;
	const SiftVec *keys[SIFT_STREAM_MAX_KEYS];
	uint8		nvals;			/* the aggregate slots */
	const SiftVec *val[SIFT_STREAM_MAX_VALS];	/* SVEC_I32, SVEC_I64 or SVEC_F64; NULL: the slot counts rows */
	const uint64 *filter[SIFT_STREAM_MAX_VALS];	/* the rows that pass the slot's FILTER, NULL: every row */
	const uint64 *mask;			/* the active rows, NULL: all n */
} SiftStreamBatch;

/* Fills the next batch (valid until the next call); false at the end of the part */
typedef bool (*SiftStreamNext) (void *src, SiftStreamBatch *out);

typedef struct SiftStreamVal
{
	uint64		n;
	SiftI128	sum;
	int64		min;
	int64		max;
	double		fsum;
	double		fmin;
	double		fmax;
} SiftStreamVal;

typedef struct SiftStreamGroup
{
	int64		key[SIFT_STREAM_MAX_KEYS];
	uint32		key_nulls;
	uint64		rows;
	SiftStreamVal v[SIFT_STREAM_MAX_VALS];
} SiftStreamGroup;

typedef struct SiftStreamRow
{
	int64		key[SIFT_STREAM_MAX_KEYS];
	uint32		key_nulls;
	int64		val;
	bool		val_null;
} SiftStreamRow;

typedef struct SiftPartCursor
{
	SiftStreamNext next;
	void	   *src;
	SiftStreamBatch batch;
	uint16		pos;
	bool		live;
} SiftPartCursor;

typedef struct SiftStreamExec
{
	SiftPartCursor *cursors;
	uint32		n_parts;
	uint8		nkeys;
	bool		descending;
	bool		rows_mode;		/* Top-N: every row is emitted, not grouped */
	bool		done;
	SiftStreamGroup cur_group;
	bool		has_cur_group;
	uint64		offset_count;
	uint64		limit_count;
	uint64		emitted;		/* the groups or rows out, those of the offset included */
	void		(*emit_group) (void *arg, const SiftStreamGroup *g);
	void		(*emit_row) (void *arg, const SiftStreamRow *r);
	void	   *arg;
} SiftStreamExec;

extern void sift_stream_init(SiftStreamExec *se, uint8 nkeys, bool descending, bool rows_mode, uint64 offset, uint64 limit,
							 void (*emit_group) (void *, const SiftStreamGroup *), void (*emit_row) (void *, const SiftStreamRow *),
							 void *arg);
extern void sift_stream_add_source(SiftStreamExec *se, SiftStreamNext next, void *src);
/* Reads the parts up to one batch of one of them, emitting the groups or rows that are final; false once everything is out (the groups of that last step are emitted too) */
extern bool sift_stream_step(SiftStreamExec *se);
/* Steps to the end of the parts or to the limit */
extern void sift_stream_run(SiftStreamExec *se);
extern void sift_stream_free(SiftStreamExec *se);

#endif							/* SIFT_AGG_STREAM_EXEC_H */
