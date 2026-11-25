/*
 * agg/vk/fast.c - the fast kernels (agg/vk/shaders/common_fast.glsl): which jobs they take, and their pipelines. A job is taken when its
 * program only loads columns and compares them with constants (a conjunction is the WHERE), no aggregate has a FILTER, and its aggregates,
 * keys and columns fit the packed shape of the kernels. The shape (the columns, the comparisons, the kind of every aggregate) is fixed in
 * the pipeline with specialization constants so that the kernels unroll over it; the pipelines are kept for the next job of the same shape.
 */
#include <string.h>

#include "agg/vk/int.h"

#define NU_MAX		15
#define FAGG_MAX	8
#define NONE_IDX	15

enum { SC_NU, SC_UC0, SC_UC1, SC_UF, SC_NPRED, SC_PCOL, SC_POP, SC_NAGG, SC_AKIND, SC_ACOL, SC_NKEYS, SC_KCOL, SC_WORDS };

typedef struct Analysis
{
	int			reg_col[16];	/* the column slot a register loads, -1 none */
	bool		reg_f64[16];
	bool		reg_const[16];	/* the register holds a constant (LOAD_CONST) */
	uint64_t	reg_imm[16];
	bool		slot_set[16];
	bool		act_only[16];	/* the slot holds the rows an operation is live for (ACT_*), not a result */
	uint32_t	leaves[16];		/* the comparisons a mask slot is the conjunction of: a bit of the comparison */
	uint32_t	npred;
	int			pred_col[FAST_PRED_MAX];	/* the column slot and the type a comparison reads (its register may be loaded again afterwards) */
	bool		pred_f64[FAST_PRED_MAX];
	uint8_t		pred_op[FAST_PRED_MAX];
	uint64_t	pred_imm[FAST_PRED_MAX];
	int			ucol[NU_MAX];	/* the distinct (column slot, double) pairs */
	bool		uf64[NU_MAX];
	uint32_t	nu;
} Analysis;

/* the index of the column (as a double or not) in the list of the columns the kernel loads; -1 when the list is full */
static int
use_col(Analysis *an, int col, bool f64)
{
	for (uint32_t u = 0; u < an->nu; u++)
		if (an->ucol[u] == col && an->uf64[u] == f64)
			return (int) u;
	if (an->nu >= NU_MAX)
		return -1;
	an->ucol[an->nu] = col;
	an->uf64[an->nu] = f64;
	return (int) an->nu++;
}

static int
use_reg(Analysis *an, uint32_t r)
{
	return use_col(an, an->reg_col[r], an->reg_f64[r]);
}

static bool
reg_loaded(const Analysis *an, uint32_t r)
{
	return r < 16 && an->reg_col[r] >= 0;
}

/* operator of `b op a` for `a op b` */
static uint8_t
flipped(uint32_t cmp)
{
	return (uint8_t) (cmp == SIFT_VK_CMP_LT ? SIFT_VK_CMP_GT : cmp == SIFT_VK_CMP_GT ? SIFT_VK_CMP_LT : cmp == SIFT_VK_CMP_LE ? SIFT_VK_CMP_GE :
					  cmp == SIFT_VK_CMP_GE ? SIFT_VK_CMP_LE : cmp);
}

/* a comparison of a loaded column with a constant, the constant an immediate of the operation or a register of LOAD_CONST, on either side:
 * kept as column, operator, constant; false for any other comparison */
static bool
cmp_column_const(Analysis *an, const uint32_t *op, uint32_t ty, uint32_t fl, uint32_t a, uint32_t b)
{
	uint32_t	col;
	bool		swap = false;
	uint64_t	imm;

	if (a >= 16 || b >= 16)
		return false;
	if (fl & 1)
	{
		col = a;
		imm = (uint64_t) op[4] | ((uint64_t) op[5] << 32);
	}
	else if (reg_loaded(an, a) && an->reg_const[b])
	{
		col = a;
		imm = an->reg_imm[b];
	}
	else if (an->reg_const[a] && reg_loaded(an, b))
	{
		col = b;
		imm = an->reg_imm[a];
		swap = true;
	}
	else
		return false;
	if (!reg_loaded(an, col) || (ty == SIFT_VK_T_F64) != an->reg_f64[col])
		return false;
	an->pred_col[an->npred] = an->reg_col[col];
	an->pred_f64[an->npred] = an->reg_f64[col];
	an->pred_op[an->npred] = swap ? flipped(op[3]) : (uint8_t) op[3];
	an->pred_imm[an->npred] = imm;
	return true;
}

/* a CAST of a register that changes nothing the kernels read: an int4 to an int8 (the columns are held as 64 bit values), and a constant to a double */
static bool
cast_reg(Analysis *an, uint32_t reg, uint32_t from, uint32_t to)
{
	if (reg >= 16 || (!reg_loaded(an, reg) && !an->reg_const[reg]))
		return false;
	if (from == SIFT_VK_T_I32 && to == SIFT_VK_T_I64)
		return true;
	if ((from == SIFT_VK_T_I32 || from == SIFT_VK_T_I64) && to == SIFT_VK_T_F64 && an->reg_const[reg])
	{
		double		d = (double) (int64_t) an->reg_imm[reg];

		memcpy(&an->reg_imm[reg], &d, 8);
		return true;
	}
	return false;
}

/* the program as loads of columns and constants, comparisons of a column with a constant and their conjunctions; false for anything else. The program is what agg/vexpr/compile.c makes (and
 * agg/gpu/emit.c renames): registers and mask slots are reused, a conjunction is made in place, and the rows an operation is live for (ACT_NOTFALSE) are no concern of a kernel that
 * evaluates a comparison, which cannot fail, on every row */
static bool
analyze_program(const SiftVkJob *job, Analysis *an)
{
	memset(an, 0, sizeof(*an));
	for (int r = 0; r < 16; r++)
		an->reg_col[r] = -1;
	an->slot_set[0] = true;
	for (uint32_t i = 0; i < job->nops; i++)
	{
		const uint32_t *op = job->ops + (size_t) i * SIFT_VK_OP_WORDS;
		uint32_t	code = op[0] & 255,
					ty = (op[0] >> 8) & 255,
					tt = (op[0] >> 16) & 255,
					fl = op[0] >> 24,
					dst = op[1] & 255,
					a = (op[1] >> 8) & 255,
					b = (op[1] >> 16) & 255;

		if (dst >= 16)
			return false;
		if (code == SIFT_VK_OP_LOAD_COL)
		{
			if (a >= job->ncols)
				return false;
			an->reg_col[dst] = (int) a;
			an->reg_f64[dst] = ty == SIFT_VK_T_F64;
			an->reg_const[dst] = false;
		}
		else if (code == SIFT_VK_OP_LOAD_CONST)
		{
			an->reg_col[dst] = -1;
			an->reg_const[dst] = true;
			an->reg_imm[dst] = (uint64_t) op[4] | ((uint64_t) op[5] << 32);
		}
		else if (code == SIFT_VK_OP_CAST)
		{
			if (dst != a || !cast_reg(an, dst, ty, tt))
				return false;
		}
		else if (code == SIFT_VK_OP_CMP)
		{
			if (op[3] > SIFT_VK_CMP_GE || an->npred >= FAST_PRED_MAX || !cmp_column_const(an, op, ty, fl, a, b))
				return false;
			an->slot_set[dst] = true;
			an->act_only[dst] = false;
			an->leaves[dst] = 1u << an->npred++;
		}
		else if (code == SIFT_VK_OP_MASK_AND)
		{
			if (a >= 16 || b >= 16 || !an->slot_set[a] || !an->slot_set[b] || an->act_only[a] || an->act_only[b])
				return false;
			an->leaves[dst] = an->leaves[a] | an->leaves[b];
			an->slot_set[dst] = true;
			an->act_only[dst] = false;
		}
		else if (code == SIFT_VK_OP_ACT_NOTFALSE)
		{
			if (a >= 16 || b >= 16 || !an->slot_set[a] || !an->slot_set[b] || an->act_only[b])
				return false;
			an->slot_set[dst] = true;
			an->act_only[dst] = true;
			an->leaves[dst] = 0;
		}
		else
			return false;
	}
	return true;
}

/* every chunk of the column slot is stored without nulls: a count of its values is a count of rows */
static bool
col_has_no_nulls(const SiftVkJob *job, uint32_t slot)
{
	if (!job->gran)
		return false;
	for (uint32_t g = 0; g < job->ngran; g++)
	{
		const SiftVkChunk *c = job->gran[g].col[slot];

		if (!c || !(c->flags & SIFT_VK_CF_ALL_VALID))
			return false;
	}
	return true;
}

static void
pack(uint32_t *w, uint32_t i, uint32_t v)
{
	*w |= (v & 15u) << (4 * i);
}

/* the words of the accumulators of the groups of a dense job, rounded up to 1024, 2048 or 4096 (the shared memory of a workgroup in vk_agg_fast_dense.comp); 0 when they do not fit */
static uint32_t
dense_words(const SiftVkJob *job)
{
	uint64_t	g = 1,
				words;

	if (job->nkeys < 1 || job->nkeys > 2)
		return 0;
	for (uint32_t j = 0; j < job->nkeys; j++)
		g *= job->dense_span[j];
	words = g * (1 + 3 * job->naggs);
	return g == 0 || words > 4096 ? 0 : words <= 1024 ? 1024 : words <= 2048 ? 2048 : 4096;
}

bool
fast_shape(const SiftVkJob *job, FastShape *fs)
{
	Analysis	an;
	uint32_t	where_leaves = 0;

	memset(fs, 0, sizeof(*fs));
	if ((job->mode != SIFT_VK_UNGROUPED && job->mode != SIFT_VK_DENSE && job->mode != SIFT_VK_HASH) || job->naggs > FAGG_MAX ||
		job->nkeys > SIFT_VK_MAXKEYS || !analyze_program(job, &an))
		return false;
	if (job->where_slot != SIFT_VK_NONE8)
	{
		if (job->where_slot >= 16 || !an.slot_set[job->where_slot] || an.act_only[job->where_slot])
			return false;
		where_leaves = an.leaves[job->where_slot];
	}
	for (uint32_t p = 0; p < an.npred; p++)
		if ((where_leaves >> p) & 1u)
		{
			int			u = use_col(&an, an.pred_col[p], an.pred_f64[p]);

			if (u < 0)
				return false;
			pack(&fs->sc[SC_PCOL], fs->sc[SC_NPRED], (uint32_t) u);
			pack(&fs->sc[SC_POP], fs->sc[SC_NPRED], an.pred_op[p]);
			fs->imm[fs->sc[SC_NPRED]++] = an.pred_imm[p];
		}
	for (uint32_t a = 0; a < job->naggs; a++)
	{
		const SiftVkAgg *ag = &job->agg[a];
		int			u = NONE_IDX;

		if (ag->freg != SIFT_VK_NONE8)
			return false;
		if (ag->kind != SIFT_VK_AK_COUNT_STAR)
		{
			if (!reg_loaded(&an, ag->vreg) || (u = use_reg(&an, ag->vreg)) < 0)
				return false;
		}
		pack(&fs->sc[SC_AKIND], a, ag->kind);
		pack(&fs->sc[SC_ACOL], a, (uint32_t) u);
		if (ag->kind == SIFT_VK_AK_COUNT_STAR || col_has_no_nulls(job, (uint32_t) an.reg_col[ag->vreg]))
			fs->cnt_rows |= 1u << a;
	}
	for (uint32_t k = 0; k < job->nkeys; k++)
	{
		int			u = reg_loaded(&an, job->key_reg[k]) ? use_reg(&an, job->key_reg[k]) : -1;

		if (u < 0)
			return false;
		pack(&fs->sc[SC_KCOL], k, (uint32_t) u);
	}
	for (uint32_t u = 0; u < an.nu; u++)
	{
		pack(&fs->sc[u < 8 ? SC_UC0 : SC_UC1], u % 8, (uint32_t) an.ucol[u]);
		fs->sc[SC_UF] |= an.uf64[u] ? 1u << u : 0;
	}
	fs->sc[SC_NU] = an.nu;
	fs->sc[SC_NAGG] = job->naggs;
	fs->sc[SC_NKEYS] = job->nkeys;
	fs->sc[SC_WORDS] = job->mode == SIFT_VK_DENSE ? dense_words(job) : 0;
	return job->mode != SIFT_VK_DENSE || fs->sc[SC_WORDS] != 0;
}

bool
sift_vk_job_is_fast(const SiftVkJob *job)
{
	FastShape	fs;

	return fast_shape(job, &fs);
}

#ifdef SIFT_VULKAN

static VkPipeline
make_fast_pipeline(SiftVk *vk, int pl, const uint32_t *sc)
{
	VkSpecializationMapEntry map[FAST_NSC];
	VkSpecializationInfo si = {.mapEntryCount = FAST_NSC, .pMapEntries = map, .dataSize = FAST_NSC * 4, .pData = sc};
	VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = vk->pl};
	VkPipeline	p = VK_NULL_HANDLE;
	VkResult	r;

	for (uint32_t i = 0; i < FAST_NSC; i++)
		map[i] = (VkSpecializationMapEntry) {i, i * 4, 4};
	cpi.stage = (VkPipelineShaderStageCreateInfo) {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = vk->mod[pl], .pName = "main", .pSpecializationInfo = &si};
	r = vk->vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p);
	if (r != VK_SUCCESS)
	{
		fail(vk, "vkCreateComputePipelines", r);
		return VK_NULL_HANDLE;
	}
	return p;
}

/* the pipeline of the kernel pl for the shape; no job is in flight (sift_vk_run settles the device first) */
VkPipeline
fast_pipeline(SiftVk *vk, int pl, const FastShape *fs)
{
	FastPipe   *victim = NULL;

	for (int i = 0; i < FAST_CACHE; i++)
	{
		FastPipe   *e = &vk->fast[i];

		if (e->pipe && e->pl == pl && !memcmp(e->sc, fs->sc, sizeof(e->sc)))
		{
			e->used = ++vk->clock;
			return e->pipe;
		}
		if (!victim || !e->pipe || (victim->pipe && e->used < victim->used))
			victim = e;
	}
	if (victim->pipe)
		vk->vkDestroyPipeline(vk->dev, victim->pipe, NULL);
	victim->pipe = make_fast_pipeline(vk, pl, fs->sc);
	victim->pl = pl;
	memcpy(victim->sc, fs->sc, sizeof(victim->sc));
	victim->used = ++vk->clock;
	return victim->pipe;
}

void
free_fast(SiftVk *vk)
{
	for (int i = 0; i < FAST_CACHE; i++)
		if (vk->fast[i].pipe)
			vk->vkDestroyPipeline(vk->dev, vk->fast[i].pipe, NULL);
	memset(vk->fast, 0, sizeof(vk->fast));
}

#endif							/* SIFT_VULKAN */
