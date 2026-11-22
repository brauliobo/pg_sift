/*
 * core/quarantine.h - a sift index that finds its own damage leaves service, and the database keeps serving.
 *
 * sift.on_corruption = quarantine (the default): damage raised through core/damage.h against a sift or sift_col index is recorded, and then raised as before, so the
 * statement that found it fails once. From then on the planner does not offer the index and statements run on the heap, with PostgreSQL's own plans and exact
 * answers; writes and maintenance skip it with a WARNING. A quarantined index leaves quarantine only by being rebuilt (REINDEX): while it was out its entries
 * were not kept up to date and VACUUM freed the rows they point to.
 * sift.on_corruption = error: the damage is raised and nothing else changes (the behaviour of the releases before 5.0).
 *
 * The state is the flag SIFT_META_QUARANTINED of the metapage (written by a background worker, it reaches standbys through WAL and survives restarts) and a registry in
 * shared memory, keyed by the file of the index, that is in force at once and where the flag cannot be written (a standby, a damaged metapage).
 */
#ifndef SIFT_QUARANTINE_H
#define SIFT_QUARANTINE_H

#include "core/host.h"

#include "core/store.h"

typedef enum SiftOnCorruption
{
	SIFT_ON_CORRUPTION_ERROR,
	SIFT_ON_CORRUPTION_QUARANTINE
} SiftOnCorruption;

extern PGDLLIMPORT int sift_on_corruption;

extern void sift_quarantine_define_gucs(void);
/* The planner hook that leaves a quarantined index out of the index list of its relation (core/quarantine_plan.c); first of the planner hooks of sift to be installed */
extern void sift_quarantine_plan_init(void);

/* The index is out of service: recorded in this backend or in the registry, or flagged in its metapage. False under sift.on_corruption = error */
extern bool sift_index_quarantined(Relation index);
/* Recorded in this backend or in the registry: no page is read. The callbacks PostgreSQL makes while it plans (amcanreturn) ask this, as the planner has not left the index out yet */
extern bool sift_index_known_out(Relation index);
/* A plan that left the index out of its index list: asks for the worker that flags the metapage when it is not flagged yet */
extern void sift_quarantine_planned(Relation index);
/*
 * The amcostestimate of an index that is out of service: it is given the price no path beats and the worker that flags the metapage is asked for; false when the index is in service. PostgreSQL asks
 * the callbacks of an access method while it plans, before a session that did not preload sift has installed the planner hook that leaves the index out of the list of its relation.
 */
extern bool sift_quarantine_cost(Relation index, double index_pages, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages);
/* The first cause that this server recorded for the index being out of service (what sift.quarantine says), a copy in the current memory context; NULL when none is recorded: a flag in the metapage that no backend met */
extern char *sift_quarantine_reason(Relation index);
/* A function that has no result without the index (sift.facets): refused with a hint while the index is out of service */
extern void sift_quarantine_require(Relation index, const char *what);
/* The indexes of this database that are out of service, quoted and joined by commas, for the hint of a function that exists only inside a sift scan; NULL when none is known */
extern const char *sift_quarantine_names(void);
/* The same for a caller that read the metapage already (meta_flags) */
extern bool sift_index_out(Relation index, uint32 meta_flags);

/* What a statement that skipped a quarantined index counts for sift.quarantine */
typedef enum SiftQuarSkip
{
	SIFT_SKIP_WRITE,
	SIFT_SKIP_VACUUM,
	SIFT_SKIP_SCAN
} SiftQuarSkip;

extern void sift_quarantine_skip(Relation index, SiftQuarSkip kind);

/*
 * Runs fn(arg), the work of a write or a VACUUM on the index, unless the index is out of service: that one is skipped (false is returned, counted for sift.quarantine and said with a
 * WARNING). There is no subtransaction around fn, as a write and a VACUUM would pay it every time: damage fn finds is recorded where it is raised and fails the statement once, and the
 * statements that follow skip the index.
 */
typedef void (*SiftGuarded) (void *arg);
/* A caller that catches errors asks of each: is it the damage recorded since it last asked (it is, once). Such an error ends the work on the index, and is not a failure to retry */
extern bool sift_damage_caught(const ErrorData *e);
/* The work on the index ended because it is out of service: counted for sift.quarantine, and said once a minute with a WARNING */
extern void sift_quarantine_stopped(Relation index, SiftQuarSkip kind);
/* The entry of a write or a VACUUM into an index of any format: true, after the work was counted and said as stopped, when the index is out of service (a plain index or a table never is) */
extern bool sift_quarantine_skips(Relation index, SiftQuarSkip kind);
/* The step of the queue (SIFT_MK_QUARANTINE_MARK): flags the metapage of an index the registry holds out of service, the work of the worker of core/quarantine.c, in the transaction of the caller */
extern void sift_quarantine_mark(Relation index);
extern bool sift_quarantine_run(Relation index, SiftQuarSkip kind, SiftGuarded fn, void *arg);

/*
 * Where a scan opens, after it read the directory (meta_flags: the flags of its metapage): a plan made before the index left service is refused with SQLSTATE 40001, the one a
 * client retries, and the plans of this backend that name the index and its table are dropped, so the retry is planned without it.
 */
extern void sift_quarantine_net(Relation index, uint32 meta_flags);

/* The worker that flags the metapage of an index the registry holds out of service (main_arg: the oid of the index) */
extern void sift_quarantine_main(Datum main_arg);

#endif							/* SIFT_QUARANTINE_H */
