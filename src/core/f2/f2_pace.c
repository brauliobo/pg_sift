/*
 * core/f2/f2_pace.c - the pacing and the standby-safe horizon of the maintenance of format major 2 (core/f2/f2_pace.h).
 */
#include "postgres.h"

#include "core/f2/f2_pace.h"

#define LOAD(p)			__atomic_load_n(&(p), __ATOMIC_RELAXED)
#define STORE(p, v)		__atomic_store_n(&(p), (v), __ATOMIC_RELAXED)

void
sift_horizon_ring_init(SiftStandbyHorizon *r)
{
	memset(r, 0, sizeof(*r));
}

bool
sift_horizon_ring_record(SiftStandbyHorizon *r, TimestampTz now, FullTransactionId raw)
{
	uint32		v = __atomic_load_n(&r->version, __ATOMIC_ACQUIRE);
	uint32		head;

	if ((v & 1) != 0 || !__atomic_compare_exchange_n(&r->version, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return false;
	head = LOAD(r->head);
	if (head > 0 && now - LOAD(r->slot[(head - 1) % SIFT_HORIZON_RING_SLOTS].ts) < (TimestampTz) SIFT_HORIZON_SAMPLE_SECS * USECS_PER_SEC)
	{
		__atomic_store_n(&r->version, v, __ATOMIC_RELEASE);
		return false;
	}
	STORE(r->slot[head % SIFT_HORIZON_RING_SLOTS].ts, now);
	STORE(r->slot[head % SIFT_HORIZON_RING_SLOTS].horizon, U64FromFullTransactionId(raw));
	STORE(r->head, head + 1);
	__atomic_store_n(&r->version, v + 2, __ATOMIC_RELEASE);
	return true;
}

/* A copy of the ring as it was between two reads of an unchanged, even version; false when a writer did not leave it */
static bool
ring_copy(const SiftStandbyHorizon *r, SiftStandbyHorizon *out)
{
	for (int tries = 0; tries < 1000; tries++)
	{
		uint32		v = __atomic_load_n(&r->version, __ATOMIC_ACQUIRE);

		if ((v & 1) != 0)
			continue;
		out->head = LOAD(r->head);
		for (int i = 0; i < SIFT_HORIZON_RING_SLOTS; i++)
		{
			out->slot[i].ts = LOAD(r->slot[i].ts);
			out->slot[i].horizon = LOAD(r->slot[i].horizon);
		}
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&r->version, __ATOMIC_RELAXED) == v)
			return true;
	}
	return false;
}

FullTransactionId
sift_horizon_ring_lagged(const SiftStandbyHorizon *r, TimestampTz now, FullTransactionId raw)
{
	SiftStandbyHorizon c;
	const SiftHorizonSample *best = NULL,
			   *oldest = NULL;
	uint64		h;

	if (!ring_copy(r, &c))
		return raw;
	for (int i = 0; i < SIFT_HORIZON_RING_SLOTS; i++)
	{
		const SiftHorizonSample *s = &c.slot[i];

		if (s->ts == 0)
			continue;
		if (oldest == NULL || s->ts < oldest->ts)
			oldest = s;
		if (now - s->ts >= (TimestampTz) SIFT_STANDBY_SAFE_LAG_SECS * USECS_PER_SEC && (best == NULL || s->ts > best->ts))
			best = s;
	}
	if (best == NULL)
		best = oldest;
	if (best == NULL)
		return raw;
	h = best->horizon;
	return FullTransactionIdPrecedes(FullTransactionIdFromU64(h), raw) ? FullTransactionIdFromU64(h) : raw;
}

bool
sift_horizon_wraparound_urgent(TransactionId next, TransactionId oldest, int freeze_max_age)
{
	return TransactionIdIsNormal(oldest) && (uint32) (next - oldest) > (uint32) freeze_max_age / 2;
}

uint32
sift_f2_pace_units(uint64 *acc, uint64 npages)
{
	uint32		units;

	*acc += npages;
	units = (uint32) (*acc / SIFT_F2_PACE_UNIT_PAGES);
	*acc %= SIFT_F2_PACE_UNIT_PAGES;
	return units;
}

int64
sift_f2_wal_deficit_ms(uint64 wal_bytes, int kbps, int64 elapsed_ms)
{
	int64		due_ms;

	if (kbps <= 0)
		return 0;
	due_ms = (int64) (wal_bytes * 1000 / ((uint64) kbps * 1024));
	return Max(due_ms - elapsed_ms, 0);
}

#ifndef SIFT_S49_STANDALONE
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "postmaster/autovacuum.h"
#include "executor/instrument.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "core/horizon.h"
#include "core/maint_pacer.h"
#include "core/wal_conflict.h"
#include "core/waits.h"
#include "core/worker.h"

extern PGDLLIMPORT int sift_maintenance_wal_rate;

static SiftStandbyHorizon local_ring;
static SiftStandbyHorizon *ring = &local_ring;

void
sift_standby_horizon_attach(SiftStandbyHorizon *shared)
{
	ring = shared != NULL ? shared : &local_ring;
}

static bool
table_near_wraparound(Relation rel)
{
	Oid			table = rel->rd_index != NULL ? rel->rd_index->indrelid : RelationGetRelid(rel);
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(table));
	bool		urgent;

	if (!HeapTupleIsValid(tup))
		return false;
	urgent = sift_horizon_wraparound_urgent(XidFromFullTransactionId(ReadNextFullTransactionId()), ((Form_pg_class) GETSTRUCT(tup))->relfrozenxid, autovacuum_freeze_max_age);
	ReleaseSysCache(tup);
	return urgent;
}

FullTransactionId
sift_standby_safe_horizon(Relation index, bool urgent_freeze)
{
	FullTransactionId raw = sift_horizon(index);
	TimestampTz now = GetCurrentTimestamp();

	(void) sift_horizon_ring_record(ring, now, raw);
	if (!XLogStandbyInfoActive() || !sift_standby_conflicts || urgent_freeze || table_near_wraparound(index))
		return raw;
	return sift_horizon_ring_lagged(ring, now, raw);
}

/* The WAL a paced worker wrote since it began is held to sift.maintenance_wal_rate: it sleeps, interruptibly, for what it is ahead of the rate */
static void
wal_pace(void)
{
	static uint64 wal_base;
	static TimestampTz t0;
	int64		wait;

	if (sift_maintenance_wal_rate <= 0)
		return;
	if (t0 == 0)
	{
		t0 = GetCurrentTimestamp();
		wal_base = pgWalUsage.wal_bytes;
		return;
	}
	while ((wait = sift_f2_wal_deficit_ms(pgWalUsage.wal_bytes - wal_base, sift_maintenance_wal_rate, (GetCurrentTimestamp() - t0) / 1000)) > 0)
		sift_wait(SIFT_WAIT_MAINT_PACE, (long) Min(wait, 1000));
	if (GetCurrentTimestamp() - t0 > (TimestampTz) SIFT_F2_PACE_WINDOW_SECS * USECS_PER_SEC)
	{
		t0 = GetCurrentTimestamp();
		wal_base = pgWalUsage.wal_bytes;
	}
}

void
sift_f2_pace_pages(uint64 npages)
{
	static uint64 charged;

	if (!sift_worker_paced())
		return;
	wal_pace();
	for (uint32 u = sift_f2_pace_units(&charged, npages); u > 0; u--)
		sift_pacer_pause((SiftPacerTier) sift_worker_tier());
}
#endif
