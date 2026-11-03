/*
 * agg/vk/run.c - the submission of a batch
 * The wait for a submission, the command buffer of a batch (copies, the kernel, the readback) and its run.
 */
#include <stdio.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

bool
settle(SiftVk *vk)
{
	double		t0;

	if (!vk->inflight)
		return !vk->lost;
	t0 = now_ms();
	for (;;)
	{
		VkResult	r = vk->vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE, WAIT_SLICE_NS);

		if (r == VK_SUCCESS)
		{
			vk->inflight = false;
			return true;
		}
		if (r != VK_TIMEOUT)
		{
			fail(vk, "vkWaitForFences", r);
			vk->lost = true;
			return false;
		}
		if (now_ms() - t0 > WAIT_TOTAL_MS)
		{
			snprintf(vk->err, sizeof(vk->err), "a submission did not finish in %d ms", WAIT_TOTAL_MS);
			vk->lost = true;
			return false;
		}
	}
}

static void
barrier(SiftVk *vk, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da)
{
	VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};

	vk->vkCmdPipelineBarrier(vk->cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void
stamp(SiftVk *vk, int slot, VkPipelineStageFlagBits stage)
{
	if (vk->stamps)
		vk->vkCmdWriteTimestamp(vk->cb, stage, vk->qp, (uint32_t) slot);
}

static bool
record(SiftVk *vk, const SiftVkJob *job, uint64_t desc_bytes, uint64_t wg_bytes, uint64_t res_bytes)
{
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	uint32_t	pc[3] = {job->npred, job->nsum, job->groups};
	VkBufferCopy r;

	CK(vk->vkResetCommandBuffer(vk->cb, 0));
	CK(vk->vkBeginCommandBuffer(vk->cb, &bi));
	if (vk->stamps)
		vk->vkCmdResetQueryPool(vk->cb, vk->qp, 0, 4);
	stamp(vk, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
	for (uint32_t i = 0; i < vk->ncopy; i++)
	{
		r = (VkBufferCopy) {vk->copy[i].src, vk->copy[i].dst, vk->copy[i].size};
		vk->vkCmdCopyBuffer(vk->cb, vk->stage.b, vk->arena.b, 1, &r);
	}
	r = (VkBufferCopy) {UP_CAP, 0, desc_bytes};
	vk->vkCmdCopyBuffer(vk->cb, vk->stage.b, vk->cdesc.b, 1, &r);
	r = (VkBufferCopy) {UP_CAP + DESC_CAP, 0, wg_bytes};
	vk->vkCmdCopyBuffer(vk->cb, vk->stage.b, vk->cwg.b, 1, &r);
	barrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	stamp(vk, 1, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vk->vkCmdBindPipeline(vk->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vk->pipe);
	vk->vkCmdBindDescriptorSets(vk->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vk->pl, 0, 1, &vk->ds, 0, NULL);
	vk->vkCmdPushConstants(vk->cb, vk->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vk->vkCmdDispatch(vk->cb, job->nwg, 1, 1);
	barrier(vk, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	stamp(vk, 2, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	r = (VkBufferCopy) {0, 0, res_bytes};
	vk->vkCmdCopyBuffer(vk->cb, vk->rdev.b, vk->rhost.b, 1, &r);
	barrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	stamp(vk, 3, VK_PIPELINE_STAGE_TRANSFER_BIT);
	CK(vk->vkEndCommandBuffer(vk->cb));
	return true;
}

static void
read_times(SiftVk *vk, SiftVkTimes *t)
{
	uint64_t	ts[4];

	if (!vk->stamps || vk->vkGetQueryPoolResults(vk->dev, vk->qp, 0, 4, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
		return;
	t->upload_ms = (double) (ts[1] - ts[0]) * vk->props.limits.timestampPeriod / 1e6;
	t->kernel_ms = (double) (ts[2] - ts[1]) * vk->props.limits.timestampPeriod / 1e6;
	t->readback_ms = (double) (ts[3] - ts[2]) * vk->props.limits.timestampPeriod / 1e6;
}

SiftVkStatus
sift_vk_run(SiftVk *vk, const SiftVkJob *job, void (*interrupt) (void), const uint32_t **results, SiftVkTimes *times)
{
	uint32_t	nslots = job->npred + 1 + job->nsum;
	uint64_t	desc_bytes = (uint64_t) job->nunits * nslots * 16,
				wg_bytes = (uint64_t) job->nwg * 16,
				res_bytes = (uint64_t) job->nwg * (job->groups ? job->groups : 1) * (1 + 2 * job->nsum) * 4;
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
	sigset_t	old;
	VkResult	r;
	double		t0,
				t1;

	if (times)
		memset(times, 0, sizeof(*times));
	if (vk->lost || !settle(vk))
		return vk->lost ? SIFT_VK_LOST : SIFT_VK_FAIL;
	if (job->nwg == 0 || desc_bytes > DESC_CAP || wg_bytes > WG_CAP || res_bytes > RES_CAP || job->nwg > vk->props.limits.maxComputeWorkGroupCount[0])
	{
		snprintf(vk->err, sizeof(vk->err), "a job of %u units and %u workgroups does not fit", job->nunits, job->nwg);
		return SIFT_VK_FAIL;
	}
	memcpy((char *) vk->stage.map + UP_CAP, job->desc, desc_bytes);
	memcpy((char *) vk->stage.map + UP_CAP + DESC_CAP, job->wg, wg_bytes);
	if (!record(vk, job, desc_bytes, wg_bytes, res_bytes))
		return vk->lost ? SIFT_VK_LOST : SIFT_VK_FAIL;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &vk->cb;
	t0 = now_ms();
	sigs_block(&old);
	r = vk->vkResetFences(vk->dev, 1, &vk->fence);
	if (r == VK_SUCCESS)
		r = vk->vkQueueSubmit(vk->q, 1, &si, vk->fence);
	sigs_restore(&old);
	if (r != VK_SUCCESS)
	{
		fail(vk, "vkQueueSubmit", r);
		return vk->lost ? SIFT_VK_LOST : SIFT_VK_FAIL;
	}
	vk->inflight = true;
	for (;;)
	{
		r = vk->vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE, WAIT_SLICE_NS);
		if (r == VK_SUCCESS)
			break;
		if (r != VK_TIMEOUT)
		{
			fail(vk, "vkWaitForFences", r);
			vk->lost = true;
			return SIFT_VK_LOST;
		}
		if (interrupt)
			interrupt();
		if (now_ms() - t0 > WAIT_TOTAL_MS)
		{
			snprintf(vk->err, sizeof(vk->err), "the submission did not finish in %d ms", WAIT_TOTAL_MS);
			vk->lost = true;
			return SIFT_VK_TIMEOUT;
		}
	}
	t1 = now_ms();
	vk->inflight = false;
	vk->npend = vk->ncopy = 0;
	vk->up = 0;
	vk->batch++;
	if (times)
	{
		read_times(vk, times);
		times->wall_ms = t1 - t0;
	}
	*results = vk->rhost.map;
	return SIFT_VK_OK;
}

#endif							/* SIFT_VULKAN */
