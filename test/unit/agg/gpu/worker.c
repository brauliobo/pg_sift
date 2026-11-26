/*
 * test/unit/agg/gpu/worker.c - the slots of the GPU worker that a backend claims (agg/gpu/worker.c): a backend that finds every slot busy gives the job back to the CPU after a short wait, not
 * after the 30 s of a statement held up, and a result the backend cannot copy (an allocation that fails) leaves the slot free for the other backends.
 */
#include "postgres.h"

#include "agg/gpu/worker.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

volatile sig_atomic_t InterruptPending = false;
sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;
int			MyProcPid;
ProcNumber	MyProcNumber = 0;
static Latch latch;
struct Latch *MyLatch = &latch;
static PGPROC procs[1];
static PROC_HDR proc_hdr = {.allProcs = procs};
PROC_HDR   *ProcGlobal = &proc_hdr;
int			sift_gpu_mode;
int			sift_gpu_cache_mb;
char	   *sift_gpu_device = "";

static SiftGpuShm *shm;
static uint64 waited_ms;
static int	answer_slot = -1;

TimestampTz
GetCurrentTimestamp(void)
{
	return 1000;
}

/* The worker answers the job of answer_slot with a result larger than any palloc */
int
WaitLatch(Latch *l, int events, long timeout, uint32 wait_event_info)
{
	waited_ms += (uint64) timeout;
	if (answer_slot >= 0 && pg_atomic_read_u32(&shm->slot[answer_slot].state) == SIFT_GPU_S_SUBMITTED)
	{
		shm->slot[answer_slot].status = SIFT_VK_OK;
		shm->slot[answer_slot].acc_bytes = MaxAllocSize + 1;
		pg_atomic_write_u32(&shm->slot[answer_slot].state, SIFT_GPU_S_DONE);
	}
	return WL_TIMEOUT;
}

void
ResetLatch(Latch *l)
{
}

void
SetLatch(Latch *l)
{
}

void
ProcessInterrupts(void)
{
}

/* An error unwinds through the PG_TRY blocks of the code, to the one of the test */
void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
throw_error(void)
{
	pg_re_throw();
}

static void
fresh_shm(void)
{
	memset(shm, 0, sizeof(SiftGpuShm));
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_READY);
	pg_atomic_write_u32(&shm->worker_pid, (uint32) MyProcPid);
	pg_atomic_write_u64(&shm->heartbeat_us, 1000);
}

static void
test_all_slots_busy(void)
{
	fresh_shm();
	for (int i = 0; i < SIFT_GPU_SLOTS; i++)
	{
		pg_atomic_write_u32(&shm->slot[i].state, SIFT_GPU_S_RUNNING);
		shm->slot[i].owner_pid = MyProcPid;
	}
	waited_ms = 0;
	SIFT_CHECK(slot_claim(shm) == -1);
	SIFT_CHECK(waited_ms <= 20);
}

static void
test_result_alloc_fails(void)
{
	static SiftGpuConn conn;
	static uint32 ops[SIFT_VK_OP_WORDS];
	static SiftVkJob job = {.ops = ops,.nops = 1};
	static SiftVkResult res;
	volatile bool failed = false;

	fresh_shm();
	conn.shm = shm;
	conn.rcx = CurrentMemoryContext;
	answer_slot = 0;
	pgshim_error_hook = throw_error;
	PG_TRY();
	{
		(void) remote_run(&conn, &job, &res);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	pgshim_error_hook = NULL;
	answer_slot = -1;
	SIFT_CHECK(failed);
	SIFT_CHECK(pg_atomic_read_u32(&shm->slot[0].state) == SIFT_GPU_S_FREE);
	SIFT_CHECK(slot_claim(shm) == 0);
}

int
main(void)
{
	MyProcPid = getpid();
	shm = aligned_alloc(64, TYPEALIGN(64, SIFT_GPU_SHM_BYTES));
	SIFT_CHECK(shm != NULL);
	pgshim_keep();
	if (unit_wants("gpu_slots_busy"))
		test_all_slots_busy();
	if (unit_wants("gpu_result_alloc"))
		test_result_alloc_fails();
	free(shm);
	puts("gpu_worker: ok");
	return 0;
}
