/*
 * agg/window.h - window functions and DISTINCT ON over input that is already in the order of the partition keys and then of the window order, in the
 * memory of one partition frontier: when PARTITION BY and ORDER BY are a prefix of the sort key of the parts (agg/stream_exec.h gives the rows of the
 * parts merged in that order), a partition ends where a key changes, so there is no sort and no tuplestore, and a function needs only its running state.
 *
 * Input: batches of up to SIFT_VEC_LEN rows, the key columns and the argument columns of the functions as vectors of int4, int8 or (arguments)
 * float8, with the rows of a mask taken and the others skipped. The key columns of a batch are compared with stream_run_ne_i64 (agg/kern6.h) in one
 * pass, so that the rows of a partition are not looked at one by one when only its first row is wanted; NULL keys are equal to each other (the
 * peers and the partitions of PostgreSQL). Output: one row per input row in input order, with the results of the functions, to a callback.
 *
 * Functions, with the frame PostgreSQL gives them: ROW_NUMBER, RANK, DENSE_RANK; LAG and LEAD of any offset up to SIFT_WIN_MAX_OFFSET with the default
 * of PostgreSQL (NULL, or the given value) when the row is not in the partition; FIRST_VALUE; and COUNT(*), COUNT, SUM, AVG, MIN, MAX from the start of
 * the partition to the current row, as ROWS or as RANGE (the peers of the row, the rows of the same order key, are in its frame: its value is the
 * value after the last of them, so the rows of a peer group are held until it ends). The memory is the state of the functions, a ring of the last
 * offset values of each LAG, and a ring of the rows not yet emitted: the rows a LEAD looks ahead to, and the peers of a RANGE frame. A peer group that
 * outgrows max_bytes goes to a temporary file and comes back in order when the group ends (no LEAD: the rows it looks ahead to are in the ring); a ring
 * of LEAD rows that would outgrow max_bytes is an error, 54000 (the planner does not choose this executor for an input that would).
 *
 * Results: counts and ranks are int8; SUM of an integer is a 128-bit integer (the caller makes it int8 or numeric), of a float8 a float8 summed in
 * the order of the rows with the checks of float8pl; AVG is a float8 (an integer sum over the count: the caller's numeric differs in its last digits); MIN
 * and MAX have the type of the argument and the order of float8 (NaN above everything).
 *
 * Top-K of a partition (WHERE rn <= K over ROW_NUMBER, RANK or DENSE_RANK): once a partition has K rows (or ranks) the rest of it is not wanted:
 * its rows are not emitted, they are not looked at (the next partition start is found in the key bitmaps), and the source is told (skip) so that it can
 * pass over the granules of the partition. Not with a LEAD or a RANGE function, which look at rows past the last wanted one. DISTINCT ON is the same
 * with K = 1 (SWIN_MODE_DISTINCT_FIRST: the first row of each partition), or the last row of each partition when the order within the partition is the
 * reverse of the order of the parts (SWIN_MODE_DISTINCT_LAST: one row held, no function).
 */
#ifndef SIFT_AGG_WINDOW_H
#define SIFT_AGG_WINDOW_H

#include "agg/kern6.h"

#define SIFT_WIN_MAX_KEYS		4
#define SIFT_WIN_MAX_FUNCS		8
#define SIFT_WIN_MAX_OFFSET		4096
#define SIFT_WIN_NO_LIMIT		UINT64_MAX

typedef enum SiftWinFuncKind
{
	SWIN_ROW_NUMBER,
	SWIN_RANK,
	SWIN_DENSE_RANK,
	SWIN_LAG,
	SWIN_LEAD,
	SWIN_FIRST_VALUE,
	SWIN_COUNT_STAR,
	SWIN_COUNT,
	SWIN_SUM,
	SWIN_AVG,
	SWIN_MIN,
	SWIN_MAX
} SiftWinFuncKind;

typedef enum SiftWinMode
{
	SWIN_MODE_WINDOW,
	SWIN_MODE_DISTINCT_FIRST,
	SWIN_MODE_DISTINCT_LAST
} SiftWinMode;

/*
 * A numeric of the window: coef * 10^-scale, the coefficient in 96 bits (the average of integers has at most 20 digits: 16 significant ones at least, as numeric_div
 * rounds, and no more than the 19 of an int8). The value is the one PostgreSQL's avg of an int4 or int8 gives, with the scale it gives.
 */
typedef struct SiftWinNum
{
	uint64		coef_lo;
	int32		coef_hi;
	int32		scale;
} SiftWinNum;

static inline SiftI128
sift_win_num_coef(const SiftWinNum *n)
{
	SiftI128	c = {n->coef_lo, n->coef_hi};

	return c;
}

/* A value: an int4 or int8 in i (sign-extended), a float8 in f, a sum of integers in w, the average of integers in num; the NULL is the caller's flag */
typedef union SiftWinVal
{
	int64		i;
	double		f;
	SiftI128	w;
	SiftWinNum	num;
} SiftWinVal;

typedef struct SiftWinFuncSpec
{
	SiftWinFuncKind kind;
	SiftVecType arg_type;		/* SVEC_I32, SVEC_I64 or SVEC_F64 of the argument column; unused by the ranks and COUNT(*) */
	bool		range_frame;	/* COUNT .. MAX: RANGE (the default with an ORDER BY) and not ROWS; needs n_ord > 0 */
	uint32		offset;			/* LAG, LEAD: the offset (1 where the SQL has none), 0 .. SIFT_WIN_MAX_OFFSET */
	bool		has_default;	/* LAG, LEAD: default_val is the value of a row outside the partition, NULL otherwise */
	SiftWinVal	default_val;
} SiftWinFuncSpec;

typedef struct SiftWinTopK
{
	bool		active;
	uint16		func;			/* the ROW_NUMBER, RANK or DENSE_RANK function */
	int64		k;				/* rows (ranks) <= k are wanted */
} SiftWinTopK;

typedef struct SiftWindowSpec
{
	SiftWinMode mode;
	uint8		n_part;
	uint8		n_ord;
	uint16		n_funcs;
	SiftWinFuncSpec funcs[SIFT_WIN_MAX_FUNCS];
	SiftWinTopK topk;
	uint64		offset;			/* rows skipped before the first one emitted */
	uint64		limit;			/* rows emitted, SIFT_WIN_NO_LIMIT for all: the source is not read past them */
} SiftWindowSpec;

typedef struct SiftWinBatch
{
	uint16		n;
	const SiftVec *part[SIFT_WIN_MAX_KEYS];
	const SiftVec *ord[SIFT_WIN_MAX_KEYS];
	const SiftVec *arg[SIFT_WIN_MAX_FUNCS];	/* the argument column of each function that has one, NULL for the others */
	const uint64 *tag;			/* a number per row, given back with it (the row of the input, a row id): NULL = none */
	const uint64 *mask;			/* the rows taken, NULL = all n */
} SiftWinBatch;

typedef struct SiftWinRow
{
	uint64		tag;
	uint64		part_idx;		/* the row's place in its partition, from 0 */
	int64		part[SIFT_WIN_MAX_KEYS];
	int64		ord[SIFT_WIN_MAX_KEYS];
	uint8		part_nulls;		/* bit k: key column k is NULL */
	uint8		ord_nulls;
	uint8		pad[6];
	uint16		arg_nulls;		/* bit f: the argument of function f is NULL */
	uint16		res_nulls;		/* bit f: the result of function f is NULL */
	SiftWinVal	arg[SIFT_WIN_MAX_FUNCS];
	SiftWinVal	res[SIFT_WIN_MAX_FUNCS];
} SiftWinRow;

/* Fills the next batch (valid until the next call); false at the end of the input */
typedef bool (*SiftWinNext) (void *src, SiftWinBatch *out);
/* The rows of the partition of this key (nulls: bit k, key column k is NULL) that have not been delivered are not wanted: the source may pass over them, a granule of that partition only */
typedef void (*SiftWinSkip) (void *src, const int64 *part_key, uint8 part_nulls);
typedef void (*SiftWinEmit) (void *arg, const SiftWinRow *row);

typedef struct SiftWinStats
{
	uint64		rows_in;		/* rows taken from the batches */
	uint64		rows_looked;	/* rows that a function was run on (not those of a skipped partition) */
	uint64		rows_out;
	uint64		skips;			/* partitions the source was told to skip */
	uint64		max_pending;	/* the most rows held at once */
} SiftWinStats;

typedef struct SiftWindowExec SiftWindowExec;

/* max_bytes bounds the ring of held rows (work_mem / 4 by the caller); an offset or a frame this cannot hold is an error here, at creation */
extern SiftWindowExec *sift_window_create(MemoryContext cxt, const SiftWindowSpec *spec, uint64 max_bytes, SiftWinNext next, SiftWinSkip skip, void *src,
										  SiftWinEmit emit, void *emit_arg);
/* Reads the input to its end, or until offset + limit rows are out */
extern void sift_window_run(SiftWindowExec *w);
extern const SiftWinStats *sift_window_stats(const SiftWindowExec *w);
extern void sift_window_free(SiftWindowExec *w);

/* The zone of a granule of the order key within one partition: the order of the rows of the partition is ascending, the NULLs last */
typedef struct SiftWinZone
{
	int64		min;
	int64		max;
	bool		has_null;
	bool		all_null;
} SiftWinZone;

/*
 * The first of n >= 1 granules, in the order of the rows, that can hold a row of the tail tie run: the rows that equal the last row of the partition in the
 * order key. The granules before it hold none, so the last row of each tie run is read from it on (DISTINCT ON in the reverse of the order of the parts).
 */
extern uint32 sift_win_tail_tie_first(const SiftWinZone *zones, uint32 n);

/* The type of the result of a function: int8 (ranks, counts), SVEC_NUM128 (a sum of integers in w, an average of integers in num), float8, or the type of the argument */
extern SiftVecType sift_win_result_type(const SiftWinFuncSpec *f);

#endif							/* SIFT_AGG_WINDOW_H */
