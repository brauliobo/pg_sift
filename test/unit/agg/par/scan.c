/*
 * agg/par/scan.c - a process that exits (FATAL) inside a guarded run nested in another leaves the barriers of both: the participants of the outer run are
 * not left waiting at its barrier for a participant that is gone.
 */
#include "postgres.h"

#include "agg/par/scan.c"
#include "host.h"
#include "test/common/check.h"
#include "test/common/pgshim.h"

static int	detached;

bool
BarrierDetach(Barrier *barrier)
{
	detached++;
	return false;
}

static SiftV6ParDsmHdr hdr[2];
static SiftV6ParRun runs[2];

static void
inner_body(SiftV6ParRun *r)
{
	par_host_before_shmem_exit();
}

static void
outer_body(SiftV6ParRun *r)
{
	sift_v6_par_guarded(&runs[1], inner_body);
}

static void
test_exit_in_a_nested_run_leaves_every_barrier(void)
{
	for (int i = 0; i < 2; i++)
		runs[i] = (SiftV6ParRun) {.hdr = &hdr[i],.attached = true};
	sift_v6_par_guarded(&runs[0], outer_body);
	SIFT_CHECKF(!runs[0].attached && !runs[1].attached, "outer attached %d, inner attached %d", runs[0].attached, runs[1].attached);
	SIFT_CHECKF(detached == 2, "%d barriers left", detached);
}

int
main(void)
{
	test_exit_in_a_nested_run_leaves_every_barrier();
	return sift_check_summary();
}
