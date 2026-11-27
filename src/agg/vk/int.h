/*
 * agg/vk/int.h - the Vulkan backend, inside agg/vk/ (the interface is agg/vk/vk.h)
 */
#ifndef SIFT_AGG_VK_DIR_INT_H
#define SIFT_AGG_VK_DIR_INT_H

#include <signal.h>

#include "agg/vk/vk.h"
#include "agg/vk/fast.h"
#include "agg/vk/layout.h"

#ifdef SIFT_VULKAN

#include "agg/vk/min.h"

#define VK_INSTANCE_FNS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
	X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VK_DEVICE_FNS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
	X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
	X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
	X(vkCmdCopyBuffer) X(vkCmdFillBuffer) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
	X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) \
	X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkDeviceWaitIdle)

#define SLAB_BYTES		SIFT_VK_SLAB_BYTES
#define SLAB_WORDS		((uint32_t) (SLAB_BYTES / 4))
#define MIN_SLABS		4
/* MEM[MEM-VK-10]: slot staging stage (UP_CAP=4MiB pinned host per slot, 8MiB for both; was 16MiB per slot) */
#define UP_CAP			((uint64_t) 4 << 20)		/* a slot: chunk bytes copied to the device */
#define UNW_CAP			((uint64_t) 16 << 20)		/* ... and the bytes the LZ4 chunks of the batch unwrap to */
#define SLACK			64
#define CTL_CAP			((uint64_t) 4 << 20)		/* a slot: the control words of a batch */
#define DJ_MAX			8192						/* decode jobs of a batch */
#define BATCH_GRAN_MAX	2048
#define RES_CAP_WORDS	((uint64_t) SIFT_VK_RES_ACC_OFF + ((uint64_t) 1 << 20))
#define RES_MIN_WORDS	((uint64_t) SIFT_VK_RES_ACC_OFF + 8192)	/* the result buffers at open: the words of the decoder and room for a scalar or small grouped result */
/* MEM[MEM-VK-04]: slot work buffer (UP_CAP + UNW_CAP + DEC_WGS * 896KiB = 48MiB per slot, made at its first batch; was 104MiB per slot at open) */
#define DEC_WGS			32							/* the workgroups of the decoder: each takes jobs one after the other */
#define HT_RETAIN_MAX	((uint64_t) 16 << 20)		/* the hash table, the result and its read back kept between jobs: more is freed when the job ends */
#define OUT_RETAIN_MAX	((uint64_t) 8 << 20)
#define HOUT_CHUNK		((uint64_t) 8 << 20)		/* the pinned bytes of the read back of a result: a larger one is gathered chunk by chunk */
#define AGG_WGS_MAX		512
#define WAIT_SLICE_NS	5000000ull
#define WAIT_TOTAL_MS	10000
#define NSLOT			2
#define FAST_CACHE		32							/* pipelines of fast kernels kept */
#define TAB_MAX			64							/* descriptor tables kept */
#define TAB_BYTES_MAX	((uint64_t) 64 << 20)		/* ... in all */

typedef struct Buf
{
	VkBuffer	b;
	VkDeviceMemory m;
	void	   *map;
	uint64_t	size;
	bool		host;			/* pinned host memory: counted in vk->accounted */
} Buf;

#define VK_ENT_NONE		UINT32_MAX

typedef struct Slab
{
	uint32_t	used;			/* words handed out */
	uint32_t	ent_head;		/* the first of the entries the slab holds (VK_ENT_NONE: none), a list through Ent.slab_next */
	uint32_t	ent_count;
	uint64_t	stamp;			/* last use */
	uint64_t	pin;			/* the batch that last used it: not evictable until that batch ended */
	uint64_t	gen;			/* bumped when the slab is evicted: a table that names it from before is stale */
} Slab;

typedef struct Ent
{
	SiftVkKey	key;
	uint32_t	desc[8];		/* what the shaders read of a decoded column */
	uint32_t	hash;			/* of key: where the entry belongs, which the deletion of another entry asks */
	uint32_t	slab;
	uint32_t	slab_prev;		/* the entries of one slab are a doubly linked list */
	uint32_t	slab_next;
	uint8_t		used;			/* 0 empty, 1 holds an entry: no cell is ever deleted but emptied */
} Ent;

typedef struct FastPipe
{
	int			pl;
	uint32_t	sc[FAST_NSC];
	VkPipeline	pipe;
	uint64_t	used;
} FastPipe;

/* the descriptors of the decoded columns of a range of granules (the rows dt of a batch), kept on the device while the chunks they name stay in the cache */
typedef struct Tab
{
	SiftVkTabKey key;
	uint32_t	ngran, ncols;
	uint64_t	gen;			/* vk->evict_gen when it was made: an eviction while its job ran keeps it from being stored */
	uint64_t	used;			/* last use */
	Buf			buf;			/* ngran * ncols * 8 words, as the shaders read them */
	uint32_t   *slabs;			/* the slabs of the chunks it names, to keep them young */
	uint64_t   *sgen;			/* Slab.gen of each when it was made: the eviction of one makes the table stale */
	uint32_t	nslabs;
	uint32_t   *host;			/* tests: the table as the host built it */
} Tab;

typedef struct Slot
{
	Buf			stage, work, ctl, dj, rstat;
	VkCommandBuffer tcb, ccb;
	VkSemaphore sem;
	VkFence		fence;
	VkDescriptorSet ds;
	VkQueryPool qp;
	bool		busy;
	bool		stamps;
	uint32_t	njobs;
	uint64_t	up;				/* bytes of the staging buffer in use */
	uint64_t	unw;
	SiftVkKey  *pend;			/* the cache entries made for the batch, by key: an entry moves in the table when another leaves it */
	uint32_t	npend;
	uint32_t	cappend;
} Slot;

enum { PL_LZ4, PL_DECODE, PL_UNGROUPED, PL_DENSE, PL_HASH, PL_COMPACT, PL_HT_TOPK, PL_TOPN, PL_FAST_UNGROUPED, PL_FAST_DENSE, PL_FAST_HASH, PL_N };
#define PL_FIXED		PL_FAST_UNGROUPED		/* the pipelines before it are built at open; the fast ones per shape */
enum { K_DEVICE, K_REBAR, K_HOST, K_READBACK };

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
	VkQueue		cq, tq;
	uint32_t	cf, tf;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mp;
	SiftVkCaps	caps;
	bool		lost, inflight, stamps, split_queues, test_lost, test_timeout, usable;
	bool		cancel;			/* the run in progress left a wait because its query was cancelled: the submissions it made are in flight, the next run settles them */
	char		err[256];
	void		(*account) (void *arg, int64_t delta);
	void	   *account_arg;
	int64_t		accounted;
	VkCommandPool cpool, tpool;
	VkShaderModule mod[PL_N];
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline	pipe[PL_N];
	VkDescriptorPool dpool;
	Slot		slot[NSLOT];
	FastPipe	fast[FAST_CACHE];
	Tab			tab[TAB_MAX];
	uint64_t	tab_clock;
	uint64_t	evict_gen;		/* bumped when a decoded chunk the cache held is gone: the tables made before are stale */
	bool		verify_tabs;	/* test: a hit is compared with the chunks */
	bool		no_fast;		/* test: every job runs the expression machine */
	Buf			arena, res, rinit, rhost, ht, out, hout;
	bool		unbound;		/* ht or out was made again and the descriptor sets do not name it yet */
	uint64_t	res_words;		/* the words res, rinit and rhost hold */
	void	   *rbuf;			/* a result larger than hout, gathered from it in chunks */
	uint64_t	rbuf_cap;
	bool		released;		/* sift_vk_release: the cache is gone, the next job makes it again */
	bool		trimmed;		/* the buffers of the slots and of the result are gone: the next job makes them again */
	uint64_t	cache_want;		/* the bytes of cache the device was asked for: the ceiling, caps.cache_bytes is what that came to */
	uint64_t	envelope, fixed;	/* sift_vk_set_envelope */
	void	   *topn_buf;		/* the candidates of the top N of the last job (16 bytes each) */
	uint64_t	topn_cap;		/* in candidates */
	/* the cache */
	uint32_t	nslab;
	Slab	   *slab;
	int64_t		cur;			/* the slab being filled, -1 none */
	uint64_t	clock;
	uint64_t	batch;			/* the batch in preparation (pins) */
	uint64_t	done_batch;		/* the last batch that ended: its pins are gone */
	Ent		   *ent;
	uint32_t	nent;			/* power of two */
	uint32_t	nent_max;		/* the table doubles up to here when it is full */
	uint32_t	used;			/* the entries the cache holds */
	SiftVkResult last;
};

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) return fail(vk, #x, r_); } while (0)

/* mem.c */
bool fail(SiftVk *vk, const char *what, VkResult r);
double now_ms(void);
void sigs_block(sigset_t *old);
void sigs_restore(const sigset_t *old);
void buf_free(SiftVk *vk, Buf *b);
bool buf_new(SiftVk *vk, Buf *b, uint64_t size, VkBufferUsageFlags usage, int kind);
void account(SiftVk *vk, int64_t delta);
uint64_t heap_budget_free(SiftVk *vk);

/* pipe.c */
bool make_pipelines(SiftVk *vk);
void free_pipelines(SiftVk *vk);
bool make_slots(SiftVk *vk);
void free_slots(SiftVk *vk);
bool bind_buffers(SiftVk *vk);
bool slots_alloc(SiftVk *vk);
void slots_release(SiftVk *vk);
bool ensure_work(SiftVk *vk, Slot *s);
bool ensure_res(SiftVk *vk, uint64_t words);

/* fast.c */
VkPipeline fast_pipeline(SiftVk *vk, int pl, const FastShape *fs);
void free_fast(SiftVk *vk);

/* cache.c */
bool make_cache(SiftVk *vk, uint64_t bytes);
bool revive(SiftVk *vk);
bool job_resident(SiftVk *vk, const SiftVkJob *job);
Ent *cache_find(SiftVk *vk, const SiftVkKey *k);
Ent *cache_insert(SiftVk *vk, const SiftVkKey *k, uint32_t words, uint32_t *woff);
void cache_remove(SiftVk *vk, Ent *e);
void cache_remove_key(SiftVk *vk, const SiftVkKey *k);
void cache_touch(SiftVk *vk, Ent *e);

/* tab.c */
Tab *tab_find(SiftVk *vk, const SiftVkJob *job);
bool tab_store(SiftVk *vk, const SiftVkJob *job, const uint32_t *host, uint64_t gen);
bool tab_verify(SiftVk *vk, const SiftVkJob *job, const Tab *t);
void tab_touch(SiftVk *vk, Tab *t);
void free_tabs(SiftVk *vk);
void reap_tabs(SiftVk *vk);

/* run.c */
void trim_job_buffers(SiftVk *vk);
bool settle_all(SiftVk *vk);
bool wait_slot(SiftVk *vk, Slot *s, bool (*cancelled) (void));

#endif							/* SIFT_VULKAN */

#endif
