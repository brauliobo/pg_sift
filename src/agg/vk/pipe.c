/*
 * agg/vk/pipe.c - the compute pipelines (LZ4, decode, the three aggregations, compaction, top N; the fast kernels are fast.c's), the two batch slots of the
 * double-buffered run (staging, work, control buffers, command buffers on the transfer and the compute queue, semaphore, fence) and
 * the descriptor set that binds the same twelve buffers to every shader.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/int.h"
#include "agg/vk_spv.h"

#ifdef SIFT_VULKAN

#define NBIND 12

static const struct
{
	const uint32_t *code;
	size_t		size;
}			shaders[PL_N] = {
	{sift_vk_spv_lz4, sizeof(sift_vk_spv_lz4)},
	{sift_vk_spv_decode, sizeof(sift_vk_spv_decode)},
	{sift_vk_spv_agg_ungrouped, sizeof(sift_vk_spv_agg_ungrouped)},
	{sift_vk_spv_agg_dense, sizeof(sift_vk_spv_agg_dense)},
	{sift_vk_spv_agg_hash, sizeof(sift_vk_spv_agg_hash)},
	{sift_vk_spv_ht_compact, sizeof(sift_vk_spv_ht_compact)},
	{sift_vk_spv_topn, sizeof(sift_vk_spv_topn)},
	{sift_vk_spv_agg_fast_ungrouped, sizeof(sift_vk_spv_agg_fast_ungrouped)},
	{sift_vk_spv_agg_fast_dense, sizeof(sift_vk_spv_agg_fast_dense)},
	{sift_vk_spv_agg_fast_hash, sizeof(sift_vk_spv_agg_fast_hash)},
};

bool
make_pipelines(SiftVk *vk)
{
	VkDescriptorSetLayoutBinding bind[NBIND];
	VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = NBIND, .pBindings = bind};
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NBIND * NSLOT};
	VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NSLOT, .poolSizeCount = 1, .pPoolSizes = &ps};

	for (int i = 0; i < NBIND; i++)
		bind[i] = (VkDescriptorSetLayoutBinding) {(uint32_t) i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
	CK(vk->vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &vk->dsl));
	pli.pSetLayouts = &vk->dsl;
	CK(vk->vkCreatePipelineLayout(vk->dev, &pli, NULL, &vk->pl));
	CK(vk->vkCreateDescriptorPool(vk->dev, &dpi, NULL, &vk->dpool));
	for (int i = 0; i < PL_N; i++)
	{
		VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaders[i].size, .pCode = shaders[i].code};
		VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = vk->pl};

		CK(vk->vkCreateShaderModule(vk->dev, &smi, NULL, &vk->mod[i]));
		cpi.stage = (VkPipelineShaderStageCreateInfo) {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = vk->mod[i], .pName = "main"};
		if (i < PL_FIXED)
			CK(vk->vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &vk->pipe[i]));
	}
	return true;
}

void
free_pipelines(SiftVk *vk)
{
	free_fast(vk);
	for (int i = 0; i < PL_N; i++)
	{
		if (vk->pipe[i])
			vk->vkDestroyPipeline(vk->dev, vk->pipe[i], NULL);
		if (vk->mod[i])
			vk->vkDestroyShaderModule(vk->dev, vk->mod[i], NULL);
	}
	if (vk->dpool)
		vk->vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
	if (vk->pl)
		vk->vkDestroyPipelineLayout(vk->dev, vk->pl, NULL);
	if (vk->dsl)
		vk->vkDestroyDescriptorSetLayout(vk->dev, vk->dsl, NULL);
}

/* the buffers of the descriptor set of every slot: the arena four times (words, bytes, halves, double words), the work area three times, ... */
bool
bind_buffers(SiftVk *vk)
{
	for (int k = 0; k < NSLOT; k++)
	{
		Slot	   *s = &vk->slot[k];
		VkDescriptorBufferInfo bi[NBIND];
		VkWriteDescriptorSet w[NBIND];
		VkBuffer	src[NBIND] = {vk->arena.b, vk->arena.b, vk->arena.b, vk->arena.b, s->work.b, s->work.b, s->work.b, s->dj.b, s->ctl.b,
		vk->res.b, vk->ht.b, vk->out.b};

		for (int i = 0; i < NBIND; i++)
		{
			bi[i] = (VkDescriptorBufferInfo) {src[i], 0, VK_WHOLE_SIZE};
			w[i] = (VkWriteDescriptorSet) {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = (uint32_t) i, .descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
		}
		vk->vkUpdateDescriptorSets(vk->dev, NBIND, w, 0, NULL);
	}
	return true;
}

bool
make_slots(SiftVk *vk)
{
	const VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tsrc = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, tdst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	uint64_t	work = UP_CAP + UNW_CAP + (uint64_t) DEC_WGS * SIFT_VK_DEC_SCR_BYTES + SLACK;
	VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
	VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = vk->dpool, .descriptorSetCount = 1,
		.pSetLayouts = &vk->dsl};

	if (!buf_new(vk, &vk->res, RES_CAP_WORDS * 8, st | tsrc | tdst, K_DEVICE) || !buf_new(vk, &vk->rinit, RES_CAP_WORDS * 8, tsrc, K_HOST) ||
		!buf_new(vk, &vk->rhost, RES_CAP_WORDS * 8, tdst, K_READBACK) || !buf_new(vk, &vk->ht, 1 << 20, st | tsrc | tdst, K_DEVICE) ||
		!buf_new(vk, &vk->out, 1 << 20, st | tsrc | tdst, K_DEVICE) || !buf_new(vk, &vk->hout, 1 << 20, tdst, K_READBACK))
		return false;
	vk->ht_cap = vk->out_cap = vk->hout_cap = 1 << 20;
	account(vk, (int64_t) (RES_CAP_WORDS * 8 * 2));
	for (int k = 0; k < NSLOT; k++)
	{
		Slot	   *s = &vk->slot[k];

		if (!buf_new(vk, &s->stage, UP_CAP + SLACK, tsrc, K_HOST) || !buf_new(vk, &s->work, work, st | tdst, K_DEVICE) ||
			!buf_new(vk, &s->ctl, CTL_CAP, st | tdst, K_REBAR) || !buf_new(vk, &s->dj, (uint64_t) DJ_MAX * SIFT_VK_DJ_WORDS * 4, st, K_REBAR) ||
			!buf_new(vk, &s->rstat, (uint64_t) SIFT_VK_RES_DEC_CAP * 8, tdst, K_READBACK))
			return false;
		ai.commandPool = vk->cpool;
		CK(vk->vkAllocateCommandBuffers(vk->dev, &ai, &s->ccb));
		ai.commandPool = vk->tpool;
		CK(vk->vkAllocateCommandBuffers(vk->dev, &ai, &s->tcb));
		CK(vk->vkCreateFence(vk->dev, &fi, NULL, &s->fence));
		CK(vk->vkCreateSemaphore(vk->dev, &si, NULL, &s->sem));
		if (vk->stamps)
			CK(vk->vkCreateQueryPool(vk->dev, &qci, NULL, &s->qp));
		s->stamps = vk->stamps;
		CK(vk->vkAllocateDescriptorSets(vk->dev, &dai, &s->ds));
		account(vk, (int64_t) (UP_CAP + SLACK));
	}
	return bind_buffers(vk);
}

void
free_slots(SiftVk *vk)
{
	for (int k = 0; k < NSLOT; k++)
	{
		Slot	   *s = &vk->slot[k];

		buf_free(vk, &s->stage);
		buf_free(vk, &s->work);
		buf_free(vk, &s->ctl);
		buf_free(vk, &s->dj);
		buf_free(vk, &s->rstat);
		if (s->fence)
			vk->vkDestroyFence(vk->dev, s->fence, NULL);
		if (s->sem)
			vk->vkDestroySemaphore(vk->dev, s->sem, NULL);
		if (s->qp)
			vk->vkDestroyQueryPool(vk->dev, s->qp, NULL);
		free(s->pend);
	}
	buf_free(vk, &vk->res);
	buf_free(vk, &vk->rinit);
	buf_free(vk, &vk->rhost);
	buf_free(vk, &vk->ht);
	buf_free(vk, &vk->out);
	buf_free(vk, &vk->hout);
}

#endif							/* SIFT_VULKAN */
