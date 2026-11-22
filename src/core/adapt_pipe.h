/*
 * core/adapt_pipe.h - the order of the conjuncts of a scan, learned while it runs. A scan whose filters are a skipping index, a compressed
 * chunk filter, a join filter, a compiled expression and a residual function does not know what each costs and keeps: the estimates of the planner
 * are wrong for a bind parameter, an expression or a skewed part. The pipeline measures every conjunct (the share of the rows that pass it, the
 * nanoseconds and the compressed bytes it takes for a row), and puts them in the order of Smith's rule, the cheapest way to reject a row first:
 *
 *   rank = (cpu_ns + w_io * io_bytes) / max(0.01, 1 - pass)
 *
 * A column that the previous conjunct has already loaded costs no read. The order changes only when it gains: a conjunct moves ahead of its
 * neighbour only when its rank is below SIFT_APIPE_HYSTERESIS of the other's, so that two near ranks do not trade places on noise. A conjunct that
 * can raise an error (a division, a checked cast, a function that is not leakproof) has a barrier_group above 0, and no conjunct of a lower group ever
 * moves behind it: a row that the cheap filter rejects is never divided. A conjunct that a caller disabled (a join filter that passes most rows) is
 * not in the order. A conjunct that has been last for a long time is never fed rows and so never measured: one conjunct in turn is run first for a
 * granule, every SIFT_APIPE_EXPLORE_GRAN granules, to tell a drift from a steady state.
 *
 * The pipeline has no allocation and no lock: a scan, or a participant of a parallel scan, owns one and adapts to the granules it claims.
 */
#ifndef SIFT_CORE_ADAPT_PIPE_H
#define SIFT_CORE_ADAPT_PIPE_H

#include "core/host.h"

#define SIFT_APIPE_MAX_OPS		64
#define SIFT_APIPE_WARMUP_GRAN	4		/* the first reorder, after 4 granules */
#define SIFT_APIPE_EPOCH_GRAN	16		/* the reorders after it */
#define SIFT_APIPE_EXPLORE_GRAN	64		/* a conjunct is run first, one at a time */
#define SIFT_APIPE_HYSTERESIS	0.85	/* a rank must be 15% below its neighbour's to pass it */
#define SIFT_APIPE_EWMA_ALPHA	0.25
#define SIFT_APIPE_DENSE_SURVIVAL 0.5	/* the share of the granules with a row left from which a scan reads whole chunks */
#define SIFT_APIPE_IO_WEIGHT_WARM 0.05
#define SIFT_APIPE_IO_WEIGHT_COLD 0.8

/* The stage of a conjunct: what it costs before it has been measured, and where it runs */
typedef enum SiftPredStage
{
	SPSTAGE_SKIP_INDEX = 0,		/* the metadata of a granule: it prunes the granule, before any row */
	SPSTAGE_COMPRESSED = 1,		/* a filter on the compressed chunk */
	SPSTAGE_SIP_VEC = 2,		/* the probe of a join filter on a batch */
	SPSTAGE_VEXPR = 3,			/* a compiled expression */
	SPSTAGE_FALLBACK = 4		/* the server's evaluation: a residual, a lookup */
} SiftPredStage;

typedef struct SiftAdaptOp
{
	uint16		op_idx;			/* the caller's index of the conjunct */
	int16		col;			/* the column it reads first, -1 for several */
	SiftPredStage stage;
	uint8		barrier_group;	/* 0 raises no error; the higher groups may, and run after the lower */
	bool		disabled;		/* out of the order until enabled again */
	bool		seen;

	double		ewma_pass;		/* in [0, 1] */
	double		ewma_cpu_ns;	/* for an input row */
	double		ewma_io_bytes;	/* for an input row */
	double		rank;

	uint64		rows_in;
	uint64		rows_out;
	uint64		early_exits;	/* the granules or batches it left with no row */
} SiftAdaptOp;

typedef struct SiftAdaptivePipeline
{
	uint16		n_ops;
	uint16		n_active;		/* the conjuncts in next[] */
	uint16		order[SIFT_APIPE_MAX_OPS];	/* the committed order: every conjunct */
	uint16		next[SIFT_APIPE_MAX_OPS];	/* the order of the next granule: the committed one without the disabled, and the explored one first */
	SiftAdaptOp ops[SIFT_APIPE_MAX_OPS];

	double		gran_survival;	/* the share of the granules opened that kept a row */
	uint32		granules_seen;
	uint32		next_explore_at;
	uint16		explore_cursor;
	uint32		reorders;
	bool		io_cold;		/* the reads miss the cache: a byte is worth more than a nanosecond */
} SiftAdaptivePipeline;

/* An empty pipeline: add the conjuncts with sift_apipe_add, then call sift_apipe_start */
extern void sift_apipe_init(SiftAdaptivePipeline * p);
/* Adds a conjunct (the index it has for the caller); returns its slot, the index in ops[] */
extern uint16 sift_apipe_add(SiftAdaptivePipeline * p, uint16 op_idx, int16 col, SiftPredStage stage, uint8 barrier_group);
/* Orders the conjuncts by group, then by the cost of the stage: the order before anything is measured */
extern void sift_apipe_start(SiftAdaptivePipeline * p);

/* What a conjunct did with a batch: the rows in and out, the nanoseconds and the compressed bytes it took. The ewma of a conjunct that took no row is left */
extern void sift_apipe_record(SiftAdaptivePipeline * p, uint16 slot, uint32 rows_in, uint32 rows_out, double cpu_ns, double io_bytes);
/* The end of a granule: whether a row was left. It makes the order of the next one (a reorder when due) */
extern void sift_apipe_granule_done(SiftAdaptivePipeline * p, bool survived);
/* The conjuncts to run on the next granule, in order: n of them */
extern const uint16 *sift_apipe_order(const SiftAdaptivePipeline * p, uint16 *n);
extern void sift_apipe_set_disabled(SiftAdaptivePipeline * p, uint16 slot, bool disabled);
/* The scan reads whole chunks (dense) rather than the rows the filters leave (selective) while most granules keep rows */
extern bool sift_apipe_dense(const SiftAdaptivePipeline * p);

#endif							/* SIFT_CORE_ADAPT_PIPE_H */
