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

/* What a statement that skipped a quarantined index counts for sift.quarantine */
typedef enum SiftSkipKind
{
	SIFT_SKIP_WRITE,
	SIFT_SKIP_VACUUM,
	SIFT_SKIP_SCAN
} SiftSkipKind;

extern void sift_quarantine_skip(Relation index, SiftSkipKind kind);

#endif							/* SIFT_QUARANTINE_H */
