/*
 * agg/vk/job.c - the preparation of a batch: for the granules that fit, the decoded columns the cache holds are found and the chunks it
 * lacks are staged for upload (their cache regions made, a decode job written), and the control words the kernels read are laid out:
 * the job header, the granule table, the descriptors of the decoded columns, the program, its sets and the masks of the deleted rows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/vk/job.h"

#define TAB_GRAN_MAX	16384						/* granules of a batch whose descriptors come from a table */

#ifdef SIFT_VULKAN

static uint32_t
pow2_ge(uint64_t x)
{
	uint32_t	p = 4096;

	while ((uint64_t) p < x && p < ((uint32_t) 1 << 30))
		p <<= 1;
	return p;
}

static const int kernel_of[] = {PL_UNGROUPED, PL_DENSE, PL_HASH, PL_TOPN};
static const int fast_kernel_of[] = {PL_FAST_UNGROUPED, PL_FAST_DENSE, PL_FAST_HASH};

/* the fast kernel when the job has the shape for it (its pipeline is made or found), else the one that runs any program */
static bool
pick_kernel(SiftVk *vk, const SiftVkJob *job, Plan *p)
{
	double		t0;

	p->kern = vk->pipe[kernel_of[job->mode]];
	if (vk->no_fast || !fast_shape(job, &p->shape))
		return true;
	t0 = now_ms();
	p->kern = fast_pipeline(vk, fast_kernel_of[job->mode], &p->shape);
	p->compile_ms = now_ms() - t0;
	p->fast = p->kern != VK_NULL_HANDLE;
	return p->fast;
}

/* what a hash job that does not give every group needs of the result buffer: the counts of the dense groups of its keys but the last, or the candidates a host can read back at most (the staging area of the worker) */
static bool
plan_finish(const SiftVkJob *job, Plan *p)
{
	uint64_t	g = 1;

	if (job->finish == SIFT_VK_FIN_GROUPS)
		return true;
	p->out_bytes = 0;
	if (job->finish == SIFT_VK_FIN_TOPK)
	{
		p->cand_cap = (uint32_t) (SIFT_VK_SLAB_BYTES / ((p->stride - 1) * 8) < p->ht_maxload ? SIFT_VK_SLAB_BYTES / ((p->stride - 1) * 8) : p->ht_maxload);
		p->out_bytes = (uint64_t) p->cand_cap * (p->stride - 1) * 8;
		return job->topk.k > 0 && job->topk.ncomp >= 1 && job->topk.ncomp <= 2;
	}
	if (job->finish == SIFT_VK_FIN_DENSE_COUNT)
	{
		if (job->nkeys < 2 || job->nkeys > 3)
			return false;
		for (uint32_t j = 0; j + 1 < job->nkeys; j++)
			g *= job->dense_span[j];
		p->groups = (uint32_t) g;
		p->out_bytes = g * 8;
		return g > 0 && g <= ((uint64_t) 1 << 20);
	}
	return job->nkeys >= 1;
}

/* the table of groups and the result of a hash job, the slots as many as the groups it expects ask for but for the largest table that room bytes of memory allow */
static bool
size_hash(const SiftVkJob *job, uint64_t room, Plan *p)
{
	uint64_t	est = job->est_groups ? job->est_groups : 4096,
				slots;

	p->stride = job->nkeys + 3 + 3 * job->naggs;
	slots = pow2_ge(est * 2);
	while (slots > 4096 && (slots * p->stride * 8 > room || slots * p->stride * 8 > ((uint64_t) 3 << 30)))
		slots >>= 1;
	p->ht_mask = (uint32_t) slots - 1;
	p->ht_maxload = (uint32_t) (slots / 4 * 3);
	p->ht_bytes = slots * p->stride * 8;
	p->out_bytes = (uint64_t) p->ht_maxload * (p->stride - 1) * 8;
	return plan_finish(job, p);
}

uint64_t
sift_vk_job_tables(const SiftVkJob *job)
{
	Plan		p = {0};

	return job->mode == SIFT_VK_HASH && size_hash(job, UINT64_MAX, &p) ? p.ht_bytes + p.out_bytes : 0;
}

bool
plan_job(SiftVk *vk, const SiftVkJob *job, Plan *p)
{
	uint32_t	n = job->naggs;

	memset(p, 0, sizeof(*p));
	if (job->ncols > SIFT_VK_MAXCOLS || n > SIFT_VK_MAXAGGS || job->nkeys > SIFT_VK_MAXKEYS || job->nops > 64 || job->ngran == 0)
	{
		snprintf(vk->err, sizeof(vk->err), "the job is out of the limits of the kernels");
		return false;
	}
	if (job->mode == SIFT_VK_UNGROUPED)
		p->acc_words = 1 + 3 * n;
	else if (job->mode == SIFT_VK_DENSE)
	{
		uint64_t	g = 1;

		if (job->nkeys < 1 || job->nkeys > 2)
		{
			snprintf(vk->err, sizeof(vk->err), "a dense job has one or two keys");
			return false;
		}
		for (uint32_t j = 0; j < job->nkeys; j++)
			g *= job->dense_span[j];
		p->groups = (uint32_t) g;
		p->stride = 1 + 3 * n;
		if (g == 0 || g * p->stride > ((uint64_t) 1 << 20))
		{
			snprintf(vk->err, sizeof(vk->err), "the dense groups do not fit the result");
			return false;
		}
		p->acc_words = p->groups * p->stride;
	}
	else if (job->mode == SIFT_VK_HASH)
	{
		if (job->nkeys < 1)
		{
			snprintf(vk->err, sizeof(vk->err), "a hash job has a key");
			return false;
		}
		if (!size_hash(job, vk->caps.vram_budget_free / 4, p))
		{
			snprintf(vk->err, sizeof(vk->err), "the finish of a hash job does not fit the dense groups or the candidates");
			return false;
		}
	}
	else if (job->mode == SIFT_VK_TOPN)
	{
		p->cand_cap = job->max_cand ? job->max_cand : (1u << 20);
		p->out_bytes = (uint64_t) p->cand_cap * 16;
		p->flags = (job->sort_float ? SIFT_VK_FL_SORT_FLOAT : 0) | (job->topn_nulls ? SIFT_VK_FL_NULLS : 0);
	}
	if (!sift_vk_fit_cache(vk, job->tables ? job->tables : p->ht_bytes + p->out_bytes))
	{
		snprintf(vk->err, sizeof(vk->err), "no room for the cache the job leaves it");
		return false;
	}
	p->gen0 = vk->evict_gen;
	p->tab = tab_find(vk, job);
	if (job->tab.rel != 0 && !p->tab && (p->tab_host = calloc((size_t) job->ngran * job->ncols * 8, 4)) == NULL)
	{
		snprintf(vk->err, sizeof(vk->err), "no memory for the table of the job");
		return false;
	}
	return pick_kernel(vk, job, p);
}

void
plan_free(Plan *p)
{
	free(p->tab_host);
	p->tab_host = NULL;
}

static uint32_t
round2(uint32_t x)
{
	return (x + 1) & ~1u;
}

/* the words a decoded column takes: its values (whole 8 bytes) and its validity, a bit a row */
static uint32_t
dcol_words(const SiftVkChunk *c, uint32_t *vw)
{
	uint32_t	dw = c->w ? round2((c->nrows * c->w + 3) / 4) : 0;

	*vw = (c->flags & (SIFT_VK_CF_ALL_VALID | SIFT_VK_CF_ALL_NULL)) ? 0 : round2((c->nrows + 31) / 32);
	return dw + *vw;
}

static void
set_desc(uint32_t *d, const SiftVkChunk *c, uint32_t woff, uint32_t vw)
{
	uint32_t	dw = c->w ? round2((c->nrows * c->w + 3) / 4) : 0;

	d[0] = woff;
	d[1] = (c->flags & SIFT_VK_CF_ALL_NULL) ? SIFT_VK_DC_ALL_NULL : (vw == 0 ? SIFT_VK_DC_ALL_VALID : woff + dw);
	d[2] = (uint32_t) (uint64_t) c->base;
	d[3] = (uint32_t) ((uint64_t) c->base >> 32);
	d[4] = c->w;
	d[5] = c->nrows;
	d[6] = d[7] = 0;
}

static void
remember(Slot *s, const SiftVkKey *key)
{
	if (s->npend == s->cappend)
	{
		s->cappend = s->cappend ? s->cappend * 2 : 1024;
		s->pend = realloc(s->pend, s->cappend * sizeof(SiftVkKey));
	}
	s->pend[s->npend++] = *key;
}

/* the chunk staged as a decode job; false when the batch has no room for it (nothing is changed then) */
static bool
stage_chunk(SiftVk *vk, Slot *s, const SiftVkChunk *c, uint32_t *desc, Batch *b)
{
	uint32_t	vw,
				words = dcol_words(c, &vw),
				woff,
				*dj;
	uint64_t	len8 = ((uint64_t) c->len + 7) & ~7ull,
				raw8 = ((uint64_t) c->rawlen + 7) & ~7ull;
	bool		lz4 = (c->flags & SIFT_VK_CF_LZ4) != 0;
	Ent		   *e;

	if (b->njobs >= DJ_MAX || s->up + len8 > UP_CAP || (lz4 && s->unw + raw8 > UNW_CAP))
		return false;
	e = cache_insert(vk, &c->key, words ? words : 2, &woff);
	if (!e)
		return false;
	remember(s, &c->key);
	set_desc(e->desc, c, woff, vw);
	memcpy(desc, e->desc, 32);
	dj = (uint32_t *) s->dj.map + (size_t) b->njobs * SIFT_VK_DJ_WORDS;
	memset(dj, 0, SIFT_VK_DJ_WORDS * 4);
	memcpy((char *) s->stage.map + s->up, c->bytes, c->len);
	dj[0] = (uint32_t) s->up;
	dj[1] = c->len;
	dj[2] = lz4 ? (uint32_t) (UP_CAP + s->unw) : 0;
	dj[3] = c->rawlen;
	dj[4] = c->enc;
	dj[5] = c->flags;
	dj[6] = c->nrows;
	dj[7] = c->nnonnull;
	dj[8] = c->vlen;
	dj[9] = woff;
	dj[10] = e->desc[1] == SIFT_VK_DC_ALL_VALID || e->desc[1] == SIFT_VK_DC_ALL_NULL ? e->desc[1] : woff + (c->w ? round2((c->nrows * c->w + 3) / 4) : 0);
	dj[11] = c->w;
	dj[12] = (uint32_t) (uint64_t) c->base;
	dj[13] = (uint32_t) ((uint64_t) c->base >> 32);
	s->up += len8;
	b->bytes_uploaded += c->len;
	if (lz4)
	{
		s->unw += raw8;
		b->bytes_lz4 += c->len;
	}
	b->njobs++;
	b->chunks_uploaded++;
	return true;
}

static void
fill_header(const SiftVkJob *job, const Plan *p, uint32_t *h, uint32_t ng, uint32_t gt, uint32_t dt, uint32_t ops, uint32_t pool, uint32_t dead)
{
	memset(h, 0, SIFT_VK_J_HDR_WORDS * 4);
	h[SIFT_VK_J_NGRAN] = ng;
	h[SIFT_VK_J_NCOLS] = job->ncols;
	h[SIFT_VK_J_NOPS] = job->nops;
	h[SIFT_VK_J_NAGGS] = job->naggs;
	h[SIFT_VK_J_NKEYS] = job->nkeys;
	h[SIFT_VK_J_WHERE] = job->where_slot;
	h[SIFT_VK_J_FLAGS] = p->flags | (p->have_theta ? SIFT_VK_FL_THETA : 0);
	h[SIFT_VK_J_GT] = gt;
	h[SIFT_VK_J_DT] = dt;
	h[SIFT_VK_J_OPS] = ops;
	h[SIFT_VK_J_POOL] = pool;
	h[SIFT_VK_J_DEAD] = dead;
	for (uint32_t j = 0; j < SIFT_VK_MAXKEYS; j++)
		h[SIFT_VK_J_KEYREG + j] = j < job->nkeys ? job->key_reg[j] : SIFT_VK_NONE8;
	for (uint32_t a = 0; a < SIFT_VK_MAXAGGS; a++)
	{
		h[SIFT_VK_J_AGGKIND + a] = a < job->naggs ? job->agg[a].kind : 0;
		h[SIFT_VK_J_AGGVREG + a] = a < job->naggs ? job->agg[a].vreg : SIFT_VK_NONE8;
		h[SIFT_VK_J_AGGFREG + a] = a < job->naggs ? job->agg[a].freg : SIFT_VK_NONE8;
	}
	for (uint32_t j = 0; j < 2; j++)
	{
		h[SIFT_VK_J_DENSE_KMIN + 2 * j] = (uint32_t) (uint64_t) job->dense_kmin[j];
		h[SIFT_VK_J_DENSE_KMIN + 2 * j + 1] = (uint32_t) ((uint64_t) job->dense_kmin[j] >> 32);
		h[SIFT_VK_J_DENSE_SPAN + j] = job->dense_span[j];
	}
	h[SIFT_VK_J_DENSE_G] = p->groups;
	h[SIFT_VK_J_HT_MASK] = p->ht_mask;
	h[SIFT_VK_J_HT_STRIDE] = p->stride;
	h[SIFT_VK_J_HT_MAXLOAD] = p->ht_maxload;
	h[SIFT_VK_J_THETA] = (uint32_t) (uint64_t) p->theta;
	h[SIFT_VK_J_THETA + 1] = (uint32_t) ((uint64_t) p->theta >> 32);
	h[SIFT_VK_J_TOPN_DESC] = job->topn_desc;
	h[SIFT_VK_J_TOPN_NULLS] = job->topn_nulls;
	h[SIFT_VK_J_CAND_CAP] = p->cand_cap;
	h[SIFT_VK_J_SORTREG] = job->sort_reg;
	if (!p->fast)
		return;
	h[SIFT_VK_J_CNT_ROWS] = p->shape.cnt_rows;
	for (uint32_t i = 0; i < FAST_PRED_MAX; i++)
	{
		h[SIFT_VK_J_FAST_IMM + 2 * i] = (uint32_t) p->shape.imm[i];
		h[SIFT_VK_J_FAST_IMM + 2 * i + 1] = (uint32_t) (p->shape.imm[i] >> 32);
	}
}

typedef struct Mark
{
	uint64_t	up, unw, bytes_up, bytes_lz4;
	uint32_t	jobs, pend, resident, uploaded;
} Mark;

static Mark
mark_of(const Slot *s, const Batch *b)
{
	return (Mark) {s->up, s->unw, b->bytes_uploaded, b->bytes_lz4, b->njobs, s->npend, b->chunks_resident, b->chunks_uploaded};
}

/* the batch as it was at the mark: the chunks staged since are given up */
static void
mark_undo(SiftVk *vk, Slot *s, Batch *b, const Mark *m)
{
	for (uint32_t k = m->pend; k < s->npend; k++)
		cache_remove_key(vk, &s->pend[k]);
	s->npend = m->pend;
	s->up = m->up;
	s->unw = m->unw;
	b->njobs = m->jobs;
	b->chunks_resident = m->resident;
	b->chunks_uploaded = m->uploaded;
	b->bytes_uploaded = m->bytes_up;
	b->bytes_lz4 = m->bytes_lz4;
}

/* the descriptors of the columns of a granule, a row of ncols * 8 words: the chunks the cache holds, the others staged for upload; false when the batch has no room for one */
static bool
fill_row(SiftVk *vk, Slot *s, const SiftVkGranule *g, uint32_t nc, uint32_t *row, Batch *b)
{
	for (uint32_t c = 0; c < nc; c++)
	{
		const SiftVkChunk *ch = g->col[c];
		Ent		   *e = ch ? cache_find(vk, &ch->key) : NULL;

		memset(row + c * 8, 0, 32);
		row[c * 8 + 1] = SIFT_VK_DC_ALL_NULL;
		if (e)
		{
			memcpy(row + c * 8, e->desc, 32);
			cache_touch(vk, e);
			b->chunks_resident++;
		}
		else if (ch && !stage_chunk(vk, s, ch, row + c * 8, b))
			return false;
	}
	return true;
}

/* the granules a batch takes at most: what its control words hold (a table hit has no decode jobs to bound it) */
static uint32_t
batch_gmax(const SiftVkJob *job, const Plan *p, uint32_t n)
{
	uint64_t	words = CTL_CAP / 4,
				fixed = SIFT_VK_J_HDR_WORDS + (uint64_t) job->nops * SIFT_VK_OP_WORDS + job->npool + 256,
				per = 4 + (uint64_t) job->ncols * 8,
				lim = p->tab ? TAB_GRAN_MAX : BATCH_GRAN_MAX;

	if (fixed >= words)
		return 0;
	lim = (words - fixed) / per < lim ? (words - fixed) / per : lim;
	return (uint32_t) (lim < n ? lim : n);
}

BuildStatus
batch_build(SiftVk *vk, Slot *s, const SiftVkJob *job, const Plan *p, uint32_t g0, uint32_t g1, Batch *b)
{
	uint32_t   *ctl = s->ctl.map;
	uint32_t	nc = job->ncols,
				gmax = batch_gmax(job, p, g1 - g0),
				gt_w = SIFT_VK_J_HDR_WORDS,
				dt_w = gt_w + gmax * 4,
				op_w = dt_w + gmax * nc * 8,
				pool_w = op_w + job->nops * SIFT_VK_OP_WORDS,
				dead_w = pool_w + job->npool,
				cap = (uint32_t) (CTL_CAP / 4),
				ndead = 0,
				ng = 0;
	double		t0 = now_ms();

	memset(b, 0, sizeof(*b));
	b->s = s;
	b->g0 = g0;
	b->id = vk->batch;
	s->up = s->unw = 0;
	s->npend = 0;
	if (gmax == 0)
	{
		snprintf(vk->err, sizeof(vk->err), "the control words of the batch do not fit");
		return BUILD_FAIL;
	}
	memcpy(ctl + op_w, job->ops, (size_t) job->nops * SIFT_VK_OP_WORDS * 4);
	if (job->npool)
		memcpy(ctl + pool_w, job->pool, (size_t) job->npool * 4);
	if (p->tab)
		tab_touch(vk, p->tab);
	for (uint32_t i = 0; i < gmax; i++)
	{
		const SiftVkGranule *g = &job->gran[g0 + i];
		Mark		mark = mark_of(s, b);
		uint32_t	row[SIFT_VK_MAXCOLS * 8],
				   *gt = ctl + gt_w + i * 4;

		if (g->dead && dead_w + (ndead + 1) * 256 > cap)
			break;
		if (!p->tab)
		{
			if (!fill_row(vk, s, g, nc, row, b))
			{
				mark_undo(vk, s, b, &mark);
				break;
			}
			memcpy(ctl + dt_w + (size_t) i * nc * 8, row, (size_t) nc * 32);
			if (p->tab_host)
				memcpy(p->tab_host + (size_t) (g0 + i) * nc * 8, row, (size_t) nc * 32);
		}
		gt[0] = g->nrows;
		gt[1] = SIFT_VK_DC_ALL_VALID;
		gt[2] = g->id;
		gt[3] = 0;
		if (g->dead)
		{
			memcpy(ctl + dead_w + ndead * 256, g->dead, 1024);
			gt[1] = ndead++ * 256;
		}
		ng++;
	}
	if (ng == 0)
	{
		snprintf(vk->err, sizeof(vk->err), "a granule of the job does not fit the cache or the staging area of the device");
		return BUILD_NO_ROOM;
	}
	b->ng = ng;
	if (p->tab)
	{
		b->tab_buf = p->tab->buf.b;
		b->tab_src = (uint64_t) g0 * nc * 32;
		b->tab_dst = (uint64_t) dt_w * 4;
		b->tab_bytes = (uint64_t) ng * nc * 32;
		b->chunks_resident = ng * nc;
	}
	fill_header(job, p, ctl, ng, gt_w, dt_w, op_w, pool_w, dead_w);
	b->stage_ms = now_ms() - t0;
	return BUILD_OK;
}

void
batch_abandon(SiftVk *vk, Batch *b)
{
	Slot	   *s = b->s;

	for (uint32_t k = 0; k < s->npend; k++)
		cache_remove_key(vk, &s->pend[k]);
	s->npend = 0;
	vk->evict_gen++;
}

/* the statuses of the decode jobs of a finished batch; false when one failed (its cache entries are dropped) */
bool
batch_check(SiftVk *vk, Batch *b)
{
	const int64_t *st = b->s->rstat.map;
	bool		ok = true;

	for (uint32_t j = 0; j < b->njobs; j++)
		if (st[j] != 0)
		{
			ok = false;
			snprintf(vk->err, sizeof(vk->err), "a chunk could not be decoded on the device (code %d)", (int) st[j]);
			break;
		}
	if (!ok)
		batch_abandon(vk, b);
	b->s->npend = 0;
	return ok;
}

#else							/* SIFT_VULKAN */

uint64_t
sift_vk_job_tables(const SiftVkJob *job)
{
	return 0;
}

#endif							/* SIFT_VULKAN */
