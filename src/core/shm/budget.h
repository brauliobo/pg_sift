/*
 * core/shm/budget.h - the one budget of the dynamic shared memory the extension keeps for all the backends of a server.
 *
 * sift.shared_cache_kb is the most that the five pools (the manifests, the lookup tables, the dimension maps, the parsed dictionaries and the leaf statistics) may hold together.
 * The fixed pools take a share of it when they are made, a growing pool asks for its bytes as they are used: a ledger in shared memory adds them up in one atomic counter of
 * its own cache line for each pool, so that a pool that is full is refused and does without (a table loaded by the backend), and what the server holds in /dev/shm is the setting at the most.
 * A backend takes small requests (under SIFT_SHM_BATCH_BYTES) from a credit it draws from the ledger a batch at a time: they touch no shared counter, and the credit goes back when the backend exits.
 */
#ifndef SIFT_SHMBUDGET_H
#define SIFT_SHMBUDGET_H

#include "postgres.h"

typedef enum SiftShmPoolId
{
	SIFT_SHM_MANIFEST = 0,
	SIFT_SHM_AGG_LOOKUP,
	SIFT_SHM_AGG_DIMMAP,
	SIFT_SHM_COL_DICT,
	SIFT_SHM_COL_LEAF,
	SIFT_SHM_NPOOL
} SiftShmPoolId;

#define SIFT_SHM_BATCH_BYTES	((Size) 64 * 1024)

extern PGDLLIMPORT int sift_shared_cache_kb;

extern void sift_shm_budget_define_gucs(void);
extern Size sift_shm_limit_bytes(void);

/* the bytes a pool is sized at: its share of the limit */
extern Size sift_shm_pool_bytes(SiftShmPoolId pool);

/* takes bytes for a pool from the limit; false when the pool or the pools together hold their share already */
extern bool sift_shm_reserve(SiftShmPoolId pool, Size bytes);
extern void sift_shm_release(SiftShmPoolId pool, Size bytes);

/*
 * The same for the bytes of a structure that outlives the backend that makes it and that any backend may free (a snapshot in a shared area): charged and given back to the pool itself, never
 * through the credit of a backend, which goes back to the ledger only when the backend that drew it exits
 */
extern bool sift_shm_reserve_shared(SiftShmPoolId pool, Size bytes);
extern void sift_shm_release_shared(SiftShmPoolId pool, Size bytes);

/* what the ledger holds: the bytes of a pool or of all of them, credits of the backends included */
extern Size sift_shm_pool_used(SiftShmPoolId pool);
extern Size sift_shm_total_bytes(void);

/* what a pool of the ledger reports (sift.shared_memory_pools): ledger is false when the ledger could not be had (the directory of the shared memory is full) and the counters are 0 */
typedef struct SiftShmPoolStat
{
	uint64		limit_bytes;
	uint64		used_bytes;
	uint64		peak_bytes;
	uint64		evictions;
	uint64		refusals;
	bool		ledger;
} SiftShmPoolStat;

extern const char *sift_shm_pool_name(SiftShmPoolId pool);
extern void sift_shm_pool_stat(SiftShmPoolId pool, SiftShmPoolStat *out);

/* a pool gave count entries back to make room for others */
extern void sift_shm_note_eviction(SiftShmPoolId pool, uint64 count);

#endif
