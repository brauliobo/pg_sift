/*
 * test_vk_wait - the wait for a submission of the device layer (agg/vk/run.c) with the fence of the driver faked: the query that is cancelled while it waits leaves by the return of the wait, never by a
 * jump over the run that holds the memory of the job; the submission it left stays in flight and is settled by the next call; a wait that goes on for too long ends the use of the device.
 */
#include "postgres.h"

#include <setjmp.h>

#include "agg/vk/job.h"
#include "unit.h"

static int	fence_polls;
static int	ready_after;
static int	fence_resets;
static sigjmp_buf left_by_jump;

static VkResult
fake_wait(VkDevice dev, uint32_t n, const VkFence *fences, VkBool32 all, uint64_t timeout)
{
	return ++fence_polls > ready_after ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult
fake_reset(VkDevice dev, uint32_t n, const VkFence *fences)
{
	fence_resets++;
	return VK_SUCCESS;
}

static SiftVk *
busy_device(int polls_until_ready)
{
	SiftVk	   *vk = calloc(1, sizeof(*vk));

	vk->usable = true;
	vk->vkWaitForFences = fake_wait;
	vk->vkResetFences = fake_reset;
	vk->slot[0].busy = true;
	vk->inflight = true;
	fence_polls = 0;
	fence_resets = 0;
	ready_after = polls_until_ready;
	return vk;
}

static void
interrupt_by_jump(void)
{
	siglongjmp(left_by_jump, 1);
}

static void
test_cancel_returns_to_the_caller(void)
{
	SiftVk	   *vk = busy_device(1000);
	volatile bool returned = false;

	if (sigsetjmp(left_by_jump, 1) == 0)
	{
		(void) wait_slot(vk, &vk->slot[0], interrupt_by_jump);
		returned = true;
	}
	CHECK(returned);
	free(vk);
}

static void
test_submission_left_by_a_cancel_is_settled_by_the_next_call(void)
{
	SiftVk	   *vk = busy_device(1);

	if (sigsetjmp(left_by_jump, 1) == 0)
		(void) wait_slot(vk, &vk->slot[0], interrupt_by_jump);
	CHECK(vk->slot[0].busy && vk->inflight);
	CHECK(settle_all(vk));
	CHECK(!vk->slot[0].busy && !vk->inflight && fence_resets == 1);
	free(vk);
}

int
main(void)
{
	test_cancel_returns_to_the_caller();
	test_submission_left_by_a_cancel_is_settled_by_the_next_call();
	puts("test_vk_wait: ok");
	return 0;
}
