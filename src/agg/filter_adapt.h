/*
 * agg/filter_adapt.h - the order in which a scan tests the conjuncts of its filter, learned while it runs. The planner knows the share of rows one predicate passes and little of two that
 * are correlated, of a pattern, or of an IN list; a scan that runs first the conjunct that rules out the most rows for the least work reads fewer chunks and tests fewer rows.
 *
 * Each conjunct keeps, smoothed over the granules (an EWMA), the share of its input rows it passed, the nanoseconds it took for each of them, and the bytes of its column that a row costs to read.
 * Its rank is (cpu + w_io * io) / max(0.01, 1 - pass), with w_io for a warm or a cold read, and the order is that of ascending rank. Before a granule has taught anything the ranks come from what the
 * part says of its columns (col/f2/col_stats.h), so the order is a good one from granule 0.
 *
 *   - a conjunct changes places with its neighbour only when it is at least 15% better (hysteresis), and only at the 4th granule and every 16th after it;
 *   - a conjunct on a column that an earlier one reads is charged no read, so conjuncts of one column cluster;
 *   - a conjunct that can raise an error (barrier_group >= 1: a division, a cast, a pattern, a function that is not leakproof) never changes place with any other: the free conjuncts keep to the
 *     side of it they were on, so that the errors a query raises are those of the order it was written in;
 *   - every 64th granule the conjunct furthest from the front of its group goes first, to learn its share of all the rows again (a share only seen behind others drifts);
 *   - the state is a fixed struct of at most 8 KB, private to the scan or to the worker, with no lock.
 */
#ifndef SIFT_FILTER_ADAPT_H
#define SIFT_FILTER_ADAPT_H

#include "core/host.h"

#define SIFT_AFILT_MAX			64
#define SIFT_AFILT_WARMUP		4		/* granules before the first change of order */
#define SIFT_AFILT_EPOCH		16		/* granules between the later ones */
#define SIFT_AFILT_EXPLORE		64
#define SIFT_AFILT_SAMPLE		4		/* the granules between two in which the time of a conjunct is read */
#define SIFT_AFILT_HYSTERESIS	0.85
#define SIFT_AFILT_ALPHA		0.25
#define SIFT_AFILT_W_IO_WARM	0.05	/* ns for a byte read from shared buffers */
#define SIFT_AFILT_W_IO_COLD	0.8		/* ns for a byte read from disk */
#define SIFT_AFILT_WINDOW		32		/* the granules the share that keep a row is counted over */

typedef enum SiftCostClass
{
	SIFT_CC_CONST,				/* key present, all null, constant */
	SIFT_CC_BOOL_BITS,
	SIFT_CC_RLE,
	SIFT_CC_DICT_BITSET,		/* a dictionary code tested in a set of the codes that pass; a frame of reference of bits */
	SIFT_CC_DELTA,
	SIFT_CC_NUM64,				/* a decimal float and a number held as 64 bits */
	SIFT_CC_FSST,
	SIFT_CC_DIM_SEMI,
	SIFT_CC_STR_BLOCK			/* a string tested by its bytes, a regular expression */
} SiftCostClass;

/* Nanoseconds a row costs a conjunct of the class before anything is measured */
extern double sift_cost_class_ns(SiftCostClass c);

typedef struct SiftConjunctInit
{
	int			col;			/* the column it reads: conjuncts of one column share the read */
	uint8		barrier_group;	/* 0: it cannot raise an error; 1 and up: it can, and keeps its place */
	double		pass_prior;		/* the share of rows expected to pass */
	double		cpu_ns_row;
	double		io_bytes_row;	/* bytes of the column's chunk for each row of a granule */
} SiftConjunctInit;

typedef struct SiftConjunctState
{
	int			col;
	uint8		barrier_group;
	bool		seen;
	bool		seen_cpu;
	double		pass;			/* smoothed share of input rows passed */
	double		cpu;			/* smoothed ns per input row */
	double		io;				/* smoothed bytes of the chunk per row of the granule */
	uint64		total_rows_in;
	uint64		total_rows_out;
	uint64		total_skipped;	/* granules in which it was not reached: an earlier conjunct left no row */
} SiftConjunctState;

typedef struct SiftAdaptiveFilter
{
	uint16		n;
	bool		io_cold;
	uint16		order[SIFT_AFILT_MAX];	/* the order the conjuncts are in */
	uint16		cur[SIFT_AFILT_MAX];	/* the order of the granule being scanned: order, with the conjunct being explored first in its group */
	uint16		seg_start[SIFT_AFILT_MAX];	/* of each position the first and the end of the group of conjuncts that may trade places with it */
	uint16		seg_end[SIFT_AFILT_MAX];
	uint16		explore_cursor;
	int			explore_slot;	/* -1: none */
	uint32		granules_seen;
	uint32		n_reorders;
	uint32		window;			/* a bit for each of the last SIFT_AFILT_WINDOW granules: 1 where a row survived every conjunct */
	SiftConjunctState st[SIFT_AFILT_MAX];
} SiftAdaptiveFilter;

StaticAssertDecl(sizeof(SiftAdaptiveFilter) <= 8192, "SiftAdaptiveFilter must stay within 8 KB");

/* n conjuncts (at most SIFT_AFILT_MAX) in the order of the query; they are ranked by their priors, within the groups the barriers leave */
extern void sift_afilt_init(SiftAdaptiveFilter *af, const SiftConjunctInit *init, int n, bool io_cold);

/* The order to test the conjuncts of the next granule in */
extern const uint16 *sift_afilt_order(const SiftAdaptiveFilter *af);

/* Whether the time of the conjuncts of the granule about to be scanned should be read: the first granules and then every SIFT_AFILT_SAMPLE-th, so that the clock costs under 0.1% */
extern bool sift_afilt_sample_cpu(const SiftAdaptiveFilter *af);

/* What a conjunct did in a granule of granule_rows rows: rows_in rows given, rows_out passed, cpu_ns spent (ignored unless the granule was sampled), io_bytes of its column read (0: already read) */
extern void sift_afilt_record(SiftAdaptiveFilter *af, int slot, uint32 rows_in, uint32 rows_out, double cpu_ns, uint32 io_bytes, uint32 granule_rows);

/* A conjunct that no row reached in a granule (an earlier one passed none) */
extern void sift_afilt_skipped(SiftAdaptiveFilter *af, int slot);

/* The granule is done, survived: a row passed every conjunct. The order changes here when it is time. */
extern void sift_afilt_granule_done(SiftAdaptiveFilter *af, bool survived);

/* Whether the next granule is better read whole, one request for its record: half of the last SIFT_AFILT_WINDOW granules kept a row (before 32 granules have been seen the window starts at half) */
extern bool sift_afilt_dense(const SiftAdaptiveFilter *af);

/* The rank of the conjunct in position pos of the order: what it costs for each row it removes, with the read charged when no conjunct before it reads its column */
extern double sift_afilt_rank(const SiftAdaptiveFilter *af, const uint16 *order, int pos);

#endif							/* SIFT_FILTER_ADAPT_H */
