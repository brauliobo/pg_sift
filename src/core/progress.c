/*
 * core/progress.c - see progress.h
 *
 * The slots are a table of the directory of the shared memory (col/dsmreg.c), made by the first job that reports, so a server that never runs a maintenance job has none.
 * A backend owns at most one slot, and clears it when its job ends, when its transaction aborts and when it exits; the layout is in the name of the table.
 */
#include "postgres.h"

#include "access/xact.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/dsmreg.h"
#include "core/progress.h"
#include "core/sql.h"
#include "core/store.h"
#include "core/visibility.h"

PG_FUNCTION_INFO_V1(sift_progress_rows);

#define PROGRESS_NAME	"sift_progress_1"
#define PROGRESS_SLOTS	64
#define PHASE_LEN		24

typedef struct ProgressSlot
{
	int			pid;			/* 0: free */
	Oid			db;
	Oid			index;
	int			job;
	char		phase[PHASE_LEN];
	int64		done;
	int64		total;
	uint64		bytes;
	TimestampTz started;
} ProgressSlot;

typedef struct Progress
{
	slock_t		mutex;
	ProgressSlot slot[PROGRESS_SLOTS];
} Progress;

static Progress *shared;
static bool unavailable;
static ProgressSlot *mine;
static bool hooked;

#ifdef SIFT_TESTING
int			sift_test_progress_gate = 0;
static int	phases_entered;
#endif

static void
progress_init(SIFT_DSM_INIT_ARGS)
{
	Progress   *p = ptr;

	memset(p, 0, sizeof(*p));
	SpinLockInit(&p->mutex);
}

static Progress *
table(void)
{
	if (shared == NULL && !unavailable)
		unavailable = (shared = sift_dsm_try(PROGRESS_NAME, sizeof(Progress), progress_init)) == NULL;
	return shared;
}

static void
release(void)
{
	if (mine == NULL)
		return;
	SIFT_TEST_MUTANT(1, return);
	SpinLockAcquire(&shared->mutex);
	memset(mine, 0, sizeof(*mine));
	SpinLockRelease(&shared->mutex);
	mine = NULL;
}

static void
progress_exit(int code, Datum arg)
{
	release();
}

static void
progress_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT)
		release();
}

/* The segments of the index a phase replaces: the immutable ones, and the sealed write buffers that a promotion turns into one each */
static int64
phase_total(Relation index, const char *phase)
{
	SiftDirSnapshot *snap;
	int64		n = 0;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
		n += strcmp(phase, "promote") == 0 ? snap->segs[i].kind == SIFT_SEG_WBUF_SEALED : snap->segs[i].kind == SIFT_SEG_IMMUTABLE;
	pfree(snap);
	return n;
}

bool
sift_progress_begin(Relation index, SiftJob job)
{
	Progress   *p;

	if (mine != NULL || (p = table()) == NULL)
		return false;
	SpinLockAcquire(&p->mutex);
	for (int i = 0; i < PROGRESS_SLOTS && mine == NULL; i++)
		if (p->slot[i].pid == 0)
		{
			mine = &p->slot[i];
			*mine = (ProgressSlot) {.pid = MyProcPid,.db = MyDatabaseId,.index = RelationGetRelid(index),.job = job,.started = GetCurrentTimestamp()};
		}
	SpinLockRelease(&p->mutex);
#ifdef SIFT_TESTING
	phases_entered = 0;
#endif
	if (mine != NULL && !hooked)
	{
		before_shmem_exit(progress_exit, (Datum) 0);
		RegisterXactCallback(progress_xact, NULL);
		hooked = true;
	}
	return mine != NULL;
}

void
sift_progress_phase(Relation index, const char *phase)
{
	int64		total;

	if (mine == NULL)
		return;
	total = phase_total(index, phase);
	SpinLockAcquire(&shared->mutex);
	strlcpy(mine->phase, phase, PHASE_LEN);
	mine->done = 0;
	mine->total = total;
	SpinLockRelease(&shared->mutex);
#ifdef SIFT_TESTING
	if (sift_test_progress_gate > 0 && ++phases_entered == sift_test_progress_gate && MyBackendType == B_BG_WORKER)
		sift_test_wait(12);
#endif
}

void
sift_progress_step(int64 done, uint64 bytes)
{
	if (mine == NULL)
		return;
	SpinLockAcquire(&shared->mutex);
	mine->done += done;
	mine->bytes += bytes;
	SpinLockRelease(&shared->mutex);
}

void
sift_progress_abort(void)
{
	release();
}

void
sift_progress_end(bool began)
{
	if (began)
		release();
}

/* sift._progress(): the jobs of the database that run now (the view sift.progress shows the ones of indexes the caller may look at) */
Datum
sift_progress_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Progress   *p;
	ProgressSlot copy[PROGRESS_SLOTS];
	int			n = 0;

	InitMaterializedSRF(fcinfo, 0);
	if ((p = sift_dsm_find(PROGRESS_NAME, NULL)) == NULL)
		return (Datum) 0;
	SpinLockAcquire(&p->mutex);
	for (int i = 0; i < PROGRESS_SLOTS; i++)
		if (p->slot[i].pid != 0 && p->slot[i].db == MyDatabaseId)
			copy[n++] = p->slot[i];
	SpinLockRelease(&p->mutex);
	for (int i = 0; i < n; i++)
	{
		Datum		v[8];
		bool		nulls[8] = {false};

		v[0] = Int32GetDatum(copy[i].pid);
		v[1] = ObjectIdGetDatum(copy[i].index);
		v[2] = CStringGetTextDatum(sift_job_name(copy[i].job));
		nulls[3] = copy[i].phase[0] == '\0';
		v[3] = CStringGetTextDatum(copy[i].phase);
		v[4] = Int64GetDatum(copy[i].done);
		v[5] = Int64GetDatum(copy[i].total);
		v[6] = Int64GetDatum((int64) copy[i].bytes);
		v[7] = TimestampTzGetDatum(copy[i].started);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}
