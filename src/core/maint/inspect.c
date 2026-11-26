/*
 * core/maint/inspect.c - sift_internal.maint_queue() and sift_internal.maint_pacer(): the support of sift.activity. The first lists the jobs of the background maintenance that run and wait in this
 * database, from a coherent copy of the queue (core/maint/sched.h, sift_maint_snapshot: no lock of the writers is held); the second says what the workers are doing to the server: how many run,
 * how many may, and the pace the pacer last decided (core/maint/pacer.h, sift_pacer_snapshot).
 *
 * The pace is what a maintenance worker last looked at (every 250 ms while one runs): FULL, 0 ms and 0 backends before the first look, and the values of the last one when no worker has run since.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "core/inspect.h"
#include "core/maint/pacer.h"
#include "core/maint/sched.h"
#include "core/sql/sql.h"
#include "core/sql/api.h"

PG_FUNCTION_INFO_V1(sift_maint_queue);
PG_FUNCTION_INFO_V1(sift_maint_pacer);

/* columns of sift_internal.maint_queue, in the order of sql/6.0/15-unified-api.sql */
enum QueueCol
{
	MQ_PID,
	MQ_RELATION,
	MQ_PART_ID,
	MQ_JOB,
	MQ_PHASE,
	MQ_PRIORITY,
	MQ_DONE,
	MQ_TOTAL,
	MQ_BYTES_WRITTEN,
	MQ_STARTED_AT,
	MQ_QUEUED,
	MQ_NCOLS
};

static const Oid queue_types[MQ_NCOLS] = {
	INT4OID, OIDOID, INT8OID, TEXTOID, TEXTOID, INT4OID, INT8OID, INT8OID, INT8OID, TIMESTAMPTZOID, BOOLOID
};

/* columns of sift_internal.maint_pacer, in the order of sql/6.0/15-unified-api.sql */
enum PacerCol
{
	MP_WORKERS_ACTIVE,
	MP_WORKERS_MAX,
	MP_PACER_MODE,
	MP_PACER_DELAY_MS,
	MP_OLTP_ACTIVE_BACKENDS,
	MP_NCOLS
};

static const Oid pacer_types[MP_NCOLS] = {INT4OID, INT4OID, TEXTOID, INT4OID, INT4OID};

static const char *const pacer_mode_names[] = {
	[SIFT_PACE_FULL] = "full",
	[SIFT_PACE_MODERATE] = "moderate",
	[SIFT_PACE_THROTTLED] = "throttled",
	[SIFT_PACE_DEFERRED] = "deferred"
};

/* The queue as the server holds it, or nothing where the registry had no room for it */
static int
queue_copy(SiftMaintRow *rows, SiftMaintStatus *status)
{
	SiftMaintShmem *m = sift_maint_table();

	memset(status, 0, sizeof(*status));
	return m == NULL ? 0 : sift_maint_snapshot(m, &sift_maint_server_host, rows, status);
}

Datum
sift_maint_queue(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftMaintRow *rows = palloc(SIFT_MAINT_SLOTS * sizeof(SiftMaintRow));
	SiftMaintStatus status;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, queue_types, MQ_NCOLS);
	n = queue_copy(rows, &status);
	for (int i = 0; i < n; i++)
	{
		const SiftMaintRow *q = &rows[i];
		Datum		v[MQ_NCOLS] = {0};
		bool		nulls[MQ_NCOLS] = {false};

		if (q->db != MyDatabaseId || !sift_may_inspect_rel(q->rel))
			continue;
		nulls[MQ_PID] = q->queued;
		v[MQ_PID] = Int32GetDatum(q->pid);
		v[MQ_RELATION] = ObjectIdGetDatum(q->rel);
		v[MQ_PART_ID] = Int64GetDatum((int64) q->part_id);
		v[MQ_JOB] = CStringGetTextDatum(sift_maint_job_name(q->kind, q->tier));
		v[MQ_PHASE] = CStringGetTextDatum(sift_maint_phase_name(q->phase));
		v[MQ_PRIORITY] = Int32GetDatum((int32) q->priority);
		v[MQ_DONE] = Int64GetDatum((int64) q->done);
		v[MQ_TOTAL] = Int64GetDatum((int64) q->total);
		v[MQ_BYTES_WRITTEN] = Int64GetDatum((int64) q->bytes_written);
		v[MQ_STARTED_AT] = TimestampTzGetDatum(q->started_at);
		v[MQ_QUEUED] = BoolGetDatum(q->queued);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}

Datum
sift_maint_pacer(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	SiftMaintRow *rows = palloc(SIFT_MAINT_SLOTS * sizeof(SiftMaintRow));
	SiftMaintStatus status;
	SiftPacerSnapshot pace;
	Datum		v[MP_NCOLS] = {0};
	bool		nulls[MP_NCOLS] = {false};
	int			others;
	TypeFuncClass kind = get_call_result_type(fcinfo, NULL, &desc);

	SIFT_INVARIANT(kind == TYPEFUNC_COMPOSITE, "sift_internal.maint_pacer must return a row");
	sift_api_check_desc(desc, pacer_types, MP_NCOLS);
	if (!sift_may_read_stats())
	{
		memset(nulls, true, sizeof(nulls));
		return sift_result_row(fcinfo, v, nulls);
	}
	(void) queue_copy(rows, &status);
	sift_pacer_snapshot(&pace);
	others = Max(0, sift_maint_server_host.bgworkers_in_use() - (int) status.workers_active);
	v[MP_WORKERS_ACTIVE] = Int32GetDatum((int32) status.workers_active);
	v[MP_WORKERS_MAX] = Int32GetDatum(sift_maint_effective_max((int) status.workers_max, max_worker_processes, others));
	v[MP_PACER_MODE] = CStringGetTextDatum(pacer_mode_names[pace.mode]);
	v[MP_PACER_DELAY_MS] = Int32GetDatum(pace.delay_ms);
	v[MP_OLTP_ACTIVE_BACKENDS] = Int32GetDatum((int32) pace.active_oltp);
	return sift_result_row(fcinfo, v, nulls);
}
