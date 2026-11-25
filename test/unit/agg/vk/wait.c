/*
 * test_vk_wait - the wait for a submission of the device layer (agg/vk/run.c) with the fence of the driver faked: the query that is cancelled while it waits leaves by the return of the wait, never by a
 * jump over the run that holds the memory of the job; the device stays usable and the submission it left stays in flight, to be settled by the next call.
 */
#include "postgres.h"

#include "agg/vk/job.h"
#include "test/common/check.h"

static int	fence_polls;
static int	ready_after;
static int	fence_resets;
static int	asked;

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
	asked = 0;
	ready_after = polls_until_ready;
	return vk;
}

static bool
cancelled_at_third_ask(void)
{
	return ++asked == 3;
}

static bool
never_cancelled(void)
{
	asked++;
	return false;
}

static void
test_cancel_ends_the_wait_and_keeps_the_device(void)
{
	SiftVk	   *vk = busy_device(1000);

	SIFT_CHECK(!wait_slot(vk, &vk->slot[0], cancelled_at_third_ask));
	SIFT_CHECK(asked == 3 && fence_polls == 3);
	SIFT_CHECK(vk->cancel && vk->usable && !vk->lost);
	SIFT_CHECK(vk->slot[0].busy && vk->inflight);
	free(vk);
}

static void
test_submission_left_by_a_cancel_is_settled_by_the_next_call(void)
{
	SiftVk	   *vk = busy_device(3);

	SIFT_CHECK(!wait_slot(vk, &vk->slot[0], cancelled_at_third_ask));
	SIFT_CHECK(settle_all(vk));
	SIFT_CHECK(!vk->slot[0].busy && !vk->inflight && fence_resets == 1);
	free(vk);
}

static void
test_a_wait_that_is_not_cancelled_runs_to_the_fence(void)
{
	SiftVk	   *vk = busy_device(4);

	SIFT_CHECK(wait_slot(vk, &vk->slot[0], never_cancelled));
	SIFT_CHECK(asked == 4 && !vk->cancel);
	SIFT_CHECK(!vk->slot[0].busy && !vk->inflight);
	free(vk);
}

int
main(void)
{
	test_cancel_ends_the_wait_and_keeps_the_device();
	test_submission_left_by_a_cancel_is_settled_by_the_next_call();
	test_a_wait_that_is_not_cancelled_runs_to_the_fence();
	puts("test_vk_wait: ok");
	return 0;
}
