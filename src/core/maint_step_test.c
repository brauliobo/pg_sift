/*
 * core/maint_step_test.c - test-only SQL functions of the queue of the background maintenance (core/maint_sched.h), compiled only with SIFT_TESTING and registered by the pg_sift_test extension:
 * sift._test_maint_enqueue(index, kind) puts a request in the queue as a trigger does and says what became of it, sift._test_maint_stats() reads the counters of the queue.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "miscadmin.h"

#include "core/maint_sched.h"
#include "core/sql.h"
#include "core/worker.h"

PG_FUNCTION_INFO_V1(sift_test_maint_enqueue);
PG_FUNCTION_INFO_V1(sift_test_maint_stats);
PG_FUNCTION_INFO_V1(sift_test_worker_slots);
PG_FUNCTION_INFO_V1(sift_test_maint_workers_hold);
PG_FUNCTION_INFO_V1(sift_test_maint_workers);

static const char *const enq_names[] = {
	[SIFT_ENQ_QUEUED] = "queued",
	[SIFT_ENQ_COALESCED] = "coalesced",
	[SIFT_ENQ_EVICTED] = "evicted",
	[SIFT_ENQ_FULL] = "full",
	[SIFT_ENQ_UNAVAILABLE] = "unavailable"
};

Datum
sift_test_maint_enqueue(PG_FUNCTION_ARGS)
{
	SiftMaintReq req = {.db = MyDatabaseId,.rel = PG_GETARG_OID(0),.kind = PG_GETARG_INT32(1),.tier = SIFT_MAINT_TIER_DEFAULT};

	if (req.kind < 0 || req.kind >= SIFT_MK_N)
		elog(ERROR, "invalid kind of maintenance");
	PG_RETURN_TEXT_P(cstring_to_text(enq_names[sift_maint_enqueue_here(&req)]));
}

Datum
sift_test_maint_stats(PG_FUNCTION_ARGS)
{
	SiftMaintShmem *m = sift_maint_table();
	SiftMaintRow *rows = palloc(SIFT_MAINT_SLOTS * sizeof(SiftMaintRow));
	SiftMaintStatus st = {0};
	Datum		v[5];
	bool		n[5] = {0};

	if (m != NULL)
		(void) sift_maint_snapshot(m, &sift_maint_server_host, rows, &st);
	v[0] = Int64GetDatum(st.stats.enqueued);
	v[1] = Int64GetDatum(st.stats.coalesced);
	v[2] = Int64GetDatum(st.stats.completed);
	v[3] = Int64GetDatum(st.stats.spawned);
	v[4] = Int32GetDatum(st.queued[0] + st.queued[1] + st.queued[2] + st.queued[3] + st.claimed);
	return sift_result_row(fcinfo, v, n);
}

Datum
sift_test_worker_slots(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(sift_worker_test_hold(PG_GETARG_INT32(0)));
}

/* Holds n workers of the queue as running ones of this backend (0: gives them back): the number held */
Datum
sift_test_maint_workers_hold(PG_FUNCTION_ARGS)
{
	static int	held[SIFT_MAINT_MAX_WORKERS];
	static int	nheld;
	SiftMaintShmem *m = sift_maint_table();
	int			n = PG_GETARG_INT32(0);

	if (m == NULL)
		elog(ERROR, "the table of the queue is not there");
	while (nheld > 0)
		sift_maint_worker_exit(m, held[--nheld], MyProcPid);
	while (nheld < n && nheld < SIFT_MAINT_MAX_WORKERS && (held[nheld] = sift_maint_worker_reserve(m, &sift_maint_server_host, MyDatabaseId)) >= 0)
		(void) sift_maint_worker_started(m, held[nheld++], MyProcPid, MyProcNumber);
	PG_RETURN_INT32(nheld);
}

Datum
sift_test_maint_workers(PG_FUNCTION_ARGS)
{
	Datum		v[2] = {Int32GetDatum(sift_worker_legacy_active()), Int32GetDatum(sift_maint_active_workers())};
	bool		n[2] = {0};

	return sift_result_row(fcinfo, v, n);
}

#endif							/* SIFT_TESTING */
