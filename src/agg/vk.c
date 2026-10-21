/*
 * agg/vk.c - the Vulkan compute device of the aggregate scan (agg/vk.h, docs/GPU.md).
 *
 * Compiled only with SIFT_VULKAN (make VULKAN=1); the default build has none of it. libvulkan.so.1 is opened by dlopen at the first
 * sift_vk_open and the declarations are the vendored subset of agg/vk_min.h. Plain C, no PostgreSQL headers: the caller decides
 * what a failure means (the executor runs the query on the CPU and stops using the device for the session).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agg/vk.h"

#ifdef SIFT_VULKAN

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "agg/vk_min.h"
#include "agg/vk_spv.h"

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
	uint32_t	cappend;
};

static bool
fail(SiftVk *vk, const char *what, VkResult r)
{
	snprintf(vk->err, sizeof(vk->err), "%s failed (VkResult %d)", what, (int) r);
	if (r == VK_ERROR_DEVICE_LOST)
		vk->lost = true;
	return false;
}

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) return fail(vk, #x, r_); } while (0)

static double
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
static void
sigs_block(sigset_t *old)
{
	sigset_t	all;

	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, old);
}

static void
sigs_restore(const sigset_t *old)
{
	pthread_sigmask(SIG_SETMASK, old, NULL);
}

const char *
sift_vk_error(const SiftVk *vk)
{
	return vk ? vk->err : "";
}

const char *
sift_vk_device_name(const SiftVk *vk)
{
	return vk->props.deviceName;
}

bool
sift_vk_is_software(const SiftVk *vk)
{
	return vk->software;
}

uint64_t
sift_vk_cache_bytes(const SiftVk *vk)
{
	return (uint64_t) vk->nslab * SLAB_BYTES;
}

uint32_t
sift_vk_max_wg(const SiftVk *vk, uint32_t groups, uint32_t nsum)
{
	uint64_t	per = (uint64_t) (groups ? groups : 1) * (1 + 2 * nsum) * 4;
	uint64_t	n = RES_CAP / per;

	n = n < vk->props.limits.maxComputeWorkGroupCount[0] ? n : vk->props.limits.maxComputeWorkGroupCount[0];
	return (uint32_t) (n < WG_CAP / 16 ? n : WG_CAP / 16);
}

/* ---------- buffers ---------- */

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

enum { K_DEVICE, K_HOST, K_READBACK };

static void
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

static bool
buf_new(SiftVk *vk, Buf *b, uint64_t size, VkBufferUsageFlags usage, int kind)
{
	VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkMemoryRequirements mr;
	VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	int			t;
	VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkResult	r;

	memset(b, 0, sizeof(*b));
	b->size = size;
	CK(vk->vkCreateBuffer(vk->dev, &bi, NULL, &b->b));
	vk->vkGetBufferMemoryRequirements(vk->dev, b->b, &mr);
	if (kind == K_DEVICE)
	{
		t = memtype(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	}
	else if (kind == K_HOST)
	{
		t = memtype(vk, mr.memoryTypeBits, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, 0);
	}
	else
	{
		t = memtype(vk, mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
		if (t < 0)
			t = memtype(vk, mr.memoryTypeBits, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

/* ---------- the cache of chunks ---------- */

static uint32_t
key_hash(const SiftVkKey *k)
{
	uint64_t	h = k->rel * 0x9E3779B97F4A7C15ull;

	h ^= (k->gen + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
	h ^= ((uint64_t) k->unit << 20 | k->leaf) * 0x165667B19E3779F9ull;
	h ^= h >> 29;
	h *= 0xBF58476D1CE4E5B9ull;
	return (uint32_t) (h >> 32);
}

static bool
key_eq(const SiftVkKey *a, const SiftVkKey *b)
{
	return a->rel == b->rel && a->gen == b->gen && a->unit == b->unit && a->leaf == b->leaf;
}

static bool
ent_live(const SiftVk *vk, const Ent *e)
{
	return e->state == 1 && vk->slab[e->slab].epoch == e->epoch;
}

static Ent *
ent_find(SiftVk *vk, const SiftVkKey *k)
{
	uint32_t	h = key_hash(k) & (vk->nent - 1);

	for (uint32_t i = 0; i < vk->nent; i++, h = (h + 1) & (vk->nent - 1))
	{
		Ent		   *e = &vk->ent[h];

		if (e->state == 0)
			return NULL;
		if (e->state == 1 && key_eq(&e->key, k))
			return ent_live(vk, e) ? e : NULL;
	}
	return NULL;
}

static bool
ent_rebuild(SiftVk *vk, uint32_t nent)
{
	Ent		   *old = vk->ent;
	uint32_t	nold = vk->nent;
	Ent		   *n = calloc(nent, sizeof(Ent));

	if (!n)
		return false;
	vk->ent = n;
	vk->nent = nent;
	vk->used = 0;
	for (uint32_t i = 0; i < nold; i++)
		if (old && ent_live(vk, &old[i]))
		{
			uint32_t	h = key_hash(&old[i].key) & (nent - 1);

			while (n[h].state)
				h = (h + 1) & (nent - 1);
			n[h] = old[i];
			vk->used++;
		}
	free(old);
	return true;
}

static bool
ent_put(SiftVk *vk, const SiftVkKey *k, uint32_t slab, uint32_t woff)
{
	uint32_t	h;

	if ((vk->used + 1) * 2 > vk->nent)
	{
		/* stale entries (evicted slabs) go away on the way; the table doubles only when it is still half full of live ones */
		if (!ent_rebuild(vk, vk->nent))
			return false;
		if ((vk->used + 1) * 2 > vk->nent && !ent_rebuild(vk, vk->nent * 2))
			return false;
	}
	h = key_hash(k) & (vk->nent - 1);
	while (vk->ent[h].state == 1)
		h = (h + 1) & (vk->nent - 1);
	if (vk->ent[h].state == 0)
		vk->used++;
	vk->ent[h] = (Ent) {*k, slab, vk->slab[slab].epoch, woff, 1};
	return true;
}

static void
ent_drop(SiftVk *vk, const SiftVkKey *k)
{
	Ent		   *e = ent_find(vk, k);

	if (e)
		e->state = 2;
}

/* A slab with room for n words for the batch in preparation: the one being filled, an empty one, or the least recently used one that the batch does not use */
static int64_t
slab_for(SiftVk *vk, uint32_t n)
{
	int64_t		best = -1;

	if (vk->cur >= 0 && vk->slab[vk->cur].used + n <= SLAB_WORDS)
		return vk->cur;
	for (uint32_t i = 0; i < vk->nslab; i++)
	{
		const Slab *s = &vk->slab[i];

		if (s->used == 0)
		{
			best = i;
			break;
		}
		if (s->pin != vk->batch && (best < 0 || s->stamp < vk->slab[best].stamp))
			best = i;
	}
	if (best < 0)
		return -1;
	vk->slab[best].used = 0;
	vk->slab[best].epoch++;
	vk->cur = best;
	return best;
}

bool
sift_vk_lookup(SiftVk *vk, const SiftVkKey *k, uint32_t *woff)
{
	Ent		   *e = ent_find(vk, k);
	Slab	   *s;

	if (!e)
		return false;
	s = &vk->slab[e->slab];
	s->stamp = ++vk->clock;
	s->pin = vk->batch;
	*woff = e->slab * SLAB_WORDS + e->woff;
	return true;
}

static bool
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

bool
sift_vk_stage(SiftVk *vk, const SiftVkKey *k, const void *words, uint32_t nwords, uint32_t *woff)
{
	int64_t		s;
	uint64_t	bytes = (uint64_t) nwords * 4;

	if (vk->lost || nwords > SLAB_WORDS || !settle(vk))
		return false;
	if (vk->up + bytes > UP_CAP)
		return false;
	if (vk->ncopy == vk->capcopy)
	{
		uint32_t	n = vk->capcopy ? vk->capcopy * 2 : 256;
		Copy	   *c = realloc(vk->copy, n * sizeof(Copy));
		SiftVkKey  *p = realloc(vk->pend, n * sizeof(SiftVkKey));

		if (c)
			vk->copy = c;
		if (p)
			vk->pend = p;
		if (!c || !p)
			return false;
		vk->capcopy = vk->cappend = n;
	}
	s = slab_for(vk, nwords);
	if (s < 0 || !ent_put(vk, k, (uint32_t) s, vk->slab[s].used))
		return false;
	vk->slab[s].stamp = ++vk->clock;
	vk->slab[s].pin = vk->batch;
	*woff = (uint32_t) s * SLAB_WORDS + vk->slab[s].used;
	memcpy((char *) vk->stage.map + vk->up, words, bytes);
	vk->copy[vk->ncopy++] = (Copy) {vk->up, (uint64_t) *woff * 4, bytes};
	vk->pend[vk->npend++] = *k;
	vk->up += (bytes + 15) & ~(uint64_t) 15;
	vk->slab[s].used += (nwords + 3) & ~3u;
	return true;
}

void
sift_vk_abandon(SiftVk *vk)
{
	for (uint32_t i = 0; i < vk->npend; i++)
		ent_drop(vk, &vk->pend[i]);
	vk->npend = vk->ncopy = 0;
	vk->up = 0;
	vk->batch++;
}

/* ---------- running a job ---------- */

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

/* ---------- open, close ---------- */

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

/* A cache of about `bytes` (halved until the device has room); the caller owns the previous one, which stays when this fails */
/* A cache of about `bytes` (halved until the device has room); the caller owns the previous one, which stays when this fails */
static bool
make_cache(SiftVk *vk, uint64_t bytes)
{
	uint64_t	max = vk->props.limits.maxStorageBufferRange;

	if (max > 0xFFFFFFFFull * 4)
		max = 0xFFFFFFFFull * 4;
	if (bytes > max)
		bytes = max;
	if (bytes < (uint64_t) MIN_SLABS * SLAB_BYTES)
		bytes = (uint64_t) MIN_SLABS * SLAB_BYTES;
	vk->err[0] = '\0';
	for (uint32_t ns = (uint32_t) (bytes / SLAB_BYTES); ns >= MIN_SLABS; ns /= 2)
	{
		Slab	   *sl = calloc(ns, sizeof(Slab));
		VkDescriptorBufferInfo bi;
		VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vk->ds, .dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

		if (!sl)
		{
			snprintf(vk->err, sizeof(vk->err), "out of memory");
			return false;
		}
		if (!buf_new(vk, &vk->arena, (uint64_t) ns * SLAB_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_DEVICE))
		{
			free(sl);
			if (vk->lost)
				return false;
			continue;
		}
		bi = (VkDescriptorBufferInfo) {vk->arena.b, 0, VK_WHOLE_SIZE};
		w.pBufferInfo = &bi;
		vk->vkUpdateDescriptorSets(vk->dev, 1, &w, 0, NULL);
		vk->slab = sl;
		vk->nslab = ns;
		vk->cur = -1;
		free(vk->ent);
		vk->ent = NULL;
		vk->nent = vk->used = 0;
		if (!ent_rebuild(vk, 1024))
		{
			snprintf(vk->err, sizeof(vk->err), "out of memory");
			return false;
		}
		return true;
	}
	if (!vk->err[0])
		snprintf(vk->err, sizeof(vk->err), "no room for a cache of %llu MB on the device", (unsigned long long) (bytes >> 20));
	return false;
}

bool
sift_vk_set_cache(SiftVk *vk, uint64_t bytes)
{
	Buf			old = vk->arena;
	Slab	   *oslab = vk->slab;
	uint32_t	ons = vk->nslab;
	Ent		   *oent = vk->ent;
	uint32_t	onent = vk->nent,
				oused = vk->used;

	if (vk->lost || !settle(vk))
		return false;
	sift_vk_abandon(vk);
	vk->arena = (Buf) {0};
	vk->slab = NULL;
	vk->nslab = 0;
	if (!make_cache(vk, bytes))
	{
		/* make_cache touches the entry table only once the new buffer exists */
		vk->arena = old;
		vk->slab = oslab;
		vk->nslab = ons;
		if (!vk->ent)
		{
			vk->ent = oent;
			vk->nent = onent;
			vk->used = oused;
		}
		return false;
	}
	buf_free(vk, &old);
	free(oslab);
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
