/*
 * agg/v6/v6_par.h - the parallel SiftAggV6: two strategies over one scan and one transport, chosen by the planner from the estimated number of groups (no GUC: the constants below are those
 * of the cost model of agg/cost.h and of PostgreSQL's own settings).
 *
 *   SV6P_TWO_STAGE            few groups: every participant aggregates its morsels into a table of its own; the first to finish keeps its table and the others hand it their groups as records; after one
 *                             barrier it merges them and emits the result. Workers return no tuple.
 *   SV6P_RADIX_GROUPED        many groups: a participant aggregates into a small table (SIFT_PAR_LOCAL_GROUPS) that stays in its cache and, when it is full, scatters the groups as records into
 *                             SIFT_PAR_RADIX_PARTS partitions by the hash of the key; after one barrier every participant claims whole partitions, merges and finishes them, and emits their groups itself, through
 *                             the Gather above it: no group is merged by one process alone, none is emitted twice.
 *   SV6P_RADIX_UNGROUPED_DIST count(DISTINCT) of no keys: the distinct values are scattered by the hash of the value, each partition counts its values exactly and adds the count to a shared word; one
 *                             participant emits the row.
 *
 * Both strategies carry the groups as the records of the engine (a header of SiftV6RecHdr and a payload the engine of the node reads: group.c for the node, a table of the test for the tests) through
 * the chunks and the spill files of agg/par/radix.h, so a partial state that outgrows its share of memory leaves for a file and nothing fails. A two-stage run whose groups turn out to be many (a
 * participant's table passes SIFT_PAR_TWO_STAGE_MAX_GROUPS, or the chunks run out) sets a flag: after the barrier every participant hands its table over and the run goes on as the radix strategy.
 *
 * The shared state is one block, the DSM chunk of the node, with offsets and no pointer (SiftV6ParDsmHdr); the barrier has three phases: 0 scan, 1 merge, 2 (the distinct strategy only) every partition counted.
 */
#ifndef SIFT_AGG_V6_PAR_H
#define SIFT_AGG_V6_PAR_H

#include "core/host.h"

#include "storage/barrier.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "utils/wait_event.h"

#include "core/par/morsel.h"
#include "agg/par/radix.h"
#include "agg/v6/v6_acc.h"

#define SIFT_V6_PAR_DSM_MAGIC		0x53365041U
#define SIFT_PAR_TWO_STAGE_MAX_GROUPS	65536U		/* SIFT_DIRECT_MAX of agg/cost.h */
#define SIFT_PAR_TWO_STAGE_MERGE_RATIO	0.05		/* est_groups * workers at most this share of the rows: the merge is not the scan's tail */
#define SIFT_PAR_MIN_ROWS			32768.0			/* under four granules the launch of a worker costs more than the scan */
#define SIFT_PAR_MIN_GRANULES_PER_W	2U
#define SIFT_PAR_RADIX_PARTS		64U
#define SIFT_PAR_DIST_PARTS		16U				/* the distinct values of a count(DISTINCT) of no keys: each participant holds a chunk of every partition, so fewer partitions keep them inside the memory the records may take */
#define SIFT_PAR_RADIX_PARTS_WIDE	128U			/* from SIFT_PAR_WIDE_WORKERS workers */
#define SIFT_PAR_WIDE_WORKERS		16
#define SIFT_PAR_RING_MAX_BYTES		((Size) 16 << 20)	/* the chunks of the two-stage strategy */
#define SIFT_PAR_LOCAL_GROUPS		65536U			/* the table of a participant of the radix strategy: it folds a group of up to this many keys before it is scattered */
#define SIFT_PAR_MAX_DIST			32
#define SIFT_PAR_DIST_FLAT_SHARE	0.3			/* the share of the rows whose values a participant of a query of no keys hands over: it dedups them first */

typedef enum SiftV6ParStrategy
{
	SV6P_SERIAL = 0,
	SV6P_TWO_STAGE = 1,
	SV6P_RADIX_GROUPED = 2,
	SV6P_RADIX_UNGROUPED_DIST = 3
} SiftV6ParStrategy;

/* ---- the planner's decision: numbers in, a strategy out (agg/v6/v6_par_plan.c) ---- */

typedef struct SiftV6ParInput
{
	bool		parallel_scan;		/* sift.parallel_scan */
	bool		parallel_ok;		/* parallelModeOK && consider_parallel */
	bool		has_rls;			/* row security or a security barrier applies */
	bool		parallel_hazard;	/* a target, qual, having or group expression is not parallel safe, or an aggregate keeps a state that is not a V6Acc to combine (stddev, variance, bool_or, bool_and, min and max of text) */
	bool		exact_float;		/* sift.agg_exact_float */
	bool		float_sum_avg;		/* a sum or avg of a float */
	bool		has_distinct;		/* a count(DISTINCT) */
	int			max_workers;		/* max_parallel_workers_per_gather */
	int			nkeys;
	int			nsets;
	int			naggs;
	double		est_rows;			/* rows the scan accepts */
	double		scan_rows;			/* rows of the granules it reads: the zone maps and the primary index leave them, and a text qual that is not a key prunes none */
	double		g_eff;				/* the granules that are */
	double		est_groups;			/* output groups of all the sets of keys, >= 1 */
	double		state_bytes;		/* of one group: its key and accumulators */
	double		hash_limit;			/* get_hash_memory_limit() */
	/* the workers the cost model of agg/cost.h picks (0: the serial plan is cheaper) given the most the granules allow */
	int			(*cost_workers) (void *arg, int max_by_granules);
	void	   *cost_arg;
} SiftV6ParInput;

extern SiftV6ParStrategy sift_v6_par_choose_strategy(const SiftV6ParInput *in, int *nworkers);

/* ---- the records ---- */

#define V6REC_WIDE		0x0001		/* the key is the bytes after the accumulators */
#define V6REC_PLAIN		0x0002		/* the state of a set of no keys */
#define V6REC_DIST		0x0004		/* a distinct value of a group (or of the query): the payload is the engine's */

/*
 * The header of every record, whatever the engine puts after it: the transport reads len (a multiple of 8) and hash64 (at offset 8, where agg/par/radix.h partitions by it); set_idx is the grouping set,
 * key the inline key of the group or the reference of a wide one.
 */
typedef struct SiftV6RecHdr
{
	uint32		len;
	uint16		set_idx;
	uint16		flags;
	uint64		hash64;
	SiftInlineKey16 key;
} SiftV6RecHdr;

StaticAssertDecl(offsetof(SiftV6RecHdr, hash64) == 8 && sizeof(SiftV6RecHdr) == 32, "the hash is where the partitions read it");

#define V6REC_DIST_INT		0x0008		/* a value of an integer COUNT(DISTINCT), see SiftV6DistRec */
#define V6REC_DIST_SLOT(f)	((uint32) (f) >> 8)

/* A value of an integer COUNT(DISTINCT) (v6_par_dist.h): the slot of its aggregate is in the bits of flags above V6REC_DIST_INT, the plen bytes of its group (its nulls and its key) follow it */
typedef struct SiftV6DistRec
{
	uint32		len;
	uint16		plen;
	uint16		flags;
	uint64		hash64;
	int64		value;
} SiftV6DistRec;

StaticAssertDecl(offsetof(SiftV6DistRec, hash64) == offsetof(SiftV6RecHdr, hash64) && offsetof(SiftV6DistRec, flags) == offsetof(SiftV6RecHdr, flags), "a record of a value has the head of every record");

typedef void (*SiftV6RecPut) (void *arg, const uint8 *rec, uint32 len);

/* ---- the engine: what a participant folds rows into and what finishes a group; group.c for the node ---- */

typedef struct SiftV6ParWorkerStats
{
	uint64		morsels_stolen;
	uint64		parts_scanned;
	uint64		grans_scanned;
	uint64		grans_preagg;
	uint64		grans_preagg_delta;
	uint64		grans_pruned_dict;
	uint64		cfilt_grans_total;
	uint64		cfilt_grans_hit;
	uint64		rows_scanned;
	uint64		heap_vet_rows;
	uint64		heap_rows_dead;
	uint64		groups_emitted;
	uint64		radix_parts_claimed;
	uint64		radix_parts_spilled;
	uint64		spill_bytes;
	uint64		peak_mem_bytes;
} SiftV6ParWorkerStats;

/* the counters of the participants are an array in the shared block, each a whole number of cache lines; a run is palloc'd, so the type has no alignment of its own */
StaticAssertDecl(sizeof(SiftV6ParWorkerStats) % PG_CACHE_LINE_SIZE == 0, "a participant's counters do not share a cache line with another's");

typedef struct SiftV6TopKRun SiftV6TopKRun;

typedef struct SiftV6ParEngine
{
	void	   *env;
	/*
	 * A state the rows or the records fold into. whole: it is the state of the whole answer (a serial run, the table the two-stage run keeps, the row of the distinct strategy), so a set of no
	 * keys emits its row though nothing was folded into it; a state of one partition emits the row of that set only if the records of the set were merged into it. The distinct values of a
	 * state are counted when it is finished; a participant's state is exported before that and the values travel as V6REC_DIST records.
	 */
	void	   *(*local_new) (void *env, bool whole);
	void		(*local_free) (void *local);
	/* Folds the rows of the morsel (the part is an index into the parts of the shared block) */
	void		(*scan_morsel) (void *local, const SiftMorsel *m, SiftV6ParWorkerStats *stats);
	/* The groups and distinct values it holds: the participant empties it into the partitions when this passes the cap */
	uint64		(*local_load) (const void *local);
	/* Every group as records (the state of each, and the pending distinct values); the state is empty afterwards. A set of no keys gives its record even when it holds no row */
	void		(*local_export) (void *local, SiftV6RecPut put, void *arg);
	void		(*local_merge) (void *local, const uint8 *rec);
	/* The state is final: local_next gives the groups, once each */
	void		(*local_finish) (void *local);
	bool		(*local_next) (void *local, Datum *values, bool *isnull);
	/* The final state's groups offered to the heap of a ORDER BY .. LIMIT, which makes the Datums of the keys of those that stay only */
	void		(*local_offer) (void *local, SiftV6TopKRun *topk);
	/* The distinct strategy: the number of distinct values of aggregate slot the partitions merged into local, and the total that the final state is given */
	uint64		(*local_dist_count) (const void *local, uint32 slot);
	void		(*local_set_dist) (void *local, uint32 slot, uint64 total);
	/* The state of a partition emptied for the records of the next one (its tables and buffers are kept): a participant counts one partition after another in one state */
	void		(*local_reset) (void *local);
} SiftV6ParEngine;

/* ---- the shared block ---- */

/* The counters of the two-stage strategy's merge that EXPLAIN prints: the records that went through the chunks and their bytes */
typedef struct SiftParPartialRing
{
	pg_atomic_uint64 total_recs;
	pg_atomic_uint64 total_bytes;
	uint32		max_chunks;
	uint32		pad;
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftParPartialRing;

typedef struct SiftV6ParDsmHdr
{
	uint32		magic;
	uint16		strategy;
	uint16		nworkers_planned;
	uint32		n_parts;			/* the parts (SiftParPartRef) the morsels are over */
	uint32		n_staged_dead;
	uint32		n_aggs;
	uint32		n_dist;
	uint32		radix_nparts;
	uint32		radix_max_chunks;
	uint32		rescan_gen;
	dsm_handle	seg_handle;			/* the workers find their segment by it: InitializeWorkerCustomScan has the table of contents, not the segment */
	uint32		plain_slot_bytes;	/* of the record each participant leaves for the final state (the distinct strategy) */
	uint32		n_children;			/* the relations the parts are of (the partitions of a table, else one) */

	/* byte offsets from this header, each on a cache line */
	Size		parts_off;
	Size		staged_dead_off;
	Size		morsel_off;
	Size		coord_off;
	Size		plain_off;
	Size		stats_off;
	Size		total_bytes;

	Barrier		phase_barrier pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 next_pid pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 keeper pg_attribute_aligned(PG_CACHE_LINE_SIZE);		/* the participant that keeps its table (two-stage) */
	pg_atomic_uint32 final_emitter pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* the one that emits the single row (distinct) */
	pg_atomic_uint32 escalated pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* the two-stage run turned into a radix one */
	SiftParPartialRing ring;
	pg_atomic_uint64 dist_total[SIFT_PAR_MAX_DIST] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftV6ParDsmHdr;

/* What the block holds, for its size and its offsets */
typedef struct SiftV6ParShape
{
	SiftV6ParStrategy strategy;
	int			nworkers;
	uint32		n_parts;
	Size		part_ref_bytes;		/* sizeof(SiftParPartRef) */
	uint32		n_children;
	uint32		n_staged_dead;
	uint32		n_aggs;
	uint32		n_dist;
	uint32		granules_total;
	uint32		max_chunks;
	uint32		radix_nparts;
	uint32		plain_slot_bytes;
} SiftV6ParShape;

extern uint32 sift_v6_par_chunks(SiftV6ParStrategy strategy, double expected_bytes);
extern uint32 sift_v6_par_radix_parts(SiftV6ParStrategy strategy, int nworkers);
extern Size sift_v6_par_dsm_size(const SiftV6ParShape *sh);
/* Lays the block out in raw (sift_v6_par_dsm_size bytes) and initializes every shared word; granules[p] is the granules of part p */
extern SiftV6ParDsmHdr *sift_v6_par_dsm_init(void *raw, dsm_segment *seg, const SiftV6ParShape *sh, const uint32 *granules);
extern SiftV6ParDsmHdr *sift_v6_par_dsm_block(void *chunk);
extern void sift_v6_par_dsm_reinit(SiftV6ParDsmHdr *hdr, const uint32 *granules);

static inline SiftParCoord *
sift_v6_par_coord(SiftV6ParDsmHdr *h)
{
	return (SiftParCoord *) ((char *) h + h->coord_off);
}

/* The deletes the leader's transaction made in relation child that are not written yet: the block holds n_children + 1 offsets (in addresses) of the lists, then the lists */
static inline uint64 *
sift_v6_par_staged_starts(const SiftV6ParDsmHdr *h)
{
	return (uint64 *) ((char *) h + h->staged_dead_off);
}

static inline const uint64 *
sift_v6_par_staged_dead(const SiftV6ParDsmHdr *h, uint32 child, uint32 *n)
{
	const uint64 *start = sift_v6_par_staged_starts(h);

	*n = (uint32) (start[child + 1] - start[child]);
	return start + h->n_children + 1 + start[child];
}

static inline SiftMorselQueue *
sift_v6_par_morsels(SiftV6ParDsmHdr *h)
{
	return (SiftMorselQueue *) ((char *) h + h->morsel_off);
}

static inline SiftV6ParWorkerStats *
sift_v6_par_stats(SiftV6ParDsmHdr *h, int pid)
{
	return (SiftV6ParWorkerStats *) ((char *) h + h->stats_off) + pid;
}

/* ---- the run of one participant ---- */

typedef enum SiftV6ParStage
{
	SV6S_NEW,
	SV6S_LOCAL,					/* emitting the table this participant kept */
	SV6S_PARTS,					/* claiming and emitting partitions */
	SV6S_DONE
} SiftV6ParStage;

typedef struct SiftV6ParRun
{
	SiftV6ParDsmHdr *hdr;
	SiftParCoord *coord;
	const SiftV6ParEngine *eng;
	SiftV6TopKRun *topk;			/* the heap the groups of the participant go to instead of the Gather */
	int			pid;
	bool		attached;
	SiftV6ParStage stage;
	void	   *local;				/* the state the scan folds into; the kept table, or the one of the final row */
	void	   *plocal;				/* the partition being emitted */
	SiftRadixLocal *rl;
	uint32		parts_seen;
	SiftV6ParWorkerStats stats;
	uint64		ring_recs;
	uint64		ring_bytes;
	uint32		export_cap;			/* the groups a table holds before it is handed to the partitions */
	uint32		tick;				/* tuples since the last interrupt check */
} SiftV6ParRun;

extern SiftV6ParRun *sift_v6_par_run_new(SiftV6ParDsmHdr *hdr, const SiftV6ParEngine *eng, SiftV6TopKRun *topk);
/* The next finished group of this participant, false when it has no more; the first call runs the scan */
extern bool sift_v6_par_next(SiftV6ParRun *r, Datum *values, bool *isnull);
/* The next group of a final state, or with a heap every group of it offered there (false at once) */
extern bool sift_v6_par_local_next(SiftV6ParRun *r, void *local, Datum *values, bool *isnull);
/* Leaves the barrier if this participant is still at it, publishes its counters, frees it */
extern void sift_v6_par_run_end(SiftV6ParRun *r);
/* The error cleanup of a run (PG_ENSURE_ERROR_CLEANUP): the barrier of the others is not left waiting for it */
extern void sift_v6_par_run_abort(int code, Datum arg);

/* the pieces the strategies share (v6_par_scan.c) */
extern int	sift_v6_par_attach(SiftV6ParRun *r);
extern void sift_v6_par_leave(SiftV6ParRun *r);
extern void sift_v6_par_scan(SiftV6ParRun *r, void (*relief) (SiftV6ParRun *r));
extern void sift_v6_par_export(SiftV6ParRun *r);
extern void sift_v6_par_export_plain(SiftV6ParRun *r);
extern void sift_v6_par_finish_stages(SiftV6ParRun *r);
extern void sift_v6_par_merge_part(SiftV6ParRun *r, void *into, uint32 part);
extern bool sift_v6_par_claim_part(SiftV6ParRun *r, uint32 *part);
extern uint32 sift_v6_par_barrier_event(void);

/* the strategies (v6_par_twostage.c, v6_par_radix.c) */
extern bool sift_v6_par_two_stage_next(SiftV6ParRun *r, Datum *values, bool *isnull);
extern bool sift_v6_par_radix_next(SiftV6ParRun *r, Datum *values, bool *isnull);
extern bool sift_v6_par_dist_next(SiftV6ParRun *r, Datum *values, bool *isnull);
extern bool sift_v6_par_emit_parts(SiftV6ParRun *r, Datum *values, bool *isnull);

#endif							/* SIFT_AGG_V6_PAR_H */
