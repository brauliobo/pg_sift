/*
 * core/maint/sched_pg.c - the server side of maint_sched.h: the table in the dynamic shared memory registry, the host of the queue (clock, pid
 * liveness, dynamic worker start, maint_pacer.c) and the entry points of the launcher and the workers. Not built by the standalone test.
 */
#include "core/maint/sched.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "datatype/timestamp.h"
#include "utils/timestamp.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"

#include "col/dsmreg.h"
#include "core/compat.h"
#include "core/f2/pace.h"
#include "core/waits.h"
#include "core/worker.h"

#define TABLE_NAME		"sift_maint_v6"

static struct
{
	bool		async;
	int			max_workers;
	int			naptime_ms;
}			cfg = {true, 2, 2000};

static SiftMaintShmem *table;
static bool table_tried;
static SiftMaintStepFn stepper;
static bool launcher_registered;

StaticAssertDecl(sizeof(int) <= BGW_EXTRALEN, "bgw_extra holds the slot of a worker");

static void
table_init(SIFT_DSM_INIT_ARGS)
{
	int			tranche = sift_tranche_new("SiftMaintShmem");

	sift_maint_shmem_init(ptr, tranche, cfg.max_workers, cfg.naptime_ms);
}

/* The first backend of the cluster makes the table; where the registry has no room the backend goes without and asks again only after sift_maint_configure */
SiftMaintShmem *
sift_maint_table(void)
{
	if (table == NULL && !table_tried)
	{
		table_tried = true;
		/* MEM[MEM-SHM-06]: DSM maintenance queue sift_maint_v6 (fixed; 8192x128 B slots) */
		if ((table = sift_dsm_try(TABLE_NAME, sift_maint_shmem_size(), table_init)) != NULL)
		{
			sift_tranche_register(table->tranche, "SiftMaintShmem");
			sift_standby_horizon_attach(&table->horizon);
		}
	}
	return table;
}

void
sift_maint_configure(bool async_maintenance, int max_workers, int naptime_ms)
{
	cfg.async = async_maintenance;
	cfg.max_workers = max_workers;
	cfg.naptime_ms = naptime_ms;
	if (table != NULL)
		sift_maint_set_limits(table, max_workers, naptime_ms);
}

void
sift_maint_assign_async(bool newval, void *extra pg_attribute_unused())
{
	sift_maint_configure(newval, cfg.max_workers, cfg.naptime_ms);
}

void
sift_maint_assign_max_workers(int newval, void *extra pg_attribute_unused())
{
	sift_maint_configure(cfg.async, newval, cfg.naptime_ms);
}

void
sift_maint_assign_naptime(int newval, void *extra pg_attribute_unused())
{
	sift_maint_configure(cfg.async, cfg.max_workers, newval);
}

void
sift_maint_set_stepper(SiftMaintStepFn step)
{
	stepper = step;
}

static TimestampTz
host_now(void)
{
	return GetCurrentTimestamp();
}

static int
host_self_pid(void)
{
	return MyProcPid;
}

static bool
host_pid_alive(int pid)
{
	return BackendPidGetProc(pid) != NULL;
}

static void
host_wake(int procno)
{
	sift_shm_wake(procno);
}

static bool
host_spawn(Oid db, int worker)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;

	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
#ifdef BGWORKER_INTERRUPTIBLE
	bw.bgw_flags |= BGWORKER_INTERRUPTIBLE;
#endif
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_maint_sched_worker_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift maintenance of database %u", db);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance");
	bw.bgw_main_arg = ObjectIdGetDatum(db);
	memcpy(bw.bgw_extra, &worker, sizeof(worker));
	return RegisterDynamicBackgroundWorker(&bw, &handle);
}

static bool
host_defer_cold(void)
{
	return sift_pacer_defer(SIFT_TIER_P2);
}

static void
host_note_starved(SiftPacerTier tier, TimestampTz now)
{
	sift_pacer_note_starved(tier, now);
}

static bool
host_should_yield(SiftPacerTier tier, TimestampTz now)
{
	return sift_pacer_should_yield(tier, now);
}

/* A step is one unit of the pace of its tier, whatever it wrote: the steps that a stream of triggers asks for do not take the CPU of the clients */
static void
host_pace(SiftPacerTier tier pg_attribute_unused())
{
	sift_f2_pace_pages(SIFT_F2_PACE_UNIT_PAGES);
	sift_f2_pace_pay();
}

static int
host_other_workers(void)
{
	return sift_worker_legacy_active();
}

/* The bgworkers that are running, but the launcher of the queue: it is ours, and the cap on workers counts the ones that are not */
static int
host_bgworkers_in_use(void)
{
	uint32		launcher = pg_atomic_read_u32(&table->launcher);
	int			launcher_pid = launcher != 0 ? GetPGProcByNumber(launcher - 1)->pid : 0;
	int			n = 0;

	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *s = &pgstat_get_local_beentry_by_index(i)->backendStatus;

		n += s->st_backendType == B_BG_WORKER && s->st_procpid != launcher_pid;
	}
	return n;
}

static int server_effective_max(SiftMaintShmem *m);

const SiftMaintHost sift_maint_server_host = {
	.now = host_now, .self_pid = host_self_pid, .pid_alive = host_pid_alive, .spawn = host_spawn, .wake = host_wake,
	.defer_cold = host_defer_cold, .note_starved = host_note_starved, .should_yield = host_should_yield, .pace = host_pace,
	.bgworkers_in_use = host_bgworkers_in_use, .other_workers = host_other_workers, .effective_max = server_effective_max,
};

/* The bgworkers that are not ours are counted by a walk over every backend: once a second is soon enough for a soft cap (RegisterDynamicBackgroundWorker enforces the hard one) */
#define SIFT_BGW_SAMPLE_TTL_US	((int64) 1 * USECS_PER_SEC)

static int
cached_other_bgworkers(SiftMaintShmem *m)
{
	TimestampTz now = GetCurrentTimestamp();
	TimestampTz at = (TimestampTz) pg_atomic_read_u64(&m->bgw_sample_ts_us);
	int			other;

	pg_read_barrier();
	if (at != 0 && now >= at && now - at < SIFT_BGW_SAMPLE_TTL_US)
		return (int) pg_atomic_read_u32(&m->bgw_other_cached);
	other = Max(0, host_bgworkers_in_use() - (int) pg_atomic_read_u32(&m->active_workers));
	pg_atomic_write_u32(&m->bgw_other_cached, (uint32) other);
	pg_write_barrier();
	pg_atomic_write_u64(&m->bgw_sample_ts_us, (uint64) now);
	return other;
}

static int
server_effective_max(SiftMaintShmem *m)
{
	return Min(sift_maint_effective_max(cfg.max_workers, max_worker_processes, cached_other_bgworkers(m)), cfg.max_workers - host_other_workers());
}

int
sift_maint_active_workers(void)
{
	SiftMaintShmem *m = sift_maint_table();

	return m != NULL ? (int) pg_atomic_read_u32(&m->active_workers) : 0;
}

bool
sift_maint_here_available(void)
{
	return sift_maint_mode(cfg.async, launcher_registered, sift_maint_table() != NULL, cfg.max_workers) != SIFT_MAINT_MODE_OFF;
}

/* The entry of every trigger (a commit, a delete, a scan that finds maintenance due) */
SiftMaintEnq
sift_maint_enqueue_here(const SiftMaintReq *req)
{
	SiftMaintShmem *m = sift_maint_table();
	SiftMaintMode mode = sift_maint_mode(cfg.async, launcher_registered, m != NULL, cfg.max_workers);

	if (mode == SIFT_MAINT_MODE_OFF)
		return SIFT_ENQ_UNAVAILABLE;
	return sift_maint_request(m, &sift_maint_server_host, mode, req);
}

Relation
sift_maint_open(Oid relid, LOCKMODE lock)
{
	for (;;)
	{
		Oid			heapoid = IndexGetRelation(relid, true);
		Relation	rel;

		SIFT_POINT("sift-maint-f2-before-table-lock");
		if (OidIsValid(heapoid))
			LockRelationOid(heapoid, lock);
		SIFT_POINT("sift-maint-f2-after-table-lock");
		rel = try_relation_open(relid, lock);
		if (rel == NULL || !OidIsValid(heapoid) || rel->rd_index->indrelid == heapoid)
			return rel;
		relation_close(rel, lock);
	}
}

static Oid	worker_db;
static bool worker_connected;
static volatile sig_atomic_t worker_terminated;

/* SIGTERM of the administrator or of a statement that drops or copies the database: the worker ends and its claim goes back to the queue as one that never ran */
static void
worker_term(SIGNAL_ARGS)
{
	worker_terminated = true;
#if PG_VERSION_NUM >= 190000
	die(postgres_signal_arg, pg_siginfo);
#else
	die(postgres_signal_arg);
#endif
}

static void
worker_exit_cb(int code, Datum arg)
{
	int			worker = DatumGetInt32(arg);

	if (!worker_connected)
	{
		sift_maint_worker_unconnected(table, &sift_maint_server_host, worker, worker_db);
		return;
	}
	sift_maint_release_pid(table, &sift_maint_server_host, MyProcPid, code != 0 && !worker_terminated);
	sift_maint_worker_exit(table, worker, MyProcPid);
	if (code == 0)
		sift_maint_kick(table, &sift_maint_server_host);
}

static void
launcher_exit_cb(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	pg_atomic_write_u32(&table->launcher, 0);
}

static void
nap(long ms)
{
	(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, ms, sift_wait_event(SIFT_WAIT_NAP));
	ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

/* Until a request, a reload or a signal sets the latch: the launcher of a table with nothing in it has no timer to keep */
static void
sleep_until_woken(void)
{
	(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1, sift_wait_event(SIFT_WAIT_NAP));
	ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

/* A reload of the configuration (pg_reload_conf) reaches the settings of the scheduler and of the pacer in the processes that run it, between two of their rounds */
static void
reload_config(void)
{
	if (!ConfigReloadPending)
		return;
	ConfigReloadPending = false;
	ProcessConfigFile(PGC_SIGHUP);
}

/* The pages a step reads and writes are paced at the tier of its claim (core/f2/pace.c) */
static SiftMaintStep
paced_step(const SiftMaintClaim *claim, void *arg)
{
	sift_worker_queue_tier(claim->tier);
	return stepper(claim, arg);
}

/* An ERROR of a step ends the transaction of the worker and hands its claim back as a failed step: the worker lives on */
static int
run_guarded(SiftMaintShmem *m, Oid db, bool *yielded)
{
	int			n = 0;

	PG_TRY();
	{
		n = sift_maint_run(m, &sift_maint_server_host, db, 16, paced_step, NULL, yielded);
	}
	PG_CATCH();
	{
		EmitErrorReport();
		FlushErrorState();
		AbortCurrentTransaction();
		sift_f2_pace_reset();
		sift_maint_release_pid(m, &sift_maint_server_host, MyProcPid, true);
	}
	PG_END_TRY();
	return n;
}

/* A dynamic worker drains the highest priority requests of its database and leaves after SIFT_MAINT_IDLE_NAPS naps with nothing to do and nothing due soon */
void
sift_maint_sched_worker_main(Datum main_arg)
{
	Oid			db = DatumGetObjectId(main_arg);
	SiftMaintShmem *m;
	bool		yielded = false;
	int			worker;

	memcpy(&worker, MyBgworkerEntry->bgw_extra, sizeof(worker));
	worker_db = db;
	pqsignal(SIGTERM, worker_term);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	if ((m = sift_maint_table()) == NULL)
		proc_exit(0);
	before_shmem_exit(worker_exit_cb, Int32GetDatum(worker));
	BackgroundWorkerInitializeConnectionByOid(db, InvalidOid, 0);
	worker_connected = true;
	sift_worker_queue_enter();
	sift_maint_db_connected(m, db);
	if (!sift_maint_worker_started(m, worker, MyProcPid, MyProcNumber))
		proc_exit(0);
	Assert(stepper != NULL);
	for (int idle = 0; idle < SIFT_MAINT_IDLE_NAPS && !yielded;)
	{
		reload_config();
		idle = run_guarded(m, db, &yielded) > 0 || sift_maint_due_soon(m, &sift_maint_server_host, db) ? 0 : idle + 1;
		sift_maint_reap(m, &sift_maint_server_host);
		if (idle < SIFT_MAINT_IDLE_NAPS && !yielded)
			nap(m->naptime_ms);
	}
	proc_exit(0);
}

void
sift_maint_launcher_register(void)
{
	BackgroundWorker bw = {0};

	launcher_registered = true;
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = 10;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_maint_sched_launcher_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift maintenance launcher");
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance launcher");
	RegisterBackgroundWorker(&bw);
}

/*
 * Sleeps on its latch with no timer while the queue is empty and no worker runs: it looks at nothing, so it counts nothing; a request or a reload wakes it, and it starts the worker of the
 * database that has the best request waiting. Whether a request is queued is the setting of the session that asks (sift.async_maintenance), not the launcher's
 */
void
sift_maint_sched_launcher_main(Datum main_arg pg_attribute_unused())
{
	SiftMaintShmem *m;

	pqsignal(SIGTERM, die);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	if ((m = sift_maint_table()) == NULL)
		proc_exit(0);
	pg_atomic_write_u32(&m->launcher, MyProcNumber + 1);
	before_shmem_exit(launcher_exit_cb, 0);
	for (;;)
	{
		reload_config();
		if (sift_maint_idle(m))
		{
			sleep_until_woken();
			continue;
		}
		while (sift_maint_kick(m, &sift_maint_server_host) == SIFT_KICK_SPAWNED)
			;
		nap(m->naptime_ms);
	}
}
