/*
 * agg/vk/mem.c - the device memory and the small helpers of the Vulkan backend
 * The error of a call, the clock, the signal mask around a driver call, the four classes of buffer (device, ReBAR, host, readback), the
 * budget the driver gives (VK_EXT_memory_budget) and the accessors of a device.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

bool
fail(SiftVk *vk, const char *what, VkResult r)
{
	snprintf(vk->err, sizeof(vk->err), "%s failed (VkResult %d)", what, (int) r);
	if (r == VK_ERROR_DEVICE_LOST)
		vk->lost = true;
	return false;
}

double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/*
 * A driver starts threads of its own: a signal addressed to the process must not be handled by one of them (the handlers of a backend are
 * written for the thread that runs it). Blocking every signal around the calls that can start one makes the new threads inherit the mask.
 */
void
sigs_block(sigset_t *old)
{
	sigset_t	all;

	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, old);
}

void
sigs_restore(const sigset_t *old)
{
	pthread_sigmask(SIG_SETMASK, old, NULL);
}

void
account(SiftVk *vk, int64_t delta)
{
	vk->accounted += delta;
	if (vk->account)
		vk->account(vk->account_arg, delta);
}

const char *
sift_vk_error(const SiftVk *vk)
{
	return vk ? vk->err : "";
}

const SiftVkCaps *
sift_vk_caps(const SiftVk *vk)
{
	return &vk->caps;
}

bool
sift_vk_usable(const SiftVk *vk)
{
	return vk && vk->usable && !vk->lost;
}

void
sift_vk_set_account(SiftVk *vk, void (*fn) (void *arg, int64_t delta), void *arg)
{
	if (vk->account && vk->accounted)
		vk->account(vk->account_arg, -vk->accounted);
	vk->account = fn;
	vk->account_arg = arg;
	if (fn && vk->accounted)
		fn(arg, vk->accounted);
}

void
sift_vk_test_inject(SiftVk *vk, SiftVkStatus status)
{
	vk->test_lost = status == SIFT_VK_LOST;
	vk->test_timeout = status == SIFT_VK_TIMEOUT;
}

static int
memtype(const SiftVk *vk, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
	for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
	{
		VkMemoryPropertyFlags f = vk->mp.memoryTypes[i].propertyFlags;

		if ((bits & (1u << i)) && (f & want) == want && !(f & avoid))
			return (int) i;
	}
	return -1;
}

/* the bytes of the device local heap that are still free: what the driver says, else three quarters of the heap */
uint64_t
heap_budget_free(SiftVk *vk)
{
	VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
	VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bud};
	uint64_t	best = 0,
				heap_size = 0;
	uint32_t	heap = UINT32_MAX;

	for (uint32_t i = 0; i < vk->mp.memoryHeapCount; i++)
		if ((vk->mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && vk->mp.memoryHeaps[i].size > heap_size)
		{
			heap = i;
			heap_size = vk->mp.memoryHeaps[i].size;
		}
	if (heap == UINT32_MAX)
		return 0;
	if (!vk->caps.ext_budget)
		return heap_size / 4 * 3;
	vk->vkGetPhysicalDeviceMemoryProperties2(vk->pd, &mp2);
	best = bud.heapBudget[heap] > bud.heapUsage[heap] ? bud.heapBudget[heap] - bud.heapUsage[heap] : 0;
	return best;
}

void
buf_free(SiftVk *vk, Buf *b)
{
	if (b->map)
		vk->vkUnmapMemory(vk->dev, b->m);
	if (b->b)
		vk->vkDestroyBuffer(vk->dev, b->b, NULL);
	if (b->m)
		vk->vkFreeMemory(vk->dev, b->m, NULL);
	memset(b, 0, sizeof(*b));
}

bool
buf_new(SiftVk *vk, Buf *b, uint64_t size, VkBufferUsageFlags usage, int kind)
{
	uint32_t	fams[2] = {vk->cf, vk->tf};
	VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkMemoryRequirements mr;
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	int			t;
	VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkResult	r;

	memset(b, 0, sizeof(*b));
	b->size = size;
	if (vk->split_queues)
	{
		bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
		bi.queueFamilyIndexCount = 2;
		bi.pQueueFamilyIndices = fams;
	}
	CK(vk->vkCreateBuffer(vk->dev, &bi, NULL, &b->b));
	vk->vkGetBufferMemoryRequirements(vk->dev, b->b, &mr);
	if (kind == K_DEVICE)
	{
		t = memtype(vk, mr.memoryTypeBits, dl, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, dl, 0);
	}
	else if (kind == K_REBAR)
	{
		t = memtype(vk, mr.memoryTypeBits, hv | dl, 0);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, dl);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, 0);
	}
	else if (kind == K_HOST)
	{
		t = memtype(vk, mr.memoryTypeBits, hv, dl);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, 0);
	}
	else
	{
		t = memtype(vk, mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, dl);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, 0);
	}
	if (t < 0)
	{
		snprintf(vk->err, sizeof(vk->err), "no memory type for a buffer of kind %d", kind);
		buf_free(vk, b);
		return false;
	}
	ma.allocationSize = mr.size;
	ma.memoryTypeIndex = (uint32_t) t;
	r = vk->vkAllocateMemory(vk->dev, &ma, NULL, &b->m);
	if (r != VK_SUCCESS)
	{
		b->m = VK_NULL_HANDLE;
		buf_free(vk, b);
		return fail(vk, "vkAllocateMemory", r);
	}
	r = vk->vkBindBufferMemory(vk->dev, b->b, b->m, 0);
	if (r == VK_SUCCESS && (vk->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && kind != K_DEVICE)
		r = vk->vkMapMemory(vk->dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->map);
	if (r != VK_SUCCESS)
	{
		buf_free(vk, b);
		return fail(vk, "binding a buffer", r);
	}
	return true;
}

#endif							/* SIFT_VULKAN */
