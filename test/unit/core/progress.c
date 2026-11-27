/*
 * test/unit/core/progress.c - the slot of sift.progress a backend takes for a maintenance job (core/progress.c): given back when the subtransaction that began the job aborts (a job run under
 * a SAVEPOINT or an EXCEPTION block that fails), kept when a subtransaction inside the job aborts or the one that began it commits, given back with the transaction; the clock is read outside the
 * spinlock of the slots.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "core/progress.c"

#include "test/unit/core/shm/stub_lwlock.h"

int			MyProcPid = 4242;
Oid			MyDatabaseId = 5;
BackendType MyBackendType = B_BACKEND;

static SubTransactionId current_subid = TopSubTransactionId;
static XactCallback xact_cb;
static SubXactCallback subxact_cb;

void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	return stub_segment(size, init);
}

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
}

void
RegisterXactCallback(XactCallback callback, void *arg)
{
	xact_cb = callback;
}

void
RegisterSubXactCallback(SubXactCallback callback, void *arg)
{
	subxact_cb = callback;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return current_subid;
}

TimestampTz
GetCurrentTimestamp(void)
{
	SIFT_CHECKF(shared == NULL || SpinLockFree(&shared->mutex), "the clock is read under the spinlock of the slots");
	return 1;
}

static void
subxact(SubXactEvent event, SubTransactionId sub, SubTransactionId parent)
{
	if (subxact_cb != NULL)
		subxact_cb(event, sub, parent, NULL);
}

static int
slots_held(void)
{
	int			n = 0;

	for (int i = 0; i < PROGRESS_SLOTS; i++)
		n += shared->slot[i].pid == MyProcPid;
	return n;
}

static RelationData index_rel = {.rd_id = 16384};

/* A job begun in subtransaction 2 that aborts: the slot is given back, and the next job of the transaction takes one */
static void
test_subxact_abort_frees_slot(void)
{
	current_subid = 2;
	SIFT_CHECKF(sift_progress_begin(&index_rel, SIFT_JOB_MERGE), "the job takes a slot");
	subxact(SUBXACT_EVENT_ABORT_SUB, 2, TopSubTransactionId);
	SIFT_CHECKF(slots_held() == 0 && mine == NULL, "the job of a subtransaction that aborted holds its slot until the transaction ends (%d held)", slots_held());
	current_subid = TopSubTransactionId;
	SIFT_CHECKF(sift_progress_begin(&index_rel, SIFT_JOB_MERGE), "the next job of the transaction takes a slot");
	sift_progress_end(true);
}

/* A subtransaction inside the job (an attach of a table, a check of its own) that aborts, or the one that began the job committing into its parent, keeps the slot; the abort of the parent gives it back */
static void
test_subxact_keeps_slot(void)
{
	sift_progress_abort();
	current_subid = 3;
	SIFT_CHECKF(sift_progress_begin(&index_rel, SIFT_JOB_VACUUM), "the job takes a slot");
	subxact(SUBXACT_EVENT_ABORT_SUB, 4, 3);
	SIFT_CHECKF(slots_held() == 1, "a subtransaction inside the job that aborted took the slot of the job");
	subxact(SUBXACT_EVENT_COMMIT_SUB, 3, 2);
	SIFT_CHECKF(slots_held() == 1, "the subtransaction that began the job committed and the slot went");
	subxact(SUBXACT_EVENT_ABORT_SUB, 2, TopSubTransactionId);
	SIFT_CHECKF(slots_held() == 0, "the abort of the subtransaction the job went to keeps the slot");
	sift_progress_abort();
	current_subid = TopSubTransactionId;
	SIFT_CHECKF(sift_progress_begin(&index_rel, SIFT_JOB_VACUUM), "the job takes a slot");
	xact_cb(XACT_EVENT_ABORT, NULL);
	SIFT_CHECKF(slots_held() == 0, "the abort of the transaction gives the slot back");
}

int
main(void)
{
	test_subxact_abort_frees_slot();
	test_subxact_keeps_slot();
	return sift_check_summary();
}
