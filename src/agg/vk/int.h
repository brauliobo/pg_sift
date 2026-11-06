/*
 * agg/vk/int.h - the Vulkan backend, inside agg/vk/ (the interface is agg/vk.h, docs/GPU.md)
 */
#ifndef SIFT_AGG_VK_DIR_INT_H
#define SIFT_AGG_VK_DIR_INT_H

#include <signal.h>

#include "agg/vk.h"

#ifdef SIFT_VULKAN

#include "agg/vk_min.h"

#define VK_INSTANCE_FNS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VK_DEVICE_FNS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
	X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
	X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
	X(vkCmdCopyBuffer) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
	X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkDeviceWaitIdle)

#define SLAB_WORDS		SIFT_VK_CHUNK_MAX
#define SLAB_BYTES		((uint64_t) SLAB_WORDS * 4)
#define MIN_SLABS		4
#define UP_CAP			((uint64_t) 16 << 20)		/* staging: chunks of a batch */
#define DESC_CAP		((uint64_t) 1 << 20)		/* control tables: descriptors, workgroups */
#define WG_CAP			((uint64_t) 1 << 20)
#define RES_CAP			((uint64_t) 16 << 20)
#define WAIT_SLICE_NS	5000000ull					/* the wait for the fence is cut in slices: the host looks for interrupts between them */
#define WAIT_TOTAL_MS	10000						/* a submission that takes longer is given up */

typedef struct Buf
{
	VkBuffer	b;
	VkDeviceMemory m;
	void	   *map;
	uint64_t	size;
} Buf;

typedef struct Slab
{
	uint32_t	used;			/* words handed out */
	uint32_t	epoch;			/* bumped when the slab is reused: the entries of the older epoch are gone */
	uint64_t	stamp;			/* last use, for the least recently used choice */
	uint64_t	pin;			/* the batch that last used it: not evictable during that batch */
} Slab;

typedef struct Ent
{
	SiftVkKey	key;
	uint32_t	slab;
	uint32_t	epoch;
	uint32_t	woff;
	uint8_t		state;			/* 0 free, 1 used, 2 deleted */
} Ent;

typedef struct Copy
{
	uint64_t	src;
	uint64_t	dst;
	uint64_t	size;
} Copy;

struct SiftVk
{
	void	   *lib;
	PFN_vkGetInstanceProcAddr gipa;
#define X(n) PFN_##n n;
	VK_INSTANCE_FNS(X) VK_DEVICE_FNS(X)
#undef X
	VkInstance	inst;
	VkPhysicalDevice pd;
	VkDevice	dev;
	VkQueue		q;
	uint32_t	qf;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mp;
	bool		software;
	bool		stamps;			/* the queue has timestamps */
	bool		lost;
	bool		inflight;		/* a submission may still be running: settle first */
	char		err[256];
	VkCommandPool pool;
	VkCommandBuffer cb;
	VkFence		fence;
	VkQueryPool qp;
	VkShaderModule mod;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline	pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet ds;
	Buf			arena, stage, cdesc, cwg, rdev, rhost;
	/* the cache */
	uint32_t	nslab;
	Slab	   *slab;
	int64_t		cur;			/* the slab being filled, -1 none */
	uint64_t	clock;
	uint64_t	batch;			/* the batch in preparation */
	Ent		   *ent;
	uint32_t	nent;			/* power of two */
	uint32_t	used;			/* used + deleted slots */
	/* the batch in preparation */
	Copy	   *copy;
	uint32_t	ncopy;
	uint32_t	capcopy;
	uint64_t	up;				/* bytes of the staging area in use */
	SiftVkKey  *pend;
	uint32_t	npend;
};

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) return fail(vk, #x, r_); } while (0)

enum { K_DEVICE, K_HOST, K_READBACK };

/* mem.c, cache.c, run.c */
bool fail(SiftVk *vk, const char *what, VkResult r);
double now_ms(void);
void sigs_block(sigset_t *old);
void sigs_restore(const sigset_t *old);
void buf_free(SiftVk *vk, Buf *b);
bool buf_new(SiftVk *vk, Buf *b, uint64_t size, VkBufferUsageFlags usage, int kind);
bool make_cache(SiftVk *vk, uint64_t bytes);
bool settle(SiftVk *vk);

#endif							/* SIFT_VULKAN */

#endif
