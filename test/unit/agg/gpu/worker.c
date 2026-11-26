/*
 * test/unit/agg/gpu/worker.c - the slots of the GPU worker that a backend claims (agg/gpu/worker.c): a backend that finds every slot busy gives the job back to the CPU after a short wait, not
 * after the 30 s of a statement held up, and a result the backend cannot copy (an allocation that fails) leaves the slot free for the other backends. A backend that left acts only on
 * the generation of the slot it claimed (agg/gpu/slot.c): it frees no other backend's answer, and no backend takes a live slot for the pid of the owner before it.
 */
#include "postgres.h"

#include <sys/wait.h>

int			test_kill(pid_t pid, int sig);
#define kill test_kill
#include "agg/gpu/worker.c"
#include "agg/gpu/slot.c"
#undef kill

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
static void (*kill_hook) (void);

/* The other backends and the worker run at the next probe of a pid: the window between a read of the slot and what is done with it */
int
test_kill(pid_t pid, int sig)
{
	void		(*hook) (void) = kill_hook;

	kill_hook = NULL;
	if (hook != NULL)
		hook();
	return kill(pid, sig);
}

static pid_t
dead_pid(void)
{
	pid_t		pid = fork();

	if (pid == 0)
		_exit(0);
	SIFT_CHECK(pid > 0 && waitpid(pid, NULL, 0) == pid);
	return pid;
}

TimestampTz
GetCurrentTimestamp(void)
{
	return 1000;
}

static SiftGpuSlotState
slot_state(int i)
{
	return sift_gpu_slot_state(sift_gpu_slot_read(&shm->slot[i]));
}

static uint32
slot_gen(int i)
{
	return sift_gpu_slot_gen(sift_gpu_slot_read(&shm->slot[i]));
}

/* The worker answers the job of answer_slot with a result larger than any palloc */
int
WaitLatch(Latch *l, int events, long timeout, uint32 wait_event_info)
{
	waited_ms += (uint64) timeout;
	if (answer_slot >= 0 && slot_state(answer_slot) == SIFT_GPU_S_SUBMITTED)
	{
		shm->slot[answer_slot].status = SIFT_VK_OK;
		shm->slot[answer_slot].acc_bytes = MaxAllocSize + 1;
		sift_gpu_slot_finish(&shm->slot[answer_slot], slot_gen(answer_slot), SIFT_GPU_S_SUBMITTED);
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
	uint32		gen;

	fresh_shm();
	for (int i = 0; i < SIFT_GPU_SLOTS; i++)
	{
		sift_gpu_slot_set(&shm->slot[i], 1, SIFT_GPU_S_RUNNING);
		shm->slot[i].owner_pid = MyProcPid;
	}
	waited_ms = 0;
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == -1);
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
	uint32		gen;

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
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_FREE);
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
}

/* The worker takes the job of slot i, as worker_loop does: a job whose backend left is dropped */
static void
worker_take(int i)
{
	SiftGpuSlot *s = &shm->slot[i];
	const uint32 gen = slot_gen(i);

	SIFT_CHECK(sift_gpu_slot_move(s, gen, SIFT_GPU_S_SUBMITTED, SIFT_GPU_S_RUNNING));
	if (sift_gpu_slot_cancelled(s, gen))
		SIFT_CHECK(sift_gpu_slot_move(s, gen, SIFT_GPU_S_RUNNING, SIFT_GPU_S_FREE));
}

/* B1 is cancelled before the worker took its job; before it frees the slot, the worker drops the job, B2 claims the slot and its job is done: B1 must leave B2's answer alone */
static void
b1_paused(void)
{
	uint32		gen;

	worker_take(0);
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_FREE);
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	sift_gpu_slot_set(&shm->slot[0], gen, SIFT_GPU_S_SUBMITTED);
	worker_take(0);
	sift_gpu_slot_finish(&shm->slot[0], gen, SIFT_GPU_S_RUNNING);
}

static void
test_abandon_aba(void)
{
	uint32		gen;

	fresh_shm();
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	sift_gpu_slot_set(&shm->slot[0], gen, SIFT_GPU_S_SUBMITTED);
	kill_hook = b1_paused;
	sift_gpu_slot_abandon(shm, &shm->slot[0], gen);
	SIFT_CHECK(kill_hook == NULL);
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_DONE && slot_gen(0) == gen + 1);
}

/* B0 freed the slot and exited; B1 won the slot and has not yet written its pid: B2 must not take B1's live slot for B0's */
static void
test_claim_theft(void)
{
	uint32		gen;

	fresh_shm();
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	shm->slot[0].owner_pid = dead_pid();
	sift_gpu_slot_set(&shm->slot[0], gen, SIFT_GPU_S_FREE);
	for (int i = 1; i < SIFT_GPU_SLOTS; i++)
		sift_gpu_slot_set(&shm->slot[i], 1, SIFT_GPU_S_RUNNING);
	pg_atomic_write_u64(&shm->slot[0].word, sift_gpu_slot_word(gen + 1, SIFT_GPU_S_CLAIMED));
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == -1);
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_CLAIMED);
}

/* The owner of a slot exited without leaving it: once its own pid is the one of the slot, the slot is taken back */
static void
test_claim_dead_owner(void)
{
	uint32		gen;

	fresh_shm();
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	for (int i = 1; i < SIFT_GPU_SLOTS; i++)
		sift_gpu_slot_set(&shm->slot[i], 1, SIFT_GPU_S_RUNNING);
	shm->slot[0].owner_pid = dead_pid();
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0 && gen == 2);
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
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf of a PG_TRY that the ERROR goes to */
	if (unit_wants("gpu_result_alloc"))
		test_result_alloc_fails();
#endif
	if (unit_wants("gpu_abandon_aba"))
		test_abandon_aba();
	if (unit_wants("gpu_claim_theft"))
		test_claim_theft();
	if (unit_wants("gpu_claim_dead_owner"))
		test_claim_dead_owner();
	free(shm);
	puts("gpu_worker: ok");
	return 0;
}
