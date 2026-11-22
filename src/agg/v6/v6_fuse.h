/*
 * agg/v6/v6_fuse.h - SiftV6Fuse: the arms of one statement that read the same index under the same snapshot (the branches of a CTE that was inlined, the sides of a UNION ALL) share one
 * pass of the scan. The first arm of a group that is asked for a tuple leads: it finds the other arms that have not started and can read with it (a cohort), runs the one scan, and every
 * batch the scan accepts goes to every arm, each folding it into its own groups; the others find their groups ready when they are asked.
 *
 * Which arms fuse is decided here, on plain descriptions (SiftV6FuseArm): an arm that is in a parallel worker, depends on a parameter another node sets, or reads a pre-aggregate runs alone;
 * arms with the same scan keys share them as they are, and arms with other keys fuse only when the may_union of the arm says they can (a quals the arms have in common and the others a gate evaluates), the values of their lead keys unite into at most SIFT_F2_MAX_RANGES
 * ranges (each is probed apart, so the pass reads no granule that no arm asked for), and the windows of their time columns do not leave more than a quarter between them. Each such arm
 * takes of the rows of the pass those that its own quals not common to all arms allow (agg/v6/v6_fuse_keys.h).
 * The arms of a statement are kept in a registry of the executor state of the statement, which goes with the memory of the query.
 */
#ifndef SIFT_AGG_V6_FUSE_H
#define SIFT_AGG_V6_FUSE_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"

#include "col/f2/f2.h"

struct SiftV6Spec;
struct SiftV6QualSet;

#define SIFT_V6_FUSE_MAX_ARMS	16

struct SiftV6FuseCohort;

typedef struct SiftV6FuseArm
{
	void	   *owner;			/* the node of the arm */
	Oid			indexoid;
	const void *snap;			/* compared as a pointer: the arms of one statement share the snapshot of its estate */
	uint64		scan_sig;		/* of the scan keys: equal signatures are equal keys */
	bool		ordered;		/* the arm reads its rows in the order of the index */
	bool		parallel;		/* a parallel-aware node or a worker */
	bool		param_exec;		/* depends on a parameter another node sets: it scans again for each value */
	bool		cube_served;	/* reads a pre-aggregate, which is cheaper than any shared pass */
	bool		started;
	bool		detached;
	SiftF2KeySet key_set;
	SiftF2TimeWin time_win;
	int			lead_att;		/* the lead key column as the index numbers it, 0 when the arm has none to unite */
	List	   *quals;			/* the quals of the scan of the arm */
	struct SiftV6QualSet *qset;	/* the same quals by hash, with what the gate of each costs to know (v6_fuse_keys.c) */
	const struct SiftV6Spec *spec;	/* the spec of the node */
	bool		(*may_union) (const struct SiftV6FuseArm *lead, const struct SiftV6FuseArm *other);	/* NULL: arms of other keys fuse by their key sets alone */
	struct SiftV6FuseCohort *cohort;
} SiftV6FuseArm;

typedef struct SiftV6FuseCohort
{
	int			narms;
	SiftV6FuseArm *arms[SIFT_V6_FUSE_MAX_ARMS];	/* the leader first */
	bool		same_keys;		/* every arm has the scan keys of the leader */
	SiftF2KeySet union_keys;
	SiftF2TimeWin union_time;
	uint64		granules_scanned;
	uint64		granules_pruned;
	uint64		rows_shared_vetted;
	bool		ran;
} SiftV6FuseCohort;

typedef struct SiftV6FuseReg
{
	List	   *arms;			/* of SiftV6FuseArm *, in the memory of the query */
} SiftV6FuseReg;

/*
 * The union of the ranges of two key sets, each in ascending order of the leading integer, as at most SIFT_F2_MAX_RANGES disjoint ranges in that order, each range a half-open [ilo, ihi) of an arm; false when
 * they unite into more, or a range holds only NULLs (it cannot be merged with the others by its integers).
 */
extern bool sift_v6_fuse_union_keys(const SiftF2KeySet *a, const SiftF2KeySet *b, SiftF2KeySet *out);

/* The window of both: the time columns every arm narrows, from the least lower bound to the greatest upper one; false when the gap that leaves between the arms is more than a quarter of what they ask for */
extern bool sift_v6_fuse_union_time(const SiftF2TimeWin *a, const SiftF2TimeWin *b, SiftF2TimeWin *out);

/* The registry of the estate (made on first use, gone with the memory of the query) */
extern SiftV6FuseReg *sift_v6_fuse_reg(EState *estate);

/* The arm joins the registry of the estate and is not started */
extern void sift_v6_fuse_arm_add(EState *estate, SiftV6FuseArm *arm);

/*
 * The arm is asked for its first tuple: the cohort it leads, with the arms that have not started and read with it (and marked started), or NULL when it runs alone. shared_keys_only: only
 * the arms of equal scan keys fuse (the node folds every batch with the same keys). explain_only: an EXPLAIN that runs nothing fuses nothing.
 */
extern SiftV6FuseCohort *sift_v6_fuse_lead(EState *estate, SiftV6FuseArm *arm, bool shared_keys_only, bool explain_only);

/* The arm scans again with other parameters, or ends: it leaves its cohort; the cohort is gone with its last arm */
extern void sift_v6_fuse_detach(SiftV6FuseArm *arm);

/* What the scan of a cohort did, for EXPLAIN */
extern const char *sift_v6_fuse_role(const SiftV6FuseArm *arm);

/*
 * The batch of a pass to every arm of the cohort: arm_mask(ctx, i, live, out) puts in out the rows of the shared batch (live: those every arm may take) that arm i takes and returns how
 * many; an arm that takes none is not consumed for the batch. Returns the number of arms consumed. A batch of no live row is not given to any arm.
 */
typedef struct SiftV6FuseOps
{
	uint16		(*arm_mask) (void *ctx, int arm, const uint64 *live, uint64 *out);
	void		(*consume) (void *ctx, int arm, const uint64 *mask, uint16 n_active);
} SiftV6FuseOps;

extern int	sift_v6_fuse_dispatch(SiftV6FuseCohort *c, const SiftV6FuseOps *ops, void *ctx, const uint64 *live, uint16 n_live);

#endif							/* SIFT_AGG_V6_FUSE_H */
