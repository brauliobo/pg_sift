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
/* The same for a caller that read the metapage already (meta_flags) */
extern bool sift_index_out(Relation index, uint32 meta_flags);

/* What a statement that skipped a quarantined index counts for sift.quarantine */
typedef enum SiftSkipKind
{
	SIFT_SKIP_WRITE,
	SIFT_SKIP_VACUUM,
	SIFT_SKIP_SCAN
} SiftSkipKind;

extern void sift_quarantine_skip(Relation index, SiftSkipKind kind);

/*
 * Runs fn(arg), the work of a write or a VACUUM on the index, so that damage it finds does not fail the statement: it runs in a subtransaction, and damage of the index that
 * core/damage.h recorded rolls it back, leaves the index out of service and goes on with a WARNING (false is returned); an index that is out of service already is skipped the
 * same way. Every other error goes on. sift.on_corruption = error runs fn as it is.
 */
typedef void (*SiftGuarded) (void *arg);
/* A caller that catches errors asks of each: is it the damage recorded since it last asked (it is, once). Such an error ends the work on the index, and is not a failure to retry */
extern bool sift_damage_caught(const ErrorData *e);
/* The work on the index ended because it is out of service: counted for sift.quarantine, and said once a minute with a WARNING */
extern void sift_quarantine_stopped(Relation index, SiftSkipKind kind);
extern bool sift_quarantine_guard(Relation index, SiftSkipKind kind, SiftGuarded fn, void *arg);

/*
 * Where a scan opens, after it read the directory (meta_flags: the flags of its metapage): a plan made before the index left service is refused with SQLSTATE 40001, the one a
 * client retries, and the plans of this backend that name the index and its table are dropped, so the retry is planned without it.
 */
extern void sift_quarantine_net(Relation index, uint32 meta_flags);

#endif							/* SIFT_QUARANTINE_H */
