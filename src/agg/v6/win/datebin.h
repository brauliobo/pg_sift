/*
 * agg/v6/win/datebin.h - date_bin() as a vector kernel, the exact quotient of a NUMERIC average, and the window executor SiftWinx that takes FILTER (WHERE ..) and the exact NUMERIC AVG.
 *
 * date_bin(stride, source, origin) of a timestamp or timestamptz is integer arithmetic on microseconds when the stride has no months (a day is 24 hours, as in timestamp_bin): the bin of
 * a source is origin + floor((source - origin) / stride) * stride. sift_datebin_* is that arithmetic with PostgreSQL's checks and the order of them (infinite source: itself; infinite origin, months in the
 * stride, a stride that overflows or is not positive, an overflow of the difference or the result: the errors of timestamp_bin), over the active rows of a batch; a stride with months or years is an error in
 * PostgreSQL too, so no calendar or DST rule enters (a timestamptz is binned in UTC microseconds, as timestamptz_bin does). The strides of every dashboard (1 s, 1, 5, 10, 15, 30 min, 1, 6, 12 h, 1, 7 d) run as
 * divisions by a constant. The kernel reports the first row that raises and why; the caller raises it only for a row that survives its predicates, and raises what PostgreSQL does.
 *
 * sift_numavg_exact is numeric_avg of a sum and a count: the quotient of numeric_div with the display scale select_div_scale gives it and the rounding half away from zero, in 128-bit integers; no float8
 * enters, so a NUMERIC average has no drift (EXT-24). The result is sign, magnitude and scale: it is not ok (and PostgreSQL's own numeric_div is to be used) when the quotient does not fit 128 bits.
 *
 * SiftWinx is the window executor for the aggregates the executor of agg/window.h does not take: COUNT, SUM, AVG, MIN and MAX with a FILTER (WHERE ..) clause, over int2, int4, int8, float8 and numeric
 * columns (a numeric is the scaled integer of agg/vec.h), from the start of the partition to the current row as ROWS or as RANGE (the rows of the same ORDER BY value are in the frame: their value
 * is the one after the last of them, so a peer group is held, by its row ids only, until it ends). ROW_NUMBER, RANK and DENSE_RANK are there so that one window clause is one executor. LAG, LEAD and
 * FIRST_VALUE of any of those column types (a date or a timestamp is an integer here) take no frame: LAG keeps a ring of the last offset values of its argument, LEAD holds the rows that wait for the
 * rows it looks ahead to (a ring of the most offset + 1 of them, bounded by max_pending), FIRST_VALUE the first row of the partition. VAR_POP, VAR_SAMP, STDDEV_POP and STDDEV_SAMP are the exact
 * state of v6_winx_var.h folded over the frame as the other aggregates are. The input
 * is in the order of the partition keys and then of the window order (as for agg/window.h); a FILTER is a bitmap of the rows that pass it, the form agg/v6/vexpr.h makes. An aggregate ignores a row
 * that does not pass its filter or whose argument is NULL, and has no value (NULL) while it has seen none, except the counts, which are 0.
 */
#ifndef SIFT_AGG_V6_DATEBIN_WIN_H
#define SIFT_AGG_V6_DATEBIN_WIN_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/v6/batch.h"
#include "agg/v6/win/x_var.h"

/* ---- date_bin ---- */

typedef enum SiftDateBinStatus
{
	SDB_OK = 0,
	SDB_ORIGIN_RANGE,			/* origin out of range (infinite) */
	SDB_STRIDE_MONTHS,			/* timestamps cannot be binned into intervals containing months or years */
	SDB_INTERVAL_RANGE,			/* interval out of range: the stride's microseconds, or the difference of the source and the origin, overflow */
	SDB_STRIDE_NOT_POSITIVE,	/* stride must be greater than zero */
	SDB_TS_RANGE				/* timestamp out of range */
} SiftDateBinStatus;

typedef struct SiftDateBin
{
	int64		stride_us;
	int64		origin;
	SiftDateBinStatus stride_status;	/* what the stride and the origin raise for any finite source */
} SiftDateBin;

extern void sift_datebin_init(SiftDateBin *b, const SiftInterval16 *stride, int64 origin);
extern SiftDateBinStatus sift_datebin_one(const SiftDateBin *b, int64 ts, int64 *out);
/* The active rows of the batch whose input is not NULL into out[row]; on an error *err_row is the first row that raised it and the rest of the batch is not computed */
extern SiftDateBinStatus sift_datebin_batch(const SiftDateBin *b, const SiftBatchSel *bs, const SiftVec *in, int64 *out, uint32 *err_row);
extern const char *sift_datebin_message(SiftDateBinStatus s);
/* date_trunc('month', source) of a timestamp (no zone: the calendar is the proleptic Gregorian one of PostgreSQL) for the active rows of the batch whose input is not NULL; an infinite source is itself.
 * A SiftV6Bin whose stride_us is SIFT_DATEBIN_MONTH is binned by it. */
#define SIFT_DATEBIN_MONTH	(-1)
extern void sift_datemonth_batch(const SiftBatchSel *bs, const SiftVec *in, int64 *out);

/* ---- the average of a numeric ---- */

typedef struct SiftNumAvg
{
	bool		ok;
	bool		neg;
	SiftWinU128 mag;		/* the value is (neg ? -1 : 1) * mag * 10^-scale */
	int32		scale;
} SiftNumAvg;

/* sum: the scaled integer of the numeric values (sum_scale digits after the point); count > 0 */
extern bool sift_numavg_exact(SiftI128 sum, uint64 count, int sum_scale, SiftNumAvg *out);
/* The decimal text of the value ("-12.3400", "0.50", "7"): what numeric_in reads back */
extern int sift_numavg_text(const SiftNumAvg *a, char *buf, size_t cap);

/* ---- SiftWinx ---- */

#define SIFT_WINX_MAX_KEYS		4
#define SIFT_WINX_MAX_FUNCS		16
#define SIFT_WINX_MAX_FILTERS	64

typedef enum SiftWinxKind
{
	SWX_ROW_NUMBER,
	SWX_RANK,
	SWX_DENSE_RANK,
	SWX_COUNT_STAR,
	SWX_COUNT,
	SWX_SUM,
	SWX_AVG,
	SWX_MIN,
	SWX_MAX,
	SWX_LAG,
	SWX_LEAD,
	SWX_FIRST_VALUE,
	SWX_VAR_POP,
	SWX_VAR_SAMP,
	SWX_STDDEV_POP,
	SWX_STDDEV_SAMP
} SiftWinxKind;

static inline bool
sift_winx_is_var(SiftWinxKind k)
{
	return k >= SWX_VAR_POP;
}

static inline SiftWinVarKind
sift_winx_var_kind(SiftWinxKind k)
{
	return (SiftWinVarKind) (k - SWX_VAR_POP);
}

typedef struct SiftWinxVal SiftWinxVal;

/* A value of the argument as the result holds it: an integer, a date, a timestamp or a numeric of at most 18 digits in i, a float8 in f, a numeric of more digits in w */
struct SiftWinxVal
{
	bool		null;
	bool		nan;			/* the numeric NaN, which is not NULL: the other members hold nothing */
	uint64		n;				/* COUNT*; the rows the other aggregates folded */
	int64		i;				/* ROW_NUMBER, RANK, DENSE_RANK; min, max, LAG, LEAD and FIRST_VALUE of an integer or of a numeric that fits 64 bits */
	double		f;				/* sum, avg, min, max, LAG, LEAD, FIRST_VALUE, the variances of a float8 */
	SiftI128	w;				/* sum of an integer or a numeric; min, max, LAG, LEAD and FIRST_VALUE of a numeric of 38 digits */
	SiftNumAvg	avg;			/* avg of an integer or a numeric */
	SiftWinVarExact var;		/* the variances of an integer or a numeric: the quotient the caller divides */
};

typedef struct SiftWinxFunc
{
	SiftWinxKind kind;
	SiftVecType arg_type;		/* SVEC_I32, SVEC_I64, SVEC_F64, SVEC_NUM64, SVEC_NUM128 */
	int			scale;			/* of a numeric argument */
	int			filter;			/* the filter 0 .. SIFT_WINX_MAX_FILTERS - 1, or -1 */
	uint32		offset;			/* LAG, LEAD: 0 .. SIFT_WINX_MAX_OFFSET (1 where the SQL has none) */
	bool		has_default;	/* LAG, LEAD: def_i or def_f is the value of a row outside the partition, NULL otherwise */
	int64		def_i;			/* LAG, LEAD: the default of an integer, a date, a timestamp or a numeric of at most 18 digits; the 16-aligned SiftWinxVal does not live in what palloc gives */
	double		def_f;			/* the default of a float8 */
} SiftWinxFunc;

#define SIFT_WINX_MAX_OFFSET	4096

typedef struct SiftWinxSpec
{
	int			n_part;
	int			n_ord;
	int			nfuncs;
	bool		range_frame;	/* the default frame with an ORDER BY: the peers of the row are in it; with no ORDER BY every row is a peer of every other */
	SiftWinxFunc func[SIFT_WINX_MAX_FUNCS];
	uint64		max_pending;	/* the most rows of a peer group held in memory: the rest wait in a temp file, and LEAD's look-ahead is bounded by it */
} SiftWinxSpec;

typedef struct SiftWinxBatch
{
	uint32		n;
	const SiftVec *part[SIFT_WINX_MAX_KEYS];	/* SVEC_I32 or SVEC_I64 */
	const SiftVec *ord[SIFT_WINX_MAX_KEYS];
	const SiftVec *arg[SIFT_WINX_MAX_FUNCS];	/* of each function that has an argument */
	const uint64 *filter[SIFT_WINX_MAX_FILTERS];	/* of each filter that a function names: SIFT_VEC_WORDS words */
	const uint64 *tag;			/* a number for each row, given back with its results */
	const uint64 *mask;			/* the rows taken, NULL = all n */
} SiftWinxBatch;

typedef bool (*SiftWinxNext) (void *src, SiftWinxBatch *out);
typedef void (*SiftWinxEmit) (void *arg, uint64 tag, const SiftWinxVal *res);

typedef struct SiftWinxStats
{
	uint64		rows;
	uint64		max_pending;
	uint64		partitions;
} SiftWinxStats;

typedef struct SiftWinx SiftWinx;

/* The bytes the rows that wait for a LEAD take (0 without one): the planner leaves the window to PostgreSQL when they exceed max_pending words */
extern uint64 sift_winx_lead_bytes(const SiftWinxSpec *spec);
extern SiftWinx *sift_winx_create(MemoryContext cx, const SiftWinxSpec *spec, SiftWinxNext next, void *src, SiftWinxEmit emit, void *emit_arg);
extern void sift_winx_run(SiftWinx *w);
extern const SiftWinxStats *sift_winx_stats(const SiftWinx *w);
extern void sift_winx_free(SiftWinx *w);

#endif							/* SIFT_AGG_V6_DATEBIN_WIN_H */
