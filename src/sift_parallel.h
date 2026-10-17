/*
 * sift_parallel.h - shared state of a parallel-aware SiftScan / SiftCount
 * (DESIGN.md extras, item 9).
 *
 * The state lives in the custom scan node's DSM chunk. Range scans (heap-order
 * matches, count) hand out morsels of superblocks from one atomic counter.
 * A ranked scan hands out morsels of its bound-ordered units the same way and
 * shares the threshold: every participant keeps a top-k of what it scored,
 * publishes the k-th best score it holds (any participant's is a lower bound
 * of the final k-th best, so the maximum prunes for all), and the last one to
 * finish merges the published hits and emits them; the others return none.
 */
#ifndef SIFT_PARALLEL_H
#define SIFT_PARALLEL_H

#include "sift_host.h"

#include "sift.h"

/* A ranked hit as exchanged between participants */
typedef struct SiftParHit
{
	SiftKey		key;			/* the visible tuple */
	SiftKey		root;			/* the index entry that found it (differs along a HOT chain) */
	float4		score;
} SiftParHit;

typedef struct SiftParSlot
{
	dsa_pointer hits;
	int64		nhits;
} SiftParSlot;

typedef struct SiftParShared
{
	int			nplanned;		/* participants the plan counts on: workers + leader */
	int			nslots;
	uint32		nsb;			/* range scans: superblocks [0, nsb) */
	uint32		morsel;			/* range scans: superblocks per claim */
	pg_atomic_uint64 next;		/* next morsel */
	pg_atomic_uint32 theta;		/* ranked: float bits of the threshold */
	slock_t		mutex;			/* guards what follows */
	int			nattached;
	int			nfinished;
	bool		emitter_taken;
	bool		mutable_taken;
	bool		units_set;
	uint64		units_sig;
	int64		units_n;
	dsa_pointer bind;			/* ranked: bind of the leader, see sift_plan_export */
	Size		bind_len;
	SiftParSlot slot[FLEXIBLE_ARRAY_MEMBER];
} SiftParShared;

/* A participant's view */
typedef struct SiftPar
{
	SiftParShared *sh;
	dsa_area   *area;
	int			slot;
} SiftPar;

extern Size sift_par_estimate(int nworkers);

/* Leader: (re)initializes the chunk for a scan over nsb superblocks */
extern void sift_par_init(SiftParShared *sh, int nworkers, uint32 nsb);

extern void sift_par_reinit(SiftParShared *sh, dsa_area *area, int nworkers, uint32 nsb);

/* Leader: stores the bind payload (copied into the area) */
extern void sift_par_set_bind(SiftParShared *sh, dsa_area *area, const void *data, Size len);
extern const void *sift_par_get_bind(const SiftPar *par, Size *len);

/* A participant starts; slots are handed out in attach order */
extern void sift_par_attach(SiftPar *par);

/* Range scans: the next superblock range [lo, hi); the SiftNextRangeFn of the iterator */
extern bool sift_par_next_range(void *arg, uint32 *lo, uint32 *hi);

/* Ranked: the unit list must be the same everywhere, errors if the participants disagree */
extern void sift_par_check_units(SiftPar *par, int64 nunit, uint64 sig);

/* Ranked: the next morsel of units [lo, hi) of nunit; false when all are claimed */
extern bool sift_par_next_units(SiftPar *par, int64 nunit, int64 *lo, int64 *hi);

/* Ranked: units claimed so far, at most nunit */
extern int64 sift_par_units_claimed(const SiftPar *par, int64 nunit);

/* Ranked: true for the one participant that scores the mutable segments' matches */
extern bool sift_par_claim_mutable(SiftPar *par);

extern float4 sift_par_theta(const SiftPar *par);
extern void sift_par_raise_theta(SiftPar *par, float4 theta);

/* Ranked: publishes this participant's hits; then finish says whether it is the one to emit */
extern void sift_par_publish(SiftPar *par, const SiftParHit *hits, int64 n);
extern bool sift_par_finish(SiftPar *par);

/* The emitter: every published hit, palloc'd */
extern SiftParHit *sift_par_collect(SiftPar *par, int64 *n);

#endif							/* SIFT_PARALLEL_H */
