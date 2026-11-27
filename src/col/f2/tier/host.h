/*
 * col/f2/tier/host.h - the engine of f2_tier.h and f2_ttl.h bound to a relation: the policies (sift.tier_policies, sift.ttl_policies), the backend and cache each policy opens once per session, the
 * read of a part that is in object storage, and the maintenance step that moves parts to it, deletes the objects of parts that left, and drops the parts that expired.
 */
#ifndef SIFT_F2_TIER_HOST_H
#define SIFT_F2_TIER_HOST_H

#include "core/host.h"

#include "col/f2/store/store.h"
#include "col/f2/tier/tier.h"
#include "col/f2/ttl.h"

#define SIFT_TIER_CACHE_CHUNK	(1U << 20)
#define SIFT_TIER_BATCH				8	/* parts one maintenance step moves: the next step moves the next ones */

typedef struct SiftTierPolicy
{
	uint32		policy_id;
	char	   *uri;
	char	   *endpoint;		/* NULL: none */
	int64		move_after_us;
	uint32		cache_mb;
	uint64		scope_id;		/* the namespace of the objects of the relation: r<scope_id>/ */
	Oid			db_oid;			/* the database the objects belong to: a copy of it (CREATE DATABASE ... TEMPLATE) reads them and never writes or deletes one */
} SiftTierPolicy;

typedef struct SiftTtlPolicy
{
	char	   *col;
	int64		retention_us;
} SiftTtlPolicy;

/* The policy of the relation (filed under its OID, so a rename keeps it), or of a policy id (the ref of a part names it); false when there is none. Allocated in the current memory context */
extern bool sift_tier_policy_of(Relation rel, SiftTierPolicy *out);
extern void sift_tier_policy_by_id(Relation rel, uint32 policy_id, SiftTierPolicy *out);
extern bool sift_ttl_policy_of(Relation rel, SiftTtlPolicy *out);
/* Sets the flag that makes writers and compaction enforce the TTL on a relation that has a policy but not the flag (a restored or reset relation); false when it needed nothing */
extern bool sift_ttl_rearm_rocompat(Relation rel);

/* The writes of the policy functions, as the owner of the tables: a relation has at most one policy of each kind */
extern void sift_tier_policy_put(Relation rel, const char *uri, Interval *move_after, const char *endpoint, uint64 scope_id);
extern void sift_tier_policy_set_move_after(Relation rel, Interval *move_after);
extern void sift_tier_policy_delete(Relation rel);
extern void sift_ttl_policy_put(Relation rel, const char *col, Interval *retention);
extern void sift_ttl_policy_delete(Relation rel);

typedef struct SiftTierPolicyRow
{
	Oid			relid;
	char	   *uri;
	uint64		scope_id;
} SiftTierPolicyRow;

/* Every policy, for sift.tier_status */
extern SiftTierPolicyRow *sift_tier_policy_all(int *n);

/* The backend and the cache of a policy, opened once per session. An ERROR names what failed, never a secret; the _try forms return NULL and the reason in err (a store that cannot be reached) */
extern SiftTierBackend *sift_tier_backend(const SiftTierPolicy *p);
extern SiftTierCache *sift_tier_cache(const SiftTierPolicy *p);
extern SiftTierBackend *sift_tier_backend_try(const SiftTierPolicy *p, SiftTierErr *err);
extern SiftTierCache *sift_tier_cache_try(const SiftTierPolicy *p, SiftTierErr *err);

/* ERROR for a status that is not SIFT_TIER_OK */
extern void sift_tier_raise(SiftTierStatus st, const SiftTierErr *err, const char *what);

/* The refs of the tier directory as of the metapage now, kept for the next call of the session while tier_dir_gen is the same */
extern const SiftTierPartRef *sift_tier_refs(Relation rel, int *n);
/* The tiered parts read of the file go: a file truncated in place numbers its tier directory again from the start */
extern void sift_tier_refs_forget(const RelFileLocator *locator);

/* The time column the relation's parts age by: the slot (0 or 1) of the column named col, -1 when it is not a time column of the manifest */
extern int	sift_tier_time_slot(Relation rel, const SiftF2Store *store, const char *col);

/* The least time that is inside the window (now - span), in the unit of the slot */
extern int64 sift_tier_cutoff(const SiftF2Store *store, int slot, TimestampTz now, int64 span_us);

/*
 * One step of maintenance for the relation: the TTL drops, the deletion of the objects of the parts that left, the move of the next SIFT_TIER_BATCH eligible parts to object storage and the sweep of
 * the objects no commit names. Does nothing for a relation with neither policy. horizon: the oldest transaction id that may still be running.
 */
extern void sift_f2_tier_maintain(Relation rel, SiftF2Store *store, FullTransactionId horizon);

/*
 * What the object store of the policy of the relation still holds of it that no commit names, after one sweep: the objects that were just marked or wait for the end of their grace (what a rewrite
 * of the relation left for the standbys). -1 when the maintenance of the relation runs. An ERROR when the store cannot be swept.
 */
extern int	sift_f2_tier_reclaim(Relation rel, const SiftTierPolicy *pol);

/* Takes the SIFT_PART_EXT_EXPIRED mark off every part (sift.set_ttl with a NULL retention) */
extern void sift_f2_ttl_unmark_all(Relation rel);

/*
 * The check of every part of the relation that is in object storage (sift.check): its object is in the store, a .siftpart header of this version that names the part, of the size and with the
 * SHA-256 and CRC32C of the tier directory. Damage (an object that is missing or not the one named) is SIFT_CORRUPT; a store that cannot be reached is an ERROR. Reads each object whole.
 */
extern void sift_tier_check_remote(Relation rel);

/* The descriptor of the part of generation gen in parts, or NULL */
extern const SiftPartDesc *sift_tier_find_part(const SiftPartDesc *parts, int nparts, uint64 gen);

/*
 * What a change to the parts of a partition does as compaction does: it holds the compaction lock of the partition while it works (false: a compaction of the partition runs, try the next
 * one), and the partition lock from the read of the parts it changes to the end of its commit, so that no delete or flush lands in between.
 */
extern bool sift_tier_lock_compaction(Relation rel, uint64 part_id);
extern void sift_tier_unlock_compaction(Relation rel, uint64 part_id);
extern void sift_tier_lock_part(Relation rel, uint64 part_id);
extern void sift_tier_unlock_part(Relation rel, uint64 part_id);

/*
 * The extents a commit releases for the parts of the generations gens (found in parts): the blocks of each part that is not in object storage and, with dropped, the extents of its deletion vector
 * and the packed pages no part that stays is a slot of. A part that moves to object storage releases its blocks only. Returns the count; *out is palloc'd.
 */
extern int	sift_tier_release_extents(Relation rel, uint64 part_id, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, bool dropped, SiftExtent **out);

#endif							/* SIFT_F2_TIER_HOST_H */
