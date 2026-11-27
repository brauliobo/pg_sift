/*
 * test_vk_bufs - the job buffers of the device layer (agg/vk/run.c) with the allocator of the driver faked: a job whose hash table, result or read back cannot be made leaves no
 * buffer that claims room it does not have, so the next smaller job makes it again and binds the buffers it holds (never a destroyed one), and reads its result from a mapped read back.
 */
#include "postgres.h"

#include "agg/vk/job.h"
#include "test/common/check.h"

enum { NBUF = 64, BIND_HT = 10, BIND_OUT = 11 };

static bool live[NBUF];
static uint64_t sizes[NBUF];
static uint64_t fail_size;
static uintptr_t next_id;
static VkBuffer bound_ht, bound_out;
static char mapped[64];

static VkResult
fake_create(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *b)
{
	live[++next_id] = true;
	sizes[next_id] = ci->size;
	*b = (VkBuffer) next_id;
	return VK_SUCCESS;
}

static void
fake_destroy(VkDevice dev, VkBuffer b, const VkAllocationCallbacks *a)
{
	SIFT_CHECK(live[(uintptr_t) b]);
	live[(uintptr_t) b] = false;
}

static void
fake_reqs(VkDevice dev, VkBuffer b, VkMemoryRequirements *mr)
{
	*mr = (VkMemoryRequirements) {.size = sizes[(uintptr_t) b], .alignment = 256, .memoryTypeBits = 1};
}

static VkResult
fake_alloc(VkDevice dev, const VkMemoryAllocateInfo *ma, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
	if (ma->allocationSize == fail_size)
		return VK_ERROR_DEVICE_LOST;
	*m = (VkDeviceMemory) ++next_id;
	return VK_SUCCESS;
}

static void
fake_free(VkDevice dev, VkDeviceMemory m, const VkAllocationCallbacks *a)
{
}

static VkResult
fake_bind(VkDevice dev, VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
	return VK_SUCCESS;
}

static VkResult
fake_map(VkDevice dev, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags f, void **p)
{
	*p = mapped;
	return VK_SUCCESS;
}

static void
fake_unmap(VkDevice dev, VkDeviceMemory m)
{
}

static void
fake_update(VkDevice dev, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
	bound_ht = w[BIND_HT].pBufferInfo->buffer;
	bound_out = w[BIND_OUT].pBufferInfo->buffer;
}

static SiftVk *
device(void)
{
	SiftVk	   *vk = calloc(1, sizeof(*vk));

	memset(live, 0, sizeof(live));
	next_id = 0;
	fail_size = 0;
	bound_ht = bound_out = VK_NULL_HANDLE;
	vk->vkCreateBuffer = fake_create;
	vk->vkDestroyBuffer = fake_destroy;
	vk->vkGetBufferMemoryRequirements = fake_reqs;
	vk->vkAllocateMemory = fake_alloc;
	vk->vkFreeMemory = fake_free;
	vk->vkBindBufferMemory = fake_bind;
	vk->vkMapMemory = fake_map;
	vk->vkUnmapMemory = fake_unmap;
	vk->vkUpdateDescriptorSets = fake_update;
	vk->mp.memoryTypeCount = 1;
	vk->mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	vk->res.b = vk->arena.b = vk->slot[0].work.b = (VkBuffer) (uintptr_t) (NBUF - 1);
	vk->res_words = RES_CAP_WORDS;
	return vk;
}

static bool
job(SiftVk *vk, uint64_t ht, uint64_t out)
{
	return ensure_buffers(vk, &(Plan) {.ht_bytes = ht, .out_bytes = out});
}

/* the next small job binds the hash table and the result the device holds, both made, and its read back is mapped */
static void
check_small_job_after(uint64_t failing)
{
	SiftVk	   *vk = device();

	SIFT_CHECK(job(vk, 64 << 10, 64 << 10));
	SIFT_CHECK(bound_ht == vk->ht.b && bound_out == vk->out.b);
	fail_size = failing;
	SIFT_CHECK(!job(vk, 16 << 20, 12 << 20));
	fail_size = 0;
	SIFT_CHECK(job(vk, 32 << 10, 32 << 10));
	SIFT_CHECK(vk->ht.b != VK_NULL_HANDLE && vk->out.b != VK_NULL_HANDLE && vk->hout.map != NULL);
	SIFT_CHECK(bound_ht == vk->ht.b && live[(uintptr_t) bound_ht]);
	SIFT_CHECK(bound_out == vk->out.b && live[(uintptr_t) bound_out]);
	free(vk);
}

static void
test_result_that_cannot_be_made(void)
{
	check_small_job_after(12 << 20);
}

static void
test_read_back_that_cannot_be_made(void)
{
	check_small_job_after(HOUT_CHUNK);
}

static void
test_hash_table_that_cannot_be_made(void)
{
	check_small_job_after(16 << 20);
}

int
main(void)
{
	test_result_that_cannot_be_made();
	test_read_back_that_cannot_be_made();
	test_hash_table_that_cannot_be_made();
	puts("test_vk_bufs: ok");
	return 0;
}
