/*
 * core/waits.c - see waits.h.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

#include "core/waits.h"

static const char *const wait_names[SIFT_WAIT_COUNT] = {
	[SIFT_WAIT_NAP] = "SiftMaintenanceNap",
	[SIFT_WAIT_ARENA] = "SiftArenaWait",
	[SIFT_WAIT_PAR_LEADER] = "SiftParallelLeader",
	[SIFT_WAIT_LOAD_RING] = "SiftLoadRing",
	[SIFT_WAIT_BUILD_GATE] = "SiftBuildGate",
	[SIFT_WAIT_MAINT_PACE] = "SiftMaintenancePace",
	[SIFT_WAIT_TAIL_LOCK] = "SiftTailShrinkLock",
};

static uint32 wait_ids[SIFT_WAIT_COUNT];

uint32
sift_wait_event(SiftWait w)
{
	if (wait_ids[w] == 0)
		wait_ids[w] = WaitEventExtensionNew(wait_names[w]);
	return wait_ids[w];
}

void
sift_wait(SiftWait w, long ms)
{
	(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, ms, sift_wait_event(w));
	ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}
