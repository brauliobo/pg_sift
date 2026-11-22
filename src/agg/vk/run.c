/*
 * agg/vk/run.c - the run of a job: the granules in batches through two slots, so that the staging and the copy of one batch (on the
 * transfer queue when the device has one of its own) overlap the decode and the aggregation of the one before (on the compute queue);
 * the waits, cut in slices that call the interrupt; the finish (the compaction of the hash table, the read back); the top N in two passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/nth.h"
#include "agg/vk/job.h"

#ifdef SIFT_VULKAN

typedef struct RunCtx
{
	SiftVk	   *vk;
	const SiftVkJob *job;
	Plan		plan;
	void		(*interrupt) (void);
	SiftVkResult *res;
	Batch		bt[NSLOT];
} RunCtx;

bool
wait_slot(SiftVk *vk, Slot *s, void (*interrupt) (void))
{
	double		t0 = now_ms();
	VkResult	r;

	if (!s->busy)
		return true;
	for (;;)
	{
		r = vk->vkWaitForFences(vk->dev, 1, &s->fence, VK_TRUE, WAIT_SLICE_NS);
		if (r == VK_SUCCESS)
			break;
		if (r == VK_TIMEOUT)
		{
			if (interrupt)
				interrupt();
			if (now_ms() - t0 > WAIT_TOTAL_MS)
			{
				snprintf(vk->err, sizeof(vk->err), "a submission did not finish in %d ms", WAIT_TOTAL_MS);
				vk->usable = false;
				return false;
			}
			continue;
		}
		s->busy = false;
		vk->inflight = vk->slot[0].busy || vk->slot[1].busy;
		return fail(vk, "vkWaitForFences", r);
	}
	vk->vkResetFences(vk->dev, 1, &s->fence);
	s->busy = false;
	vk->inflight = vk->slot[0].busy || vk->slot[1].busy;
	return true;
}

bool
settle_all(SiftVk *vk)
{
	bool		ok = true;

	for (int k = 0; k < NSLOT; k++)
		ok = wait_slot(vk, &vk->slot[k], NULL) && ok;
	return ok;
}

static void
barrier(SiftVk *vk, VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da)
{
	VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};

	vk->vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

#define CS VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
#define TR VK_PIPELINE_STAGE_TRANSFER_BIT
#define SRW (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)

static void
dispatch(SiftVk *vk, VkCommandBuffer cb, VkPipeline pipe, uint32_t x, const uint32_t pc[4])
{
	vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vk->vkCmdPushConstants(cb, vk->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
	vk->vkCmdDispatch(cb, x, 1, 1);
}

/* the init image of the result: no error, the accumulators of every group as a new one has them */
static void
write_init(RunCtx *rc)
{
	int64_t    *w = rc->vk->rinit.map;
	const Plan *p = &rc->plan;
	const SiftVkJob *job = rc->job;

	memset(w, 0, (SIFT_VK_RES_ACC_OFF + p->acc_words) * 8);
	w[SIFT_VK_RES_ERR] = INT64_MAX;
	if (job->mode != SIFT_VK_UNGROUPED && job->mode != SIFT_VK_DENSE)
		return;
	for (uint32_t g = 0; g < (job->mode == SIFT_VK_DENSE ? p->groups : 1); g++)
	{
		int64_t    *a = w + SIFT_VK_RES_ACC_OFF + (size_t) g * (1 + 3 * job->naggs);

		for (uint32_t i = 0; i < job->naggs; i++)
		{
			uint8_t		k = job->agg[i].kind;

			a[1 + 3 * i] = (k == SIFT_VK_AK_MIN || k == SIFT_VK_AK_MIN_F) ? INT64_MAX : (k == SIFT_VK_AK_MAX || k == SIFT_VK_AK_MAX_F) ? INT64_MIN : 0;
		}
	}
}

static bool
submit(SiftVk *vk, Slot *s, bool split)
{
	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->tcb, .signalSemaphoreCount = 1, .pSignalSemaphores = &s->sem};
	VkSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->ccb};
	sigset_t	old;
	VkResult	r;

	if (split)
	{
		cs.waitSemaphoreCount = 1;
		cs.pWaitSemaphores = &s->sem;
		cs.pWaitDstStageMask = &wait;
	}
	sigs_block(&old);
	r = split ? vk->vkQueueSubmit(vk->tq, 1, &ts, VK_NULL_HANDLE) : VK_SUCCESS;
	if (r == VK_SUCCESS)
		r = vk->vkQueueSubmit(vk->cq, 1, &cs, s->fence);
	sigs_restore(&old);
	if (r != VK_SUCCESS)
		return fail(vk, "vkQueueSubmit", r);
	s->busy = true;
	vk->inflight = true;
	return true;
}

/* the commands of a batch: the staged bytes to the device, the unwrap and the decode of the chunks missing from the cache, the kernel;
 * the first one starts from the init image, the last one ends with the finish of the job */
static bool
record_batch(RunCtx *rc, Batch *b, bool first, bool last)
{
	SiftVk	   *vk = rc->vk;
	const SiftVkJob *job = rc->job;
	const Plan *p = &rc->plan;
	Slot	   *s = b->s;
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	uint32_t	scr_u64 = (uint32_t) ((UP_CAP + UNW_CAP) / 8),
				scr_stride = (uint32_t) (SIFT_VK_DEC_SCR_BYTES / 8),
				ndec = b->njobs < DEC_WGS ? b->njobs : DEC_WGS;
	uint32_t	pc_dec[4] = {b->njobs, scr_u64, scr_stride, 0},
				pc_agg[4] = {0, 0, 0, 0};
	uint32_t	nagg = b->ng < AGG_WGS_MAX ? b->ng : AGG_WGS_MAX;
	VkBufferCopy cp;
	VkCommandBuffer cb = s->ccb;

	if (vk->split_queues)
	{
		CK(vk->vkResetCommandBuffer(s->tcb, 0));
		CK(vk->vkBeginCommandBuffer(s->tcb, &bi));
		if (s->up)
		{
			cp = (VkBufferCopy) {0, 0, (s->up + 7) & ~7ull};
			vk->vkCmdCopyBuffer(s->tcb, s->stage.b, s->work.b, 1, &cp);
		}
		CK(vk->vkEndCommandBuffer(s->tcb));
	}
	CK(vk->vkResetCommandBuffer(cb, 0));
	CK(vk->vkBeginCommandBuffer(cb, &bi));
	if (s->stamps)
		vk->vkCmdResetQueryPool(cb, s->qp, 0, 4);
	if (!vk->split_queues && s->up)
	{
		cp = (VkBufferCopy) {0, 0, (s->up + 7) & ~7ull};
		vk->vkCmdCopyBuffer(cb, s->stage.b, s->work.b, 1, &cp);
	}
	if (first)
	{
		uint64_t	n = (SIFT_VK_RES_ACC_OFF + (uint64_t) p->acc_words) * 8;

		cp = (VkBufferCopy) {0, 0, n};
		vk->vkCmdCopyBuffer(cb, vk->rinit.b, vk->res.b, 1, &cp);
		if (job->mode == SIFT_VK_HASH)
			vk->vkCmdFillBuffer(cb, vk->ht.b, 0, p->ht_bytes, 0);
		barrier(vk, cb, TR, VK_ACCESS_TRANSFER_WRITE_BIT, TR, VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	else
		barrier(vk, cb, CS | TR, SRW | VK_ACCESS_TRANSFER_READ_BIT, TR, VK_ACCESS_TRANSFER_WRITE_BIT);
	if (b->njobs)
		vk->vkCmdFillBuffer(cb, vk->res.b, (uint64_t) SIFT_VK_RES_DEC_OFF * 8, (uint64_t) b->njobs * 8, 0);
	if (b->tab_bytes)
	{
		cp = (VkBufferCopy) {b->tab_src, b->tab_dst, b->tab_bytes};
		vk->vkCmdCopyBuffer(cb, b->tab_buf, s->ctl.b, 1, &cp);
	}
	barrier(vk, cb, TR, VK_ACCESS_TRANSFER_WRITE_BIT, CS, SRW);
	if (s->stamps)
		vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, 0);
	vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vk->pl, 0, 1, &s->ds, 0, NULL);
	if (b->njobs)
	{
		dispatch(vk, cb, vk->pipe[PL_LZ4], b->njobs, pc_dec);
		barrier(vk, cb, CS, SRW, CS, SRW);
		dispatch(vk, cb, vk->pipe[PL_DECODE], ndec, pc_dec);
		barrier(vk, cb, CS, SRW, CS, SRW);
	}
	if (s->stamps)
		vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, 1);
	dispatch(vk, cb, p->kern, nagg, pc_agg);
	if (s->stamps)
		vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, 2);
	barrier(vk, cb, CS, SRW, CS | TR, SRW | VK_ACCESS_TRANSFER_READ_BIT);
	if (b->njobs)
	{
		cp = (VkBufferCopy) {(uint64_t) SIFT_VK_RES_DEC_OFF * 8, 0, (uint64_t) b->njobs * 8};
		vk->vkCmdCopyBuffer(cb, vk->res.b, s->rstat.b, 1, &cp);
	}
	if (last)
	{
		if (job->mode == SIFT_VK_HASH)
		{
			dispatch(vk, cb, vk->pipe[PL_COMPACT], (p->ht_mask + 256) / 256 < 1024 ? (p->ht_mask + 256) / 256 : 1024, pc_agg);
			barrier(vk, cb, CS, SRW, TR, VK_ACCESS_TRANSFER_READ_BIT);
		}
		cp = (VkBufferCopy) {0, 0, 64};
		vk->vkCmdCopyBuffer(cb, vk->res.b, vk->rhost.b, 1, &cp);
		if (p->acc_words)
		{
			cp = (VkBufferCopy) {(uint64_t) SIFT_VK_RES_ACC_OFF * 8, 64, (uint64_t) p->acc_words * 8};
			vk->vkCmdCopyBuffer(cb, vk->res.b, vk->rhost.b, 1, &cp);
		}
	}
	if (s->stamps)
		vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, 3);
	CK(vk->vkEndCommandBuffer(cb));
	return submit(vk, s, vk->split_queues);
}

/* after the fence of a batch: its statuses, its times */
static bool
finish_batch(RunCtx *rc, Batch *b)
{
	SiftVk	   *vk = rc->vk;
	SiftVkTimes *t = &rc->res->times;
	uint64_t	ts[4];

	vk->done_batch = b->id;
	rc->res->chunks_resident += b->chunks_resident;
	rc->res->chunks_uploaded += b->chunks_uploaded;
	rc->res->bytes_uploaded += b->bytes_uploaded;
	rc->res->bytes_lz4 += b->bytes_lz4;
	rc->res->batches++;
	t->stage_ms += b->stage_ms;
	if (b->s->stamps && vk->vkGetQueryPoolResults(vk->dev, b->s->qp, 0, 4, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
	{
		double		per = vk->props.limits.timestampPeriod / 1e6;

		t->decode_ms += (double) (ts[1] - ts[0]) * per;
		t->kernel_ms += (double) (ts[2] - ts[1]) * per;
	}
	return batch_check(vk, b);
}

static bool
ensure_buffers(SiftVk *vk, const Plan *p)
{
	const VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bool		rebind = false;

	if (p->ht_bytes > vk->ht_cap)
	{
		buf_free(vk, &vk->ht);
		if (!buf_new(vk, &vk->ht, p->ht_bytes, u, K_DEVICE))
			return false;
		vk->ht_cap = p->ht_bytes;
		rebind = true;
	}
	if (p->out_bytes > vk->out_cap)
	{
		buf_free(vk, &vk->out);
		if (!buf_new(vk, &vk->out, p->out_bytes, u, K_DEVICE))
			return false;
		vk->out_cap = p->out_bytes;
		rebind = true;
	}
	if (p->out_bytes > vk->hout_cap)
	{
		buf_free(vk, &vk->hout);
		if (!buf_new(vk, &vk->hout, p->out_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, K_READBACK))
			return false;
		vk->hout_cap = p->out_bytes;
	}
	return !rebind || bind_buffers(vk);
}

/* the job went over every granule: its descriptor table is kept for the next job with its key (the device having no room for it is no failure of the job), or (tests) the one it used is checked */
static SiftVkStatus
publish_tab(RunCtx *rc)
{
	SiftVk	   *vk = rc->vk;
	Plan	   *p = &rc->plan;

	if (p->tab && vk->verify_tabs && !tab_verify(vk, rc->job, p->tab))
		return SIFT_VK_FAIL;
	if (p->tab_host && vk->evict_gen == p->gen0 && !tab_store(vk, rc->job, p->tab_host, p->gen0) && (vk->lost || !vk->usable))
		return vk->lost ? SIFT_VK_LOST : SIFT_VK_TIMEOUT;
	return SIFT_VK_OK;
}

/* the granules [g0, g1) of the job through the two slots; the result words are in rhost when this returns true */
static SiftVkStatus
run_scan(RunCtx *rc, uint32_t g0, uint32_t g1)
{
	SiftVk	   *vk = rc->vk;
	int			k = 0;
	uint32_t	g = g0;
	bool		first = true;

	if (!ensure_buffers(vk, &rc->plan))
		return SIFT_VK_FAIL;
	write_init(rc);
	while (g < g1)
	{
		Slot	   *s = &vk->slot[k];
		Batch	   *b = &rc->bt[k];
		BuildStatus bs;
		bool		last;

		if (s->busy)
		{
			if (!wait_slot(vk, s, rc->interrupt))
				return vk->lost ? SIFT_VK_LOST : SIFT_VK_TIMEOUT;
			if (!finish_batch(rc, b))
				return SIFT_VK_DECODE;
		}
		vk->batch++;
		bs = batch_build(vk, s, rc->job, &rc->plan, g, g1, b);
		if (bs == BUILD_NO_ROOM && vk->slot[k ^ 1].busy)
		{
			if (!wait_slot(vk, &vk->slot[k ^ 1], rc->interrupt))
				return vk->lost ? SIFT_VK_LOST : SIFT_VK_TIMEOUT;
			if (!finish_batch(rc, &rc->bt[k ^ 1]))
				return SIFT_VK_DECODE;
			bs = batch_build(vk, s, rc->job, &rc->plan, g, g1, b);
		}
		if (bs != BUILD_OK)
			return SIFT_VK_FAIL;
		last = b->g0 + b->ng >= g1;
		if (!record_batch(rc, b, first, last))
		{
			batch_abandon(vk, b);
			return vk->lost ? SIFT_VK_LOST : SIFT_VK_FAIL;
		}
		first = false;
		g += b->ng;
		k ^= 1;
	}
	for (int i = 0; i < NSLOT; i++)
	{
		Slot	   *s = &vk->slot[k];

		if (s->busy)
		{
			if (!wait_slot(vk, s, rc->interrupt))
				return vk->lost ? SIFT_VK_LOST : SIFT_VK_TIMEOUT;
			if (!finish_batch(rc, &rc->bt[k]))
				return SIFT_VK_DECODE;
		}
		k ^= 1;
	}
	return SIFT_VK_OK;
}

/* the second read back: the groups or candidates the first one counted, from the device buffer of the job */
static bool
read_out(RunCtx *rc, uint64_t bytes, void (*interrupt) (void))
{
	SiftVk	   *vk = rc->vk;
	Slot	   *s = &vk->slot[0];
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->ccb};
	VkBufferCopy cp = {0, 0, (bytes + 7) & ~7ull};
	sigset_t	old;
	VkResult	r;

	CK(vk->vkResetCommandBuffer(s->ccb, 0));
	CK(vk->vkBeginCommandBuffer(s->ccb, &bi));
	vk->vkCmdCopyBuffer(s->ccb, vk->out.b, vk->hout.b, 1, &cp);
	CK(vk->vkEndCommandBuffer(s->ccb));
	sigs_block(&old);
	r = vk->vkQueueSubmit(vk->cq, 1, &cs, s->fence);
	sigs_restore(&old);
	if (r != VK_SUCCESS)
		return fail(vk, "vkQueueSubmit", r);
	s->busy = true;
	vk->inflight = true;
	return wait_slot(vk, s, interrupt);
}

typedef struct Cand
{
	int64_t		key;
	uint64_t	tag;
} Cand;

typedef struct CandOrder
{
	bool		desc;
	bool		nulls_first;
} CandOrder;

static inline bool
cand_null(const Cand *c)
{
	return (c->tag >> 62) & 1;
}

static int
cand_cmp(const void *pa, const void *pb, void *arg)
{
	const Cand *a = pa,
			   *b = pb;
	const CandOrder *o = arg;

	if (cand_null(a) != cand_null(b))
		return cand_null(a) == o->nulls_first ? -1 : 1;
	if (!cand_null(a) && a->key != b->key)
		return (a->key > b->key) == o->desc ? -1 : 1;
	return a->tag < b->tag ? -1 : a->tag > b->tag;
}

static SiftVkStatus
finish_result(RunCtx *rc, SiftVkResult *res)
{
	SiftVk	   *vk = rc->vk;
	const SiftVkJob *job = rc->job;
	const Plan *p = &rc->plan;
	const int64_t *h = vk->rhost.map;
	uint64_t	n;

	if (h[SIFT_VK_RES_ERR] != INT64_MAX)
	{
		uint64_t	e = (uint64_t) h[SIFT_VK_RES_ERR];

		res->has_err = true;
		res->err_gran = (uint32_t) (e >> 40);
		res->err_row = (uint32_t) ((e >> 24) & 0xFFFF);
		res->err_op = (uint32_t) ((e >> 16) & 0xFF);
		res->err_code = (uint32_t) (e & 0xFFFF);
		return SIFT_VK_PROG_ERROR;
	}
	if (job->mode == SIFT_VK_UNGROUPED || job->mode == SIFT_VK_DENSE)
	{
		res->acc = h + 8;
		res->acc_words = p->acc_words;
		res->ngroups = job->mode == SIFT_VK_DENSE ? p->groups : 1;
		return SIFT_VK_OK;
	}
	if (job->mode == SIFT_VK_HASH)
	{
		n = (uint64_t) h[SIFT_VK_RES_NCOMP];
		if (h[SIFT_VK_RES_OVF] != 0 || n > p->ht_maxload + 1024)
			return SIFT_VK_OVERFLOW;
		res->rec_words = p->stride - 1;
		res->ngroups = (uint32_t) n;
		if (n && !read_out(rc, n * (p->stride - 1) * 8, rc->interrupt))
			return SIFT_VK_FAIL;
		res->acc = vk->hout.map;
		res->acc_words = (uint32_t) (n * (p->stride - 1));
		return SIFT_VK_OK;
	}
	n = (uint64_t) h[SIFT_VK_RES_NCAND];
	if (n > p->cand_cap)
		return SIFT_VK_OVERFLOW;
	if (n && !read_out(rc, n * 16, rc->interrupt))
		return SIFT_VK_FAIL;
	res->cand = vk->hout.map;
	res->ncand = (uint32_t) n;
	return SIFT_VK_OK;
}

/* the candidates c[0 .. n-1] cut to the k best, the k-th best last of them (the others in no order): the candidates left */
static uint32_t
keep_best(Cand *c, uint32_t n, uint32_t k, const CandOrder *o)
{
	if (n <= k)
		return n;
	if (k > 0)
		sift_nth(c, n, sizeof(Cand), k - 1, cand_cmp, (void *) o);
	return k;
}

/* the candidates of a pass that the next one keeps, in the buffer of the device, which the pass overwrites */
static Cand *
stash_cands(SiftVk *vk, uint32_t keep, const Cand *c, uint32_t n)
{
	if (vk->topn_cap < (uint64_t) keep + n)
	{
		void	   *b = realloc(vk->topn_buf, ((uint64_t) keep + n) * sizeof(Cand));

		if (!b)
			return NULL;
		vk->topn_buf = b;
		vk->topn_cap = (uint64_t) keep + n;
	}
	memcpy((Cand *) vk->topn_buf + keep, c, (size_t) n * sizeof(Cand));
	return vk->topn_buf;
}

/*
 * The top N in two passes that scan each granule once: the first granules give the k best candidates, whose k-th key bounds every row of the others that can be among the k best (the
 * device drops the rows of the second pass that are worse), and the k best of the candidates of both passes are the answer.
 */
static SiftVkStatus
run_topn(RunCtx *rc, SiftVkResult *res)
{
	SiftVk	   *vk = rc->vk;
	const SiftVkJob *job = rc->job;
	const CandOrder o = {job->topn_desc, job->topn_nulls};
	const uint32_t k = job->topn_k;
	uint32_t	sample = job->ngran < 8 ? job->ngran : 8;
	uint32_t	kept = 0,
				from = 0;
	SiftVkStatus st;
	Cand	   *c;

	if (sample < job->ngran)
	{
		from = sample;
		st = run_scan(rc, 0, sample);
		if (st == SIFT_VK_OK)
			st = finish_result(rc, res);
		if (st != SIFT_VK_OK)
			return st;
		kept = keep_best((Cand *) res->cand, res->ncand, k, &o);
		if (!(c = stash_cands(vk, 0, (const Cand *) res->cand, kept)))
			return SIFT_VK_FAIL;
		if (!o.nulls_first && k > 0 && kept == k && !cand_null(&c[k - 1]))
		{
			rc->plan.theta = c[k - 1].key;
			rc->plan.have_theta = true;
		}
	}
	st = run_scan(rc, from, job->ngran);
	if (st == SIFT_VK_OK)
		st = finish_result(rc, res);
	if (st == SIFT_VK_OK)
		st = publish_tab(rc);
	if (st != SIFT_VK_OK)
		return st;
	if (!(c = stash_cands(vk, kept, (const Cand *) res->cand, res->ncand)))
		return SIFT_VK_FAIL;
	res->ncand = keep_best(c, kept + res->ncand, k, &o);
	sift_nth_sort(c, res->ncand, sizeof(Cand), cand_cmp, (void *) &o);
	res->cand = (const int64_t *) c;
	return SIFT_VK_OK;
}

SiftVkStatus
sift_vk_run(SiftVk *vk, const SiftVkJob *job, void (*interrupt) (void), SiftVkResult *res)
{
	RunCtx	   *rc;
	SiftVkStatus st;
	double		t0 = now_ms();

	memset(res, 0, sizeof(*res));
	if (!vk || vk->lost || !vk->usable)
		return vk && vk->lost ? SIFT_VK_LOST : SIFT_VK_FAIL;
	if (vk->inflight && !settle_all(vk))
		return vk->lost ? SIFT_VK_LOST : SIFT_VK_TIMEOUT;
	rc = calloc(1, sizeof(*rc));
	if (!rc)
		return SIFT_VK_FAIL;
	rc->vk = vk;
	rc->job = job;
	rc->interrupt = interrupt;
	rc->res = res;
	if (!plan_job(vk, job, &rc->plan))
	{
		plan_free(&rc->plan);
		free(rc);
		return SIFT_VK_FAIL;
	}
	if (job->mode == SIFT_VK_TOPN)
		st = run_topn(rc, res);
	else
	{
		st = run_scan(rc, 0, job->ngran);
		if (st == SIFT_VK_OK)
			st = finish_result(rc, res);
		if (st == SIFT_VK_OK)
			st = publish_tab(rc);
	}
	if (st == SIFT_VK_OK && vk->test_lost)
	{
		vk->lost = true;
		snprintf(vk->err, sizeof(vk->err), "the device was lost (injected)");
		st = SIFT_VK_LOST;
	}
	if (st == SIFT_VK_OK && vk->test_timeout)
	{
		vk->usable = false;
		snprintf(vk->err, sizeof(vk->err), "a submission did not finish in time (injected)");
		st = SIFT_VK_TIMEOUT;
	}
	if (st != SIFT_VK_OK && st != SIFT_VK_PROG_ERROR)
	{
		res->acc = NULL;
		res->cand = NULL;
	}
	res->fast = rc->plan.fast;
	res->tab_hit = rc->plan.tab != NULL;
	plan_free(&rc->plan);
	res->times.compile_ms = rc->plan.compile_ms;
	res->times.wall_ms = now_ms() - t0;
	free(rc);
	return st;
}

#endif							/* SIFT_VULKAN */
