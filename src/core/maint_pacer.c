/*
 * core/maint_pacer.c - see maint_pacer.h
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "utils/backend_status.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "col/dsmreg.h"
#include "core/maint_pacer.h"
#include "core/sift.h"
#include "core/waits.h"
#include "core/worker.h"
#include "core/cachebudget.h"

#define TELEMETRY_NAME		"sift_pacer_1"
#define SAMPLE_US			(250 * INT64CONST(1000))	/* a worker looks at the backends at most this often */
#define LAT_FLOOR_US		1000						/* the slow average is never taken below 1 ms: a faster scan is not a spike */
#define DEFER_MIN_MS		5000						/* the backoff of a deferred step, by how far into the heavy regime the load is */
#define DEFER_SPAN_MS		25000
#define DEFER_LOG_AFTER_US	(60 * USECS_PER_SEC)		/* the deferral is said at LOG after this long without a quiet moment, once per DEFER_LOG_EVERY_US */
#define DEFER_LOG_EVERY_US	(10 * USECS_PER_MINUTE)
#define FOREGROUND_EVERY	64
#define FG_IDLE_US			USECS_PER_SEC				/* a foreground that reported nothing for this long has ended: its averages fall back to the usual */

typedef struct SiftPacerTelemetry
{
	pg_atomic_uint64 fg_lat_ewma_us;	/* fast average (1/16) of the duration of a scan of the foreground */
	pg_atomic_uint64 fg_lat_base_us;	/* slow average (1/256) of the same, never below LAT_FLOOR_US */
	pg_atomic_uint64 last_sample_ts;	/* the last look at the backends */
	pg_atomic_uint64 last_fg_ts;		/* the last operation of the foreground that fed the averages */
	pg_atomic_uint64 defer_until;		/* a P2 or P3 step does not start before this */
	pg_atomic_uint64 deferred_since;	/* the first deferral of this heavy spell, 0 when there is none */
	pg_atomic_uint64 defer_logged_at;
	pg_atomic_uint64 starved_at[SIFT_TIERS];	/* the last request of each tier that found no free worker */
	pg_atomic_uint32 active_oltp;
	pg_atomic_uint32 pacer_mode;
	pg_atomic_uint32 share_pm;
	pg_atomic_uint32 eff_delay_ms;		/* the delay of a P1 step now */
	pg_atomic_uint32 lock_yields;
} SiftPacerTelemetry;

BufferAccessStrategy sift_maint_ring;
uint32		sift_pacer_fg_ops = 15;

static void
telemetry_init(SIFT_DSM_INIT_ARGS)
{
	SiftPacerTelemetry *t = ptr;

	pg_atomic_init_u64(&t->fg_lat_ewma_us, 0);
	pg_atomic_init_u64(&t->fg_lat_base_us, 0);
	pg_atomic_init_u64(&t->last_sample_ts, 0);
	pg_atomic_init_u64(&t->last_fg_ts, 0);
	pg_atomic_init_u64(&t->defer_until, 0);
	pg_atomic_init_u64(&t->deferred_since, 0);
	pg_atomic_init_u64(&t->defer_logged_at, 0);
	for (int i = 0; i < SIFT_TIERS; i++)
		pg_atomic_init_u64(&t->starved_at[i], 0);
	pg_atomic_init_u32(&t->active_oltp, 0);
	pg_atomic_init_u32(&t->pacer_mode, SIFT_PACE_FULL);
	pg_atomic_init_u32(&t->share_pm, 0);
	pg_atomic_init_u32(&t->eff_delay_ms, 0);
	pg_atomic_init_u32(&t->lock_yields, 0);
}

static SiftPacerTelemetry *
telemetry(void)
{
	static SiftPacerTelemetry own;
	static SiftPacerTelemetry *t;

	/* MEM[MEM-SHM-11]: DSM pacer telemetry sift_pacer_1 (112 B fixed) */
	if (t == NULL && (t = sift_dsm_try(TELEMETRY_NAME, sizeof(own), telemetry_init)) == NULL)
	{
		SIFT_DSM_INIT_CALL(telemetry_init, &own);
		t = &own;
	}
	return t;
}

static int
share_pm_of(double share)
{
	return (int) (Min(Max(share, 0.0), 1.0) * 1000 + 0.5);
}

SiftPaceDecision
sift_pacer_decide(const SiftPaceLoad *l)
{
	double		load = (double) l->active_oltp / Max(l->cpu_slots, 2);
	double		spike = (double) l->lat_ewma_us / Max(l->lat_base_us, LAT_FLOOR_US);

	if (load >= 0.75 || spike >= 2.5 || l->lock_yields > 0)
		return (SiftPaceDecision) {SIFT_PACE_THROTTLED, share_pm_of(Max((load - 0.75) / 0.75, (spike - 2.5) / 2.5))};
	if (load >= 0.25 || spike >= 1.5)
		return (SiftPaceDecision) {SIFT_PACE_MODERATE, share_pm_of(Max((load - 0.25) / 0.5, spike - 1.5))};
	return (SiftPaceDecision) {SIFT_PACE_FULL, 0};
}

static int
lerp(int lo, int hi, int share_pm)
{
	return lo + (hi - lo) * share_pm / 1000;
}

int
sift_pacer_delay_ms(const SiftPaceDecision *d, SiftPacerTier tier, int base_delay_ms)
{
	switch (d->mode)
	{
		case SIFT_PACE_FULL:
			return tier == SIFT_TIER_P0 ? 0 : base_delay_ms;
		case SIFT_PACE_MODERATE:
			return tier == SIFT_TIER_P0 ? 0 : Max(base_delay_ms, tier == SIFT_TIER_P1 ? lerp(2, 15, d->share_pm) : lerp(10, 30, d->share_pm));
		case SIFT_PACE_THROTTLED:
		case SIFT_PACE_DEFERRED:
			return tier == SIFT_TIER_P0 ? lerp(0, 2, d->share_pm) : tier == SIFT_TIER_P1 ? Max(base_delay_ms, lerp(20, 50, d->share_pm)) : -1;
	}
	return 0;
}

int
sift_pacer_running_backends(Oid db)
{
	int			n = 0;

	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *s = &pgstat_get_local_beentry_by_index(i)->backendStatus;

		n += s->st_backendType == B_BACKEND && (!OidIsValid(db) || s->st_databaseid == db) && s->st_state == STATE_RUNNING;
	}
	return n;
}

/*
 * The decision for now, from the telemetry: a caller that finds the last look older than SAMPLE_US makes a new one (the first to claim it; the others use what it writes). A foreground that has
 * been silent for FG_IDLE_US has ended: its fast average goes back to the slow one and a backoff of the heavy regime ends with it. An observer (a client backend that reads the pacer) is not
 * load, and leaves the locks yielded to the worker that will look next.
 */
static SiftPaceDecision
current(SiftPacerTelemetry *t, bool observer)
{
	TimestampTz now = GetCurrentTimestamp();
	uint64		last = pg_atomic_read_u64(&t->last_sample_ts);

	if (now - (TimestampTz) last >= SAMPLE_US && pg_atomic_compare_exchange_u64(&t->last_sample_ts, &last, now))
	{
		uint32		running = (uint32) sift_pacer_running_backends(InvalidOid);
		SiftPaceLoad load;
		SiftPaceDecision d;

		if (now - (TimestampTz) pg_atomic_read_u64(&t->last_fg_ts) >= FG_IDLE_US)
			pg_atomic_write_u64(&t->fg_lat_ewma_us, pg_atomic_read_u64(&t->fg_lat_base_us));
		load = (SiftPaceLoad) {
			.active_oltp = running - (observer && running > 0),
			.cpu_slots = Max(2, max_parallel_workers),
			.lat_ewma_us = pg_atomic_read_u64(&t->fg_lat_ewma_us),
			.lat_base_us = pg_atomic_read_u64(&t->fg_lat_base_us),
			.lock_yields = observer ? pg_atomic_read_u32(&t->lock_yields) : pg_atomic_exchange_u32(&t->lock_yields, 0)
		};
		d = sift_pacer_decide(&load);
		if (d.mode == SIFT_PACE_THROTTLED && now < (TimestampTz) pg_atomic_read_u64(&t->defer_until))
			d.mode = SIFT_PACE_DEFERRED;
		if (d.mode < SIFT_PACE_THROTTLED)
		{
			pg_atomic_write_u64(&t->deferred_since, 0);
			pg_atomic_write_u64(&t->defer_until, 0);
		}
		pg_atomic_write_u32(&t->active_oltp, load.active_oltp);
		pg_atomic_write_u32(&t->share_pm, d.share_pm);
		pg_atomic_write_u32(&t->pacer_mode, d.mode);
		pg_atomic_write_u32(&t->eff_delay_ms, Max(0, sift_pacer_delay_ms(&d, SIFT_TIER_P1, sift_maintenance_cost_delay)));
	}
	return (SiftPaceDecision) {pg_atomic_read_u32(&t->pacer_mode), (int) pg_atomic_read_u32(&t->share_pm)};
}

void
sift_pacer_snapshot(SiftPacerSnapshot *out)
{
	SiftPacerTelemetry *t = telemetry();
	uint64		last = pg_atomic_read_u64(&t->last_sample_ts);

	if (last != 0 && GetCurrentTimestamp() - (TimestampTz) last >= 2 * SAMPLE_US)
		(void) current(t, MyBackendType == B_BACKEND);
	out->active_oltp = pg_atomic_read_u32(&t->active_oltp);
	out->mode = (SiftPacerMode) pg_atomic_read_u32(&t->pacer_mode);
	out->delay_ms = (int) pg_atomic_read_u32(&t->eff_delay_ms);
}

void
sift_pacer_record_foreground(int64 usecs)
{
	SiftPacerTelemetry *t = telemetry();
	uint64		ewma = pg_atomic_read_u64(&t->fg_lat_ewma_us);
	uint64		base = pg_atomic_read_u64(&t->fg_lat_base_us);

	pg_atomic_write_u64(&t->fg_lat_ewma_us, ewma == 0 ? (uint64) usecs : (ewma * 15 + usecs) >> 4);
	pg_atomic_write_u64(&t->fg_lat_base_us, base == 0 ? Max((uint64) usecs, LAT_FLOOR_US) : Max((base * 255 + Min((uint64) usecs, base * 2)) >> 8, LAT_FLOOR_US));
	pg_atomic_write_u64(&t->last_fg_ts, GetCurrentTimestamp());
}

void
sift_pacer_note_foreground(int64 usecs)
{
	static uint32 scans;

	if (++scans % FOREGROUND_EVERY == 0)
		sift_pacer_record_foreground(usecs);
}

void
sift_pacer_note_lock_yield(void)
{
	pg_atomic_fetch_add_u32(&telemetry()->lock_yields, 1);
}

void
sift_pacer_note_starved(SiftPacerTier tier, TimestampTz now)
{
	pg_atomic_write_u64(&telemetry()->starved_at[tier], now);
}

bool
sift_pacer_should_yield(SiftPacerTier tier, TimestampTz now)
{
	SiftPacerTelemetry *t = telemetry();
	int64		window = (int64) Max(sift_maintenance_naptime, 1000) * 1000;

	for (int higher = SIFT_TIER_P0; higher < (int) tier; higher++)
		if (now - (TimestampTz) pg_atomic_read_u64(&t->starved_at[higher]) < window)
			return true;
	return false;
}

static void
say_deferral(SiftPacerTelemetry *t, TimestampTz now)
{
	uint64		since = pg_atomic_read_u64(&t->deferred_since);
	uint64		logged = pg_atomic_read_u64(&t->defer_logged_at);
	bool		long_enough = since != 0 && now - (TimestampTz) since >= DEFER_LOG_AFTER_US &&
		now - (TimestampTz) logged >= DEFER_LOG_EVERY_US && pg_atomic_compare_exchange_u64(&t->defer_logged_at, &logged, now);

	if (since == 0)
		pg_atomic_write_u64(&t->deferred_since, now);
	ereport(long_enough ? LOG : DEBUG1,
			(errmsg("sift maintenance pacer defers background space and structural work (%u client backends running, a step of the worker sleeps %u ms)",
					pg_atomic_read_u32(&t->active_oltp), pg_atomic_read_u32(&t->eff_delay_ms))));
}

bool
sift_pacer_defer(SiftPacerTier tier)
{
	SiftPacerTelemetry *t = telemetry();
	SiftPaceDecision d;
	TimestampTz now = GetCurrentTimestamp();

	if (tier < SIFT_TIER_P2)
		return false;
	d = current(t, false);
	if (now >= (TimestampTz) pg_atomic_read_u64(&t->defer_until))
	{
		if (d.mode != SIFT_PACE_THROTTLED)
			return false;
		pg_atomic_write_u64(&t->defer_until, now + (int64) (DEFER_MIN_MS + (int64) DEFER_SPAN_MS * d.share_pm / 1000) * 1000);
	}
	pg_atomic_write_u32(&t->pacer_mode, SIFT_PACE_DEFERRED);
	say_deferral(t, now);
	return true;
}

void
sift_pacer_pause(SiftPacerTier tier)
{
	SiftPaceDecision d = current(telemetry(), false);
	int			ms = sift_pacer_delay_ms(&d, tier, sift_maintenance_cost_delay);

	if (ms < 0)
		ms = sift_pacer_delay_ms(&d, SIFT_TIER_P1, sift_maintenance_cost_delay);
	if (ms == 0)
		return;
#ifdef SIFT_TESTING
	elog(LOG, "sift test: throttle nap");	/* maint_cost_delay counts the naps of the worker: they follow from the pages it wrote, not from the clock */
#endif
	sift_wait(SIFT_WAIT_MAINT_PACE, ms);
}

void
sift_wt_pace_worker(SiftWriteTarget *wt)
{
	if (!sift_worker_paced())
		return;
	sift_wt_pace(wt, sift_worker_tier());
	sift_wt_wal_rate(wt, sift_maintenance_wal_rate);
}

void
sift_maint_ring_open(void)
{
	MemoryContext old;

	if (sift_maint_ring != NULL)
		return;
	/* MEM[MEM-IDLE-21]: cachebudget singleton maintenance ring ctx (1 KB; BAS_VACUUM strategy) */
	old = MemoryContextSwitchTo(sift_cache_create_context("sift cache maintenance ring"));
	sift_maint_ring = GetAccessStrategy(BAS_VACUUM);
	MemoryContextSwitchTo(old);
}

#ifdef SIFT_TESTING
void
sift_pacer_test_state(uint64 *ewma_us, uint64 *base_us)
{
	SiftPacerTelemetry *t = telemetry();

	*ewma_us = pg_atomic_read_u64(&t->fg_lat_ewma_us);
	*base_us = pg_atomic_read_u64(&t->fg_lat_base_us);
}

void
sift_pacer_test_reset(void)
{
	SiftPacerTelemetry *t = telemetry();

	SIFT_DSM_INIT_CALL(telemetry_init, t);
}
#endif
