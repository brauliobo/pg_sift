/*
 * agg/par/pinrun.h - a grouped aggregation of SiftAggV6 whose every group the compute threads of the backend (core/par/pool.h) deliver, run over the granules the scan hands over (core/par/pin.h):
 * the one driver that the time-bucket (par/tseries.h), star-join (par/star.h) and unnest (par/unnest.h) operators plug into.
 *
 * An operator says what a thread does with a granule (fill: pure C, a batch of key and argument words for agg/par/group.h) and what the leader does with each group the threads
 * deliver (fold: the server may be called). The groups the threads deliver go to a stream of each thread (core/par/radix.h) and reach fold on the leader, in the end; a granule that a thread leaves to
 * the leader (SIFT_ERR_SERIAL: a NULL it does not fold, a bin that overflows) goes through the scan and folds into the same groups as the serial path does, so that
 * the rows of the scan and the rows of the threads add up in any mix. When the streams of the threads fill, or the arenas do not fit work_mem, every granule goes through the scan.
 */
#ifndef SIFT_AGG_V6_PAR_PINRUN_H
#define SIFT_AGG_V6_PAR_PINRUN_H

#include "postgres.h"

#include "agg/gpu/scan.h"
#include "agg/par/group.h"
#include "agg/par/wordsink.h"
#include "agg/engine.h"
#include "agg/pipe.h"
#include "core/par/pin.h"

typedef struct SiftV6PinRun SiftV6PinRun;

/* A column of the batch of a scan: its leaf, and whether it is an integer without a scale that a thread decodes as the scan does */
typedef struct SiftV6PinCol
{
	int			leaf;
	bool		plain_int;
} SiftV6PinCol;

typedef struct SiftV6PinDesc
{
	const char *name;
	SiftParGroupSpec gs;
	const int  *leaf;			/* the leaves a thread decodes, the leader reads them with the columns of the predicates */
	int			nleaves;
	size_t		scratch_bytes;	/* the decode of one granule by one thread */
	/* The leader, before the threads run: what each of nthreads threads takes, made in cx (NULL: nothing) */
	void		(*prepare) (void *arg, MemoryContext cx, int nthreads);
	/* Thread `worker`: the batch of the rows of granule slot sl (memory of scratch); false with err filled: SIFT_ERR_SERIAL, SIFT_ERR_ARENA_FULL leave the granule to the scan */
	bool		(*fill) (void *arg, int worker, const SiftPinSlot *sl, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err);
	/* The leader: a group, or a partial aggregate of one, to add to the groups of the statement */
	void		(*fold) (void *arg, const SiftParGroupRow *row);
	void	   *arg;
	SiftV6Group *bulk;			/* the groups of the statement when fold adds each group to them once and no two groups have a key alike: they are kept as they come (sift_v6_group_bulk_begin); NULL: folded */
} SiftV6PinDesc;

/* The window of the statement's index with the run of d over it (d is copied): the scan offers its granules to sift_v6_pinrun_window */
extern SiftV6PinRun *sift_v6_pinrun_new(const SiftV6PinDesc *d, const SiftV6Spec *spec, SiftV6ScanStats *stats, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg);
extern SiftPinWin *sift_v6_pinrun_window(SiftV6PinRun *r);
/* The scan has read every part: the threads fold the granules handed over (or the scan does), and every group is in the groups of the statement. Before the scan ends */
extern void sift_v6_pinrun_end(SiftV6PinRun *r);

/*
 * What every fill starts with (pure C): the rows of the granule that its deletion vector and predicates leave, as a bitmap of SIFT_PIN_ACTIVE_WORDS words in *active, and the leaves leaf[i] decoded for
 * them into cv[i] (the codes of a dictionary for the leaf i with codes[i]; codes NULL: none). *nact == 0: no row is left and nothing is decoded.
 */
extern bool sift_v6_pin_slot_cols(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const int *leaf, int nleaves, const bool *codes, ColView *cv, uint64 **active, uint32 *nact);

/* The group of a run of the words of agg/par/wordsink.h, in the form the statement's groups take */
extern void sift_v6_pin_fold_words(SiftV6Group *g, const SiftV6Spec *spec, const SiftParWordShape *sh, const SiftParGroupRow *row);

/* The statements the operators of this directory take: begin is NULL for the others, or when the backend has no threads for them */
extern bool sift_v6_pin_wanted(const SiftV6Spec *spec);
extern SiftV6PinRun *sift_v6_pin_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg,
									   const SiftV6Pipe *pipe, const SiftV6PinCol *cols, int ncols);

/* The group of a run, in the key and accumulator form the statement's groups take: the counts and sums of the aggregates are in the sums of the row as sh says, or (NULL) in the order of the aggregates */
extern void sift_v6_pin_fold_row(SiftV6Group *g, const SiftV6Spec *spec, const SiftParGroupRow *row, const int64 *key, const SiftV6StrIn *str, uint64 key_nulls,
									 const SiftParWordShape *sh);

#endif							/* SIFT_AGG_V6_PAR_PINRUN_H */
