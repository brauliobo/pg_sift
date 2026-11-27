/*
 * test/unit/agg/gpu/worker.c - the slots of the GPU worker that a backend claims (agg/gpu/worker.c): a backend that finds every slot busy gives the job back to the CPU after a short wait, not
 * after the 30 s of a statement held up, and a result the backend cannot copy (an allocation that fails) leaves the slot free for the other backends. A backend that left acts only on
 * the generation of the slot it claimed (agg/gpu/slot.c): it frees no other backend's answer, and no backend takes a live slot for the pid of the owner before it. A worker that ends
 * finishes only the jobs it took or would have taken, and leaves the state, the pid and the jobs of a worker that replaced it alone. The worker wakes the backend whose job it
 * finished, even when that backend frees the slot and another claims it as soon as the answer is out. A backend gives a start up (a timeout, a cancel) only while no worker holds the
 * pid: the READY or FAILED of a worker that came up meanwhile stays, a second worker of the same start ends, and a cancel leaves no STARTING behind. A backend that copies the
 * capabilities of the device while the worker writes new ones gets the old or the new, never a mix of the two.
 */
#include "postgres.h"

#include <sys/wait.h>

int			test_kill(pid_t pid, int sig);
#define kill test_kill
#include "agg/gpu/worker.c"
static bool test_cas_u64(volatile pg_atomic_uint64 *ptr, uint64 *expected, uint64 newval);
#define pg_atomic_compare_exchange_u64 test_cas_u64
#include "agg/gpu/slot.c"
#undef pg_atomic_compare_exchange_u64
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
static PGPROC procs[2];
static PROC_HDR proc_hdr = {.allProcs = procs};
PROC_HDR   *ProcGlobal = &proc_hdr;
int			sift_gpu_mode;
int			sift_gpu_cache_mb;
char	   *sift_gpu_device = "";

static SiftGpuShm *shm;
static uint64 waited_ms;
static int	answer_slot = -1;
static void (*kill_hook) (void);
static void (*done_hook) (void);
static Latch *woken;

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

/* The backends run right after a slot is moved to DONE: the window between the answer out and the wake of its owner */
static bool
test_cas_u64(volatile pg_atomic_uint64 *ptr, uint64 *expected, uint64 newval)
{
	void		(*hook) (void) = done_hook;
	const bool	moved = pg_atomic_compare_exchange_u64(ptr, expected, newval);

	if (moved && sift_gpu_slot_state(newval) == SIFT_GPU_S_DONE && hook != NULL)
	{
		done_hook = NULL;
		hook();
	}
	return moved;
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
	woken = l;
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

/* B0 takes its answer and frees the slot, B1 (proc number 1) claims it */
static void
b0_done_b1_claims(void)
{
	uint32		gen;

	sift_gpu_slot_set(&shm->slot[0], slot_gen(0), SIFT_GPU_S_FREE);
	MyProcNumber = 1;
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	MyProcNumber = 0;
}

static void
test_finish_wakes_owner(void)
{
	uint32		gen;

	fresh_shm();
	SIFT_CHECK(sift_gpu_slot_claim(shm, &gen) == 0);
	sift_gpu_slot_set(&shm->slot[0], gen, SIFT_GPU_S_SUBMITTED);
	worker_take(0);
	woken = NULL;
	done_hook = b0_done_b1_claims;
	sift_gpu_slot_finish(&shm->slot[0], gen, SIFT_GPU_S_RUNNING);
	SIFT_CHECK(done_hook == NULL && shm->slot[0].owner_procno == 1);
	SIFT_CHECK(woken == &procs[0].procLatch);
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

void
sift_vk_seat_close(SiftVkSeat *s)
{
}

static WorkerState exiting;

static BgwHandleStatus (*startup_hook) (void);

bool
RegisterDynamicBackgroundWorker(BackgroundWorker *worker, BackgroundWorkerHandle **handle)
{
	*handle = NULL;
	return true;
}

BgwHandleStatus
WaitForBackgroundWorkerStartup(BackgroundWorkerHandle *handle, pid_t *pid)
{
	return startup_hook();
}

static BgwHandleStatus
startup_cancelled(void)
{
	pg_re_throw();
	return BGWH_STARTED;
}

static BgwHandleStatus
startup_no_device(void)
{
	strlcpy(shm->last_error, "no device", sizeof(shm->last_error));
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_FAILED);
	return BGWH_STOPPED;
}

/* The worker of the start comes up between the backend's read of STARTING and its give-up at the timeout */
static void
worker_comes_up(void)
{
	pg_atomic_write_u32(&shm->worker_pid, (uint32) MyProcPid);
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_READY);
}

static void
test_start_timeout_keeps_ready(void)
{
	const char *why = NULL;

	fresh_shm();
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STARTING);
	pg_atomic_write_u32(&shm->worker_pid, (uint32) dead_pid());
	kill_hook = worker_comes_up;
	SIFT_CHECK(!ensure_worker(shm, &why));
	SIFT_CHECK(kill_hook == NULL);
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_READY);
}

static void
test_start_cancel(void)
{
	const char *why = NULL;
	volatile bool failed = false;

	fresh_shm();
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	pg_atomic_write_u32(&shm->worker_pid, 0);
	startup_hook = startup_cancelled;
	PG_TRY();
	{
		(void) ensure_worker(shm, &why);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	SIFT_CHECK(failed);
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_STOPPED);
}

static void
test_start_failed_kept(void)
{
	const char *why = NULL;

	fresh_shm();
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	pg_atomic_write_u32(&shm->worker_pid, 0);
	startup_hook = startup_no_device;
	SIFT_CHECK(!ensure_worker(shm, &why));
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_FAILED);
	SIFT_CHECK(why != NULL && strcmp(why, "no device") == 0);
}

/* W2 is started while W1, which holds the pid, opens its device: W2 ends; W1 given up finds no STARTING to settle */
static void
test_second_worker_ends(void)
{
	fresh_shm();
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STARTING);
	pg_atomic_write_u32(&shm->worker_pid, (uint32) getppid());
	SIFT_CHECK(!worker_claim(shm));
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_pid) == (uint32) getppid());
	pg_atomic_write_u32(&shm->worker_pid, (uint32) dead_pid());
	SIFT_CHECK(worker_claim(shm) && pg_atomic_read_u32(&shm->worker_pid) == (uint32) MyProcPid);
	pg_atomic_write_u32(&shm->worker_state, SIFT_GPU_W_STOPPED);
	SIFT_CHECK(!worker_settle(shm, SIFT_GPU_W_READY));
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_STOPPED);
}

/* W1 ends after W2 replaced it: W2's pid, state and RUNNING job stay as they are, W2's staging area is no backend's to reuse */
static void
test_exit_superseded(void)
{
	fresh_shm();
	pg_atomic_write_u32(&shm->worker_pid, (uint32) MyProcPid + 1);
	sift_gpu_slot_set(&shm->slot[0], 7, SIFT_GPU_S_RUNNING);
	sift_gpu_slot_set(&shm->slot[1], 3, SIFT_GPU_S_SUBMITTED);
	worker = &exiting;
	worker_exit(0, PointerGetDatum(shm));
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_pid) == (uint32) MyProcPid + 1);
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_READY);
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_RUNNING && slot_state(1) == SIFT_GPU_S_SUBMITTED);
}

/* W1 ends with its own job and one it did not take yet: both are lost, a RUNNING job of another worker is not, and the state says STOPPED once nothing is left */
static void
test_exit_drains_own(void)
{
	fresh_shm();
	sift_gpu_slot_set(&shm->slot[0], 7, SIFT_GPU_S_RUNNING);
	sift_gpu_slot_set(&shm->slot[1], 3, SIFT_GPU_S_RUNNING);
	sift_gpu_slot_set(&shm->slot[2], 5, SIFT_GPU_S_SUBMITTED);
	exiting = (WorkerState) {.shm = shm,.cur = &shm->slot[1],.gen = 3};
	worker = &exiting;
	worker_exit(0, PointerGetDatum(shm));
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_pid) == 0);
	SIFT_CHECK(pg_atomic_read_u32(&shm->worker_state) == SIFT_GPU_W_STOPPED);
	SIFT_CHECK(slot_state(0) == SIFT_GPU_S_RUNNING && shm->slot[0].status == 0);
	SIFT_CHECK(slot_state(1) == SIFT_GPU_S_DONE && shm->slot[1].status == SIFT_VK_LOST);
	SIFT_CHECK(slot_state(2) == SIFT_GPU_S_DONE && shm->slot[2].status == SIFT_VK_LOST);
}

enum { CAPS_ROUNDS = 100000 };

static pg_atomic_uint32 caps_torn;
static pg_atomic_uint32 caps_written;

static SiftVkCaps
caps_of(int k)
{
	SiftVkCaps	c = {.software = k,.rebar = k,.subgroup_size = 32u << k,.pcie_gbps = 8.0 * (k + 1),.vram_bytes = UINT64CONST(0x0101010101010101) * (k + 1),
	.cache_bytes = UINT64CONST(0x0202020202020202) * (k + 1),.pci_bus = k,.pci_dev = k};

	memset(c.name, 'a' + k, sizeof(c.name) - 1);
	return c;
}

static bool
caps_eq(const SiftVkCaps *a, const SiftVkCaps *b)
{
	return strcmp(a->name, b->name) == 0 && a->software == b->software && a->rebar == b->rebar && a->subgroup_size == b->subgroup_size && a->pcie_gbps == b->pcie_gbps &&
		a->vram_bytes == b->vram_bytes && a->cache_bytes == b->cache_bytes && a->pci_bus == b->pci_bus && a->pci_dev == b->pci_dev;
}

static void
caps_race(void *arg, int worker)
{
	const SiftVkCaps *caps = arg;

	if (worker == 0)
	{
		for (int i = 0; i < CAPS_ROUNDS; i++)
			caps_publish(shm, &caps[i & 1]);
		pg_atomic_write_u32(&caps_written, 1);
		return;
	}
	while (pg_atomic_read_u32(&caps_written) == 0)
	{
		SiftVkCaps	c;

		caps_read(shm, &c);
		if (!caps_eq(&c, &caps[0]) && !caps_eq(&c, &caps[1]))
			pg_atomic_fetch_add_u32(&caps_torn, 1);
	}
}

/* S120-17: the capabilities the worker publishes are copied whole by a backend that reads them meanwhile */
static void
test_caps_whole(void)
{
	SiftVkCaps	caps[2] = {caps_of(0), caps_of(1)};

	SIFT_DSM_INIT_CALL(shm_init, shm);
	caps_publish(shm, &caps[0]);
	pg_atomic_init_u32(&caps_torn, 0);
	pg_atomic_init_u32(&caps_written, 0);
	unit_run_workers(2, caps_race, caps);
	SIFT_CHECK(pg_atomic_read_u32(&caps_torn) == 0);
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
	if (unit_wants("gpu_exit_superseded"))
		test_exit_superseded();
	if (unit_wants("gpu_exit_drains_own"))
		test_exit_drains_own();
	if (unit_wants("gpu_finish_wakes_owner"))
		test_finish_wakes_owner();
	if (unit_wants("gpu_start_timeout_keeps_ready"))
		test_start_timeout_keeps_ready();
#ifndef __SANITIZE_THREAD__
	if (unit_wants("gpu_start_cancel"))
		test_start_cancel();
#endif
	if (unit_wants("gpu_start_failed_kept"))
		test_start_failed_kept();
	if (unit_wants("gpu_second_worker_ends"))
		test_second_worker_ends();
#ifndef __SANITIZE_THREAD__
	/* the copy a reader of a sequence lock makes again is a race ThreadSanitizer reports by design */
	if (unit_wants("gpu_caps_whole"))
		test_caps_whole();
#endif
	free(shm);
	puts("gpu_worker: ok");
	return 0;
}
