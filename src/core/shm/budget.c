/*
 * core/shm/budget.c - the ledger of the shared memory pools (core/shm/budget.h): a table of the dynamic shared memory registry that adds up what each pool holds, with atomic counters and no lock.
 */
#include "postgres.h"

#include <limits.h>

#include "port/atomics.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include "col/dsmreg.h"
#include "core/shm/budget.h"

#define LEDGER_NAME		"sift_shm_ledger"

int			sift_shared_cache_kb = 128 * 1024;

/* the share of the limit of each pool, in percent */
/* MEM[MEM-SHM-21]: shm_ledger 3-pool split (42/42/16% of sift.shared_cache_kb) */
static const int pool_pct[SIFT_SHM_NPOOL] = {
	[SIFT_SHM_MANIFEST] = 42,
	[SIFT_SHM_COL_DICT] = 42,
	[SIFT_SHM_COL_LEAF] = 16
};

static const char *const pool_name[SIFT_SHM_NPOOL] = {
	[SIFT_SHM_MANIFEST] = "manifest",
	[SIFT_SHM_COL_DICT] = "col_dict",
	[SIFT_SHM_COL_LEAF] = "col_leaf"
};

typedef struct SiftShmPoolSlot
{
	pg_atomic_uint64 bytes_used;
	pg_atomic_uint64 peak_bytes;
	pg_atomic_uint64 refusals;
	pg_atomic_uint64 evictions;
	char		pad[PG_CACHE_LINE_SIZE - 4 * sizeof(pg_atomic_uint64)];
} SiftShmPoolSlot;

StaticAssertDecl(sizeof(SiftShmPoolSlot) == PG_CACHE_LINE_SIZE, "a pool of the ledger occupies one cache line");

/* No counter of the total is shared by all the pools: a pool is bounded by its share, and the sum of the counters, read after the pool's own is raised, bounds the total once the limit is lowered under what the pools hold */
typedef struct SiftShmLedger
{
	SiftShmPoolSlot pool[SIFT_SHM_NPOOL];
} SiftShmLedger;

StaticAssertDecl(sizeof(SiftShmLedger) == SIFT_SHM_NPOOL * PG_CACHE_LINE_SIZE, "the ledger is one cache line per pool");

static SiftShmLedger *ledger;
static int64 local_credit[SIFT_SHM_NPOOL];
static bool exit_registered;

void
sift_shm_budget_define_gucs(void)
{
	DefineCustomIntVariable("sift.shared_cache_kb",
							"Dynamic shared memory, for all backends together, that the shared caches of the extension keep (write-buffer references, parsed dictionaries and index statistics) (0: none is shared).",
							"The pools share it: 42% the manifests and write-buffer references, 42% the parsed dictionaries, 16% the statistics of the indexes. A pool that is full does without: a row is written with its values, a backend loads its own tables.",
							&sift_shared_cache_kb, 128 * 1024, 0, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
}

Size
sift_shm_limit_bytes(void)
{
	return (Size) sift_shared_cache_kb * 1024;
}

Size
sift_shm_pool_bytes(SiftShmPoolId pool)
{
	return sift_shm_limit_bytes() * pool_pct[pool] / 100;
}

static SiftShmLedger *
ledger_base(void *raw)
{
	return (SiftShmLedger *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
ledger_init(SIFT_DSM_INIT_ARGS)
{
	SiftShmLedger *l = ledger_base(ptr);

	for (int i = 0; i < SIFT_SHM_NPOOL; i++)
	{
		pg_atomic_init_u64(&l->pool[i].bytes_used, 0);
		pg_atomic_init_u64(&l->pool[i].peak_bytes, 0);
		pg_atomic_init_u64(&l->pool[i].refusals, 0);
		pg_atomic_init_u64(&l->pool[i].evictions, 0);
	}
}

static SiftShmLedger *
ledger_get(void)
{
	if (ledger == NULL)
	{
		/* MEM[MEM-SHM-02]: DSM ledger sift_shm_ledger (768 B fixed; 5x128 B pool counters) */
		void	   *raw = sift_dsm_try(LEDGER_NAME, sizeof(SiftShmLedger) + PG_CACHE_LINE_SIZE, ledger_init);

		if (raw != NULL)
			ledger = ledger_base(raw);
	}
	return ledger;
}

/* adds bytes to a counter that may not pass limit */
static bool
add_within(pg_atomic_uint64 *counter, Size bytes, Size limit)
{
	uint64		used = pg_atomic_read_u64(counter);

	do
	{
		if (used + bytes > limit)
			return false;
	} while (!pg_atomic_compare_exchange_u64(counter, &used, used + bytes));
	return true;
}

static void
note_peak(pg_atomic_uint64 *peak, uint64 used)
{
	uint64		seen = pg_atomic_read_u64(peak);

	while (used > seen && !pg_atomic_compare_exchange_u64(peak, &seen, used))
		;
}

static Size
ledger_total(SiftShmLedger *l)
{
	Size		total = 0;

	for (int i = 0; i < SIFT_SHM_NPOOL; i++)
		total += pg_atomic_read_u64(&l->pool[i].bytes_used);
	return total;
}

static bool
ledger_take(SiftShmLedger *l, SiftShmPoolId pool, Size bytes)
{
	SiftShmPoolSlot *s = &l->pool[pool];

	if (!add_within(&s->bytes_used, bytes, sift_shm_pool_bytes(pool)))
		return false;
	if (ledger_total(l) > sift_shm_limit_bytes())
	{
		pg_atomic_fetch_sub_u64(&s->bytes_used, bytes);
		return false;
	}
	note_peak(&s->peak_bytes, pg_atomic_read_u64(&s->bytes_used));
	return true;
}

static void
ledger_give(SiftShmLedger *l, SiftShmPoolId pool, Size bytes)
{
	Assert(pg_atomic_read_u64(&l->pool[pool].bytes_used) >= bytes);
	pg_atomic_fetch_sub_u64(&l->pool[pool].bytes_used, bytes);
}

static void
return_credits(int code, Datum arg)
{
	for (int i = 0; i < SIFT_SHM_NPOOL; i++)
		if (local_credit[i] > 0)
		{
			ledger_give(ledger, i, local_credit[i]);
			local_credit[i] = 0;
		}
}

static bool
draw_credit(SiftShmLedger *l, SiftShmPoolId pool, Size bytes)
{
	Size		draw = SIFT_SHM_BATCH_BYTES;

	if (!exit_registered)
	{
		before_shmem_exit(return_credits, 0);
		exit_registered = true;
	}
	if (!ledger_take(l, pool, draw))
	{
		draw = bytes - (Size) local_credit[pool];
		if (!ledger_take(l, pool, draw))
			return false;
	}
	local_credit[pool] += draw;
	return true;
}

bool
sift_shm_reserve(SiftShmPoolId pool, Size bytes)
{
	SiftShmLedger *l = ledger_get();

	if (l == NULL)
		return false;
	if (bytes >= SIFT_SHM_BATCH_BYTES)
	{
		if (ledger_take(l, pool, bytes))
			return true;
	}
	else if (local_credit[pool] >= (int64) bytes || draw_credit(l, pool, bytes))
	{
		local_credit[pool] -= bytes;
		return true;
	}
	pg_atomic_fetch_add_u64(&l->pool[pool].refusals, 1);
	return false;
}

void
sift_shm_release(SiftShmPoolId pool, Size bytes)
{
	SiftShmLedger *l = ledger_get();

	if (l == NULL)
		return;
	if (bytes >= SIFT_SHM_BATCH_BYTES)
		ledger_give(l, pool, bytes);
	else if ((local_credit[pool] += bytes) >= 2 * (int64) SIFT_SHM_BATCH_BYTES)
	{
		ledger_give(l, pool, local_credit[pool] - SIFT_SHM_BATCH_BYTES);
		local_credit[pool] = SIFT_SHM_BATCH_BYTES;
	}
}

bool
sift_shm_reserve_shared(SiftShmPoolId pool, Size bytes)
{
	SiftShmLedger *l = ledger_get();

	if (l != NULL && ledger_take(l, pool, bytes))
		return true;
	if (l != NULL)
		pg_atomic_fetch_add_u64(&l->pool[pool].refusals, 1);
	return false;
}

void
sift_shm_release_shared(SiftShmPoolId pool, Size bytes)
{
	SiftShmLedger *l = ledger_get();

	if (l != NULL)
		ledger_give(l, pool, bytes);
}

Size
sift_shm_pool_used(SiftShmPoolId pool)
{
	SiftShmLedger *l = ledger_get();

	return l == NULL ? 0 : pg_atomic_read_u64(&l->pool[pool].bytes_used);
}

Size
sift_shm_total_bytes(void)
{
	SiftShmLedger *l = ledger_get();

	return l == NULL ? 0 : ledger_total(l);
}

const char *
sift_shm_pool_name(SiftShmPoolId pool)
{
	return pool_name[pool];
}

void
sift_shm_pool_stat(SiftShmPoolId pool, SiftShmPoolStat *out)
{
	SiftShmLedger *l = ledger_get();

	*out = (SiftShmPoolStat) {.limit_bytes = sift_shm_pool_bytes(pool),.ledger = l != NULL};
	if (l == NULL)
		return;
	out->used_bytes = pg_atomic_read_u64(&l->pool[pool].bytes_used);
	out->peak_bytes = pg_atomic_read_u64(&l->pool[pool].peak_bytes);
	out->evictions = pg_atomic_read_u64(&l->pool[pool].evictions);
	out->refusals = pg_atomic_read_u64(&l->pool[pool].refusals);
}

void
sift_shm_note_eviction(SiftShmPoolId pool, uint64 count)
{
	SiftShmLedger *l = ledger_get();

	if (l != NULL)
		pg_atomic_fetch_add_u64(&l->pool[pool].evictions, count);
}
