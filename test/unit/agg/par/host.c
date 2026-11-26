/*
 * agg/par/host.c - the backend of core/par/pool.c and core/par/morsel.c for the unit tests: the process flags the pool reads, the exit callbacks it registers (a test runs them as the backend does at
 * exit, and at FATAL), and the interrupt and error stack that a cancelled statement unwinds through (an interrupt jumps to the innermost PG_TRY, as ereport(ERROR) does).
 */
#include "postgres.h"

#include "access/parallel.h"
#include "miscadmin.h"
#include "storage/ipc.h"

#include <sys/mman.h>

#include "col/dsmreg.h"

#include "host.h"

bool		IsUnderPostmaster = true;
BackendType MyBackendType = B_BACKEND;
int			ParallelWorkerNumber = -1;
int			max_parallel_workers_per_gather = 3;
int			max_parallel_workers = 64;
bool		sift_parallel_scan = true;
volatile int par_host_cancels_taken;
volatile sig_atomic_t InterruptPending = false;
volatile sig_atomic_t QueryCancelPending = false;
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;

/* A table of the shared memory of the server: mapped shared, so that a process the test forks is another backend of the same server */
void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	void	   *raw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (raw == MAP_FAILED)
		return NULL;
	SIFT_DSM_INIT_CALL(init, raw);
	return raw;
}

typedef struct ExitCb
{
	pg_on_exit_callback fn;
	Datum		arg;
} ExitCb;

static ExitCb before_exit[8];
static int	nbefore;
static ExitCb proc_exit_cb[8];
static int	nproc;

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	before_exit[nbefore++] = (ExitCb) {function, arg};
}

void
on_proc_exit(pg_on_exit_callback function, Datum arg)
{
	proc_exit_cb[nproc++] = (ExitCb) {function, arg};
}

void
par_host_before_shmem_exit(void)
{
	for (int i = nbefore; i-- > 0;)
		before_exit[i].fn(0, before_exit[i].arg);
}

void
par_host_proc_exit(void)
{
	for (int i = nproc; i-- > 0;)
		proc_exit_cb[i].fn(0, proc_exit_cb[i].arg);
}

void
ProcessInterrupts(void)
{
	InterruptPending = false;
	if (QueryCancelPending)
	{
		QueryCancelPending = false;
		par_host_cancels_taken++;
		siglongjmp(*PG_exception_stack, 1);
	}
}

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

Size
add_size(Size s1, Size s2)
{
	return s1 + s2;
}

Size
mul_size(Size s1, Size s2)
{
	return s1 * s2;
}
