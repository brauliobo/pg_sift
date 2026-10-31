/*
 * agg/vk/dev.c - the open and the close of the Vulkan device
 * The choice of the device and its queue, the objects (device, buffers, pipeline) and the release of them.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agg/vk/int.h"
#include "agg/vk_spv.h"

#ifdef SIFT_VULKAN

static int
type_rank(int t)
{
	switch (t)
	{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			return 0;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			return 1;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			return 2;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			return 4;
		default:
			return 3;
	}
}

static bool
all_digits(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char) *s))
			return false;
	return true;
}

static bool
name_has(const char *name, const char *sub)
{
	size_t		n = strlen(sub);

	for (; *name; name++)
		if (strncasecmp(name, sub, n) == 0)
			return true;
	return false;
}

/* The device the setting names (an index, or a substring of the name), or the best one; the queue family with compute */
static bool
pick_device(SiftVk *vk, const char *sel, bool allow_software)
{
	uint32_t	n = 0,
				nq = 0;
	VkPhysicalDevice pd[16];
	VkQueueFamilyProperties qfp[32];
	int			best = -1,
				bestrank = 99;
	VkResult	r = vk->vkEnumeratePhysicalDevices(vk->inst, &n, NULL);

	if (r != VK_SUCCESS || n == 0)
	{
		snprintf(vk->err, sizeof(vk->err), "no Vulkan device");
		return false;
	}
	if (n > 16)
		n = 16;
	if (vk->vkEnumeratePhysicalDevices(vk->inst, &n, pd) < 0)
		return fail(vk, "vkEnumeratePhysicalDevices", VK_ERROR_INITIALIZATION_FAILED);
	for (uint32_t i = 0; i < n; i++)
	{
		VkPhysicalDeviceProperties p;
		int			rank;

		vk->vkGetPhysicalDeviceProperties(pd[i], &p);
		rank = type_rank(p.deviceType);
		if (sel[0] && all_digits(sel))
		{
			if ((uint32_t) atoi(sel) == i)
				best = (int) i;
		}
		else if (sel[0])
		{
			if (best < 0 && name_has(p.deviceName, sel))
				best = (int) i;
		}
		else if (rank < bestrank && (allow_software || rank < 4))
		{
			best = (int) i;
			bestrank = rank;
		}
	}
	if (best < 0)
	{
		if (sel[0])
			snprintf(vk->err, sizeof(vk->err), "no Vulkan device matches \"%s\"", sel);
		else
			snprintf(vk->err, sizeof(vk->err), "only a software Vulkan device is present (sift.gpu = on uses it)");
		return false;
	}
	vk->pd = pd[best];
	vk->vkGetPhysicalDeviceProperties(vk->pd, &vk->props);
	vk->software = vk->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->pd, &vk->mp);
	vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, NULL);
	if (nq > 32)
		nq = 32;
	vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, qfp);
	vk->qf = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if ((qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (vk->qf == UINT32_MAX || !(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)))
		{
			vk->qf = i;
			vk->stamps = qfp[i].timestampValidBits > 0 && vk->props.limits.timestampPeriod > 0;
		}
	if (vk->qf == UINT32_MAX)
	{
		snprintf(vk->err, sizeof(vk->err), "\"%.100s\" has no compute queue", vk->props.deviceName);
		return false;
	}
	return true;
}

static bool
make_pipeline(SiftVk *vk)
{
	VkDescriptorSetLayoutBinding bind[4];
	VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bind};
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
	VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(sift_vk_spv_agg), .pCode = sift_vk_spv_agg};
	VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
	VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
	VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1};
	VkDescriptorBufferInfo bi[3] = {{vk->cdesc.b, 0, VK_WHOLE_SIZE}, {vk->cwg.b, 0, VK_WHOLE_SIZE}, {vk->rdev.b, 0, VK_WHOLE_SIZE}};
	VkWriteDescriptorSet w[3];

	for (int i = 0; i < 4; i++)
		bind[i] = (VkDescriptorSetLayoutBinding) {(uint32_t) i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
	CK(vk->vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &vk->dsl));
	pli.pSetLayouts = &vk->dsl;
	CK(vk->vkCreatePipelineLayout(vk->dev, &pli, NULL, &vk->pl));
	CK(vk->vkCreateShaderModule(vk->dev, &smi, NULL, &vk->mod));
	cpi.layout = vk->pl;
	cpi.stage = (VkPipelineShaderStageCreateInfo) {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = vk->mod, .pName = "main"};
	CK(vk->vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &vk->pipe));
	CK(vk->vkCreateDescriptorPool(vk->dev, &dpi, NULL, &vk->dpool));
	ai.descriptorPool = vk->dpool;
	ai.pSetLayouts = &vk->dsl;
	CK(vk->vkAllocateDescriptorSets(vk->dev, &ai, &vk->ds));
	for (int i = 0; i < 3; i++)
		w[i] = (VkWriteDescriptorSet) {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vk->ds, .dstBinding = (uint32_t) i + 1, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
	vk->vkUpdateDescriptorSets(vk->dev, 3, w, 0, NULL);
	return true;
}

static bool
make_objects(SiftVk *vk, uint64_t cache_bytes)
{
	float		prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->qf, .queueCount = 1, .pQueuePriorities = &prio};
	VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
	VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->qf};
	VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
	const VkPhysicalDeviceLimits *lim = &vk->props.limits;

	if (lim->maxComputeWorkGroupSize[0] < 128 || lim->maxComputeSharedMemorySize < 64 * SIFT_VK_MAX_RS * 4 || lim->maxPushConstantsSize < 12 ||
		lim->maxPerStageDescriptorStorageBuffers < 4 || lim->maxStorageBufferRange < RES_CAP)
	{
		snprintf(vk->err, sizeof(vk->err), "\"%.100s\" is below the limits the kernel needs", vk->props.deviceName);
		return false;
	}
	CK(vk->vkCreateDevice(vk->pd, &di, NULL, &vk->dev));
#define X(n) vk->n = (PFN_##n) vk->vkGetDeviceProcAddr(vk->dev, #n);
	VK_DEVICE_FNS(X)
#undef X
	vk->vkGetDeviceQueue(vk->dev, vk->qf, 0, &vk->q);
	CK(vk->vkCreateCommandPool(vk->dev, &ci, NULL, &vk->pool));
	ai.commandPool = vk->pool;
	CK(vk->vkAllocateCommandBuffers(vk->dev, &ai, &vk->cb));
	CK(vk->vkCreateFence(vk->dev, &fi, NULL, &vk->fence));
	if (vk->stamps)
		CK(vk->vkCreateQueryPool(vk->dev, &qci, NULL, &vk->qp));
	if (!buf_new(vk, &vk->stage, UP_CAP + DESC_CAP + WG_CAP, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, K_HOST) ||
		!buf_new(vk, &vk->cdesc, DESC_CAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_DEVICE) ||
		!buf_new(vk, &vk->cwg, WG_CAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_DEVICE) ||
		!buf_new(vk, &vk->rdev, RES_CAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, K_DEVICE) ||
		!buf_new(vk, &vk->rhost, RES_CAP, VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_READBACK))
		return false;
	if (!vk->stage.map || !vk->rhost.map)
	{
		snprintf(vk->err, sizeof(vk->err), "the staging memory of \"%.100s\" cannot be mapped", vk->props.deviceName);
		return false;
	}
	vk->batch = 1;
	vk->batch = 1;
	return make_pipeline(vk) && make_cache(vk, cache_bytes);
}

static void
release(SiftVk *vk)
{
	if (vk->dev)
	{
		if (!vk->lost && !vk->inflight)
			vk->vkDeviceWaitIdle(vk->dev);
		if (!vk->inflight)
		{
			buf_free(vk, &vk->arena);
			buf_free(vk, &vk->stage);
			buf_free(vk, &vk->cdesc);
			buf_free(vk, &vk->cwg);
			buf_free(vk, &vk->rdev);
			buf_free(vk, &vk->rhost);
			if (vk->dpool)
				vk->vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
			if (vk->pipe)
				vk->vkDestroyPipeline(vk->dev, vk->pipe, NULL);
			if (vk->pl)
				vk->vkDestroyPipelineLayout(vk->dev, vk->pl, NULL);
			if (vk->dsl)
				vk->vkDestroyDescriptorSetLayout(vk->dev, vk->dsl, NULL);
			if (vk->mod)
				vk->vkDestroyShaderModule(vk->dev, vk->mod, NULL);
			if (vk->qp)
				vk->vkDestroyQueryPool(vk->dev, vk->qp, NULL);
			if (vk->fence)
				vk->vkDestroyFence(vk->dev, vk->fence, NULL);
			if (vk->pool)
				vk->vkDestroyCommandPool(vk->dev, vk->pool, NULL);
			vk->vkDestroyDevice(vk->dev, NULL);
		}
	}
	if (vk->inst && !vk->inflight)
		vk->vkDestroyInstance(vk->inst, NULL);
	/* the library stays mapped: a driver may have threads or atexit handlers that run code in it */
	free(vk->slab);
	free(vk->ent);
	free(vk->copy);
	free(vk->pend);
}

void
sift_vk_close(SiftVk *vk)
{
	sigset_t	old;

	if (!vk)
		return;
	sigs_block(&old);
	if (vk->inflight)
		settle(vk);
	release(vk);
	sigs_restore(&old);
	free(vk);
}

SiftVk *
sift_vk_open(const char *device, bool allow_software, uint64_t cache_bytes, char *err, size_t errlen)
{
	SiftVk	   *vk = calloc(1, sizeof(SiftVk));
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pg_sift", .apiVersion = VK_API_VERSION_1_0};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	PFN_vkCreateInstance create;
	sigset_t	old;
	bool		ok = false;
	VkResult	r;

	if (!vk)
	{
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	vk->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!vk->lib)
	{
		snprintf(err, errlen, "libvulkan.so.1 cannot be loaded: %s", dlerror());
		free(vk);
		return NULL;
	}
	vk->gipa = (PFN_vkGetInstanceProcAddr) dlsym(vk->lib, "vkGetInstanceProcAddr");
	create = vk->gipa ? (PFN_vkCreateInstance) vk->gipa(NULL, "vkCreateInstance") : NULL;
	if (!create)
	{
		snprintf(err, errlen, "libvulkan.so.1 has no vkCreateInstance");
		dlclose(vk->lib);
		free(vk);
		return NULL;
	}
	sigs_block(&old);
	r = create(&ici, NULL, &vk->inst);
	if (r != VK_SUCCESS)
		snprintf(vk->err, sizeof(vk->err), r == VK_ERROR_INCOMPATIBLE_DRIVER ? "no Vulkan driver is installed" : "vkCreateInstance failed (VkResult %d)", (int) r);
	else
	{
#define X(n) vk->n = (PFN_##n) vk->gipa(vk->inst, #n);
		VK_INSTANCE_FNS(X)
#undef X
		ok = pick_device(vk, device ? device : "", allow_software) && make_objects(vk, cache_bytes);
	}
	sigs_restore(&old);
	if (!ok)
	{
		snprintf(err, errlen, "%s", vk->err[0] ? vk->err : "the Vulkan device cannot be opened");
		if (vk->inst)
		{
			sigs_block(&old);
			release(vk);
			sigs_restore(&old);
		}
		free(vk);
		return NULL;
	}
	return vk;
}

#endif							/* SIFT_VULKAN */
