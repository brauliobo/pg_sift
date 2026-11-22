/*
 * agg/vk/tab.c - the descriptor tables kept on the device. The descriptors of the decoded columns of a range of granules (what the host
 * looks up chunk by chunk to build a batch) are written to a device buffer when a job ends well, named by the key of the job
 * (SiftVkTabKey). The next job with that key copies its rows into the control words on the device and looks nothing up: a warm scan of
 * thousands of granules costs the host the header and the granule table, not a hash lookup per chunk. A table is good while no decoded chunk
 * has left the cache since it was made (vk->evict_gen), because the descriptors are word offsets into the cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/job.h"

#ifdef SIFT_VULKAN

static uint64_t
tab_bytes(uint32_t ngran, uint32_t ncols)
{
	return (uint64_t) ngran * ncols * 32;
}

Tab *
tab_find(SiftVk *vk, const SiftVkJob *job)
{
	if (job->tab.rel == 0)
		return NULL;
	for (int i = 0; i < TAB_MAX; i++)
	{
		Tab		   *t = &vk->tab[i];

		if (t->buf.b && t->gen == vk->evict_gen && t->ngran == job->ngran && t->ncols == job->ncols && memcmp(&t->key, &job->tab, sizeof(t->key)) == 0)
		{
			t->used = ++vk->tab_clock;
			return t;
		}
	}
	return NULL;
}

void
tab_touch(SiftVk *vk, Tab *t)
{
	for (uint32_t i = 0; i < t->nslabs; i++)
	{
		Slab	   *s = &vk->slab[t->slabs[i]];

		s->stamp = ++vk->clock;
		s->pin = vk->batch;
	}
}

static void
tab_free(SiftVk *vk, Tab *t)
{
	buf_free(vk, &t->buf);
	free(t->slabs);
	free(t->host);
	memset(t, 0, sizeof(*t));
}

void
free_tabs(SiftVk *vk)
{
	for (int i = 0; i < TAB_MAX; i++)
		tab_free(vk, &vk->tab[i]);
}

/* the slabs the rows name: the chunks of a column that has none (all null) name none */
static bool
slabs_of(const SiftVk *vk, const uint32_t *host, uint64_t nrows, Tab *t)
{
	uint8_t    *seen = calloc(vk->nslab, 1);

	t->slabs = malloc((size_t) vk->nslab * sizeof(uint32_t));
	if (!seen || !t->slabs)
	{
		free(seen);
		return false;
	}
	for (uint64_t r = 0; r < nrows; r++)
	{
		uint32_t	slab = host[r * 8] / SLAB_WORDS;

		if (host[r * 8 + 5] != 0 && !seen[slab])
		{
			seen[slab] = 1;
			t->slabs[t->nslabs++] = slab;
		}
	}
	free(seen);
	return true;
}

/* a stale table is older than a fresh one, then the less recently used */
static bool
tab_older(const SiftVk *vk, const Tab *a, const Tab *b)
{
	bool		sa = a->gen != vk->evict_gen,
				sb = b->gen != vk->evict_gen;

	return sa != sb ? sa : a->used < b->used;
}

/* a free entry, once the tables that would not leave room for bytes are freed, the oldest first */
static Tab *
tab_slot(SiftVk *vk, uint64_t bytes)
{
	for (;;)
	{
		Tab		   *none = NULL,
				   *oldest = NULL;
		uint64_t	held = 0;

		for (int i = 0; i < TAB_MAX; i++)
		{
			Tab		   *t = &vk->tab[i];

			held += t->buf.size;
			if (!t->buf.b)
				none = none ? none : t;
			else if (!oldest || tab_older(vk, t, oldest))
				oldest = t;
		}
		if (none && held + bytes <= TAB_BYTES_MAX)
			return none;
		tab_free(vk, oldest);
	}
}

/* the copy of the table from the staging area to its buffer, waited for */
static bool
tab_upload(SiftVk *vk, const Tab *t, const uint32_t *host, uint64_t bytes)
{
	Slot	   *s = &vk->slot[0];
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->ccb};
	VkBufferCopy cp = {0, 0, bytes};
	sigset_t	old;
	VkResult	r;

	memcpy(s->stage.map, host, bytes);
	CK(vk->vkResetCommandBuffer(s->ccb, 0));
	CK(vk->vkBeginCommandBuffer(s->ccb, &bi));
	vk->vkCmdCopyBuffer(s->ccb, s->stage.b, t->buf.b, 1, &cp);
	CK(vk->vkEndCommandBuffer(s->ccb));
	sigs_block(&old);
	r = vk->vkQueueSubmit(vk->cq, 1, &si, s->fence);
	sigs_restore(&old);
	if (r != VK_SUCCESS)
		return fail(vk, "vkQueueSubmit", r);
	s->busy = true;
	vk->inflight = true;
	return wait_slot(vk, s, NULL);
}

bool
tab_store(SiftVk *vk, const SiftVkJob *job, const uint32_t *host, uint64_t gen)
{
	uint64_t	bytes = tab_bytes(job->ngran, job->ncols);
	Tab		   *t;

	if (bytes > UP_CAP || bytes > TAB_BYTES_MAX)
		return true;
	for (int i = 0; i < TAB_MAX; i++)
		if (vk->tab[i].buf.b && memcmp(&vk->tab[i].key, &job->tab, sizeof(job->tab)) == 0)
			tab_free(vk, &vk->tab[i]);
	t = tab_slot(vk, bytes);
	t->key = job->tab;
	t->ngran = job->ngran;
	t->ncols = job->ncols;
	t->gen = gen;
	t->used = ++vk->tab_clock;
	if (!buf_new(vk, &t->buf, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_DEVICE) ||
		!slabs_of(vk, host, (uint64_t) job->ngran * job->ncols, t) || !tab_upload(vk, t, host, bytes))
	{
		tab_free(vk, t);
		return false;
	}
	if (vk->verify_tabs && (t->host = malloc(bytes)) != NULL)
		memcpy(t->host, host, bytes);
	return true;
}

/* tests: the rows of the table against the chunks the cache holds now, granule by granule */
bool
tab_verify(SiftVk *vk, const SiftVkJob *job, const Tab *t)
{
	if (!t->host)
		return true;
	for (uint32_t g = 0; g < job->ngran; g++)
		for (uint32_t c = 0; c < job->ncols; c++)
		{
			const SiftVkChunk *ch = job->gran[g].col[c];
			const Ent  *e = ch ? cache_find(vk, &ch->key) : NULL;
			uint32_t	want[8] = {0, SIFT_VK_DC_ALL_NULL, 0, 0, 0, 0, 0, 0};

			if (ch && !e)
				return false;
			if (e)
				memcpy(want, e->desc, 32);
			if (memcmp(want, t->host + ((size_t) g * job->ncols + c) * 8, 32) != 0)
			{
				snprintf(vk->err, sizeof(vk->err), "the descriptor table differs from the chunks at granule %u, column %u", g, c);
				return false;
			}
		}
	return true;
}

#endif							/* SIFT_VULKAN */
