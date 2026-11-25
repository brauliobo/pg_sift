/*
 * agg/vk/test.c - the hooks of the tests (tests/main.c): the switch of the fast kernels, and the decoded column the device holds for a chunk, read back and rebuilt as
 * the values and the validity of its rows, so that a test compares the decoder of the device with the decoder of the codec value by value.
 */
#include <stdio.h>
#include <string.h>

#include "agg/vk/int.h"

#ifdef SIFT_VULKAN

void
sift_vk_test_nofast(SiftVk *vk, bool on)
{
	vk->no_fast = on;
}

void
sift_vk_test_verify_tabs(SiftVk *vk, bool on)
{
	vk->verify_tabs = on;
}

bool
sift_vk_test_dcol(SiftVk *vk, const SiftVkKey *k, int64_t *vals, uint8_t *valid)
{
	Ent		   *e;
	Slot	   *s = &vk->slot[0];
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->ccb};
	VkBufferCopy cp;
	const uint32_t *d;
	uint32_t	nrows,
				w,
				dw,
				vw = 0;
	const uint8_t *m;
	const uint32_t *vbits = NULL;
	uint64_t	base;

	if (!settle_all(vk))
		return false;
	e = cache_find(vk, k);
	if (!e)
		return false;
	d = e->desc;
	nrows = d[5];
	w = d[4];
	base = ((uint64_t) d[3] << 32) | d[2];
	dw = w ? (nrows * w + 7) / 8 * 2 : 0;
	if (d[1] != SIFT_VK_DC_ALL_VALID && d[1] != SIFT_VK_DC_ALL_NULL)
		vw = (nrows + 31) / 32;
	cp = (VkBufferCopy) {(uint64_t) d[0] * 4, 0, ((uint64_t) (dw + vw + 2) * 4 + 7) & ~7ull};
	if (vk->vkResetCommandBuffer(s->ccb, 0) != VK_SUCCESS || vk->vkBeginCommandBuffer(s->ccb, &bi) != VK_SUCCESS)
		return false;
	vk->vkCmdCopyBuffer(s->ccb, vk->arena.b, vk->rhost.b, 1, &cp);
	vk->vkEndCommandBuffer(s->ccb);
	if (vk->vkQueueSubmit(vk->cq, 1, &si, s->fence) != VK_SUCCESS)
		return false;
	s->busy = true;
	vk->inflight = true;
	if (!wait_slot(vk, s, NULL))
		return false;
	m = vk->rhost.map;
	if (vw)
		vbits = (const uint32_t *) (m + (uint64_t) dw * 4);
	for (uint32_t r = 0; r < nrows; r++)
	{
		uint64_t	f = 0;

		if (w == 1)
			f = m[r];
		else if (w == 2)
			f = ((const uint16_t *) m)[r];
		else if (w == 4)
			f = ((const uint32_t *) m)[r];
		else if (w == 8)
			f = ((const uint64_t *) m)[r];
		vals[r] = (int64_t) (base + f);
		valid[r] = d[1] == SIFT_VK_DC_ALL_VALID ? 1 : d[1] == SIFT_VK_DC_ALL_NULL ? 0 : (uint8_t) ((vbits[r >> 5] >> (r & 31)) & 1);
	}
	return true;
}

#endif							/* SIFT_VULKAN */
