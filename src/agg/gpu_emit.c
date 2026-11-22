/*
 * agg/gpu_emit.c - the programs of a query as the device runs them (agg/vk/layout.h): the SiftVexprProg of its WHERE, its keys, the arguments and filters of its aggregates and its sort
 * expression, renamed onto one register file and one set of mask slots and appended to one list of operations, the IN lists as sets, and what the job then names (the mask of the WHERE,
 * the register of every key and argument, the kind of every aggregate). Nothing here touches the device or the relation: a program the device cannot run is a reason, never an error.
 */
#include "postgres.h"

#include "agg/gpu_emit.h"
#include "agg/vk/layout.h"

extern PGDLLIMPORT bool sift_agg_exact_float;

#define MAX_OPS						64
#define MAX_REGS					16
#define MAX_SLOTS					32
#define POOL_WORDS					16384

static void
fail(SiftGpuEmit *b, const char *why)
{
	if (b->why == NULL)
		b->why = why;
}

static uint32
reg_new(SiftGpuEmit *b)
{
	if (b->nregs >= MAX_REGS)
		fail(b, "the program needs more registers than the device has");
	return Min(b->nregs++, MAX_REGS - 1);
}

static uint32
slot_new(SiftGpuEmit *b)
{
	if (b->nmasks >= MAX_SLOTS)
		fail(b, "the program needs more masks than the device has");
	return Min(b->nmasks++, MAX_SLOTS - 1);
}

static uint32 *
raw_op(SiftGpuEmit *b, uint32 op, uint32 ty, uint32 tt, uint32 fl, uint32 dst, uint32 a, uint32 bb, uint32 c, uint32 act, int scale, int32 aux, int64 imm)
{
	static uint32 sink[SIFT_VK_OP_WORDS];
	uint32	   *o;

	if (b->nops >= MAX_OPS)
	{
		fail(b, "the program has more operations than the device runs");
		return sink;
	}
	o = &b->ops[b->nops++ * SIFT_VK_OP_WORDS];
	o[0] = op | ty << 8 | tt << 16 | fl << 24;
	o[1] = dst | a << 8 | bb << 16 | c << 24;
	o[2] = act | ((uint32) scale & 0xFFFF) << 8;
	o[3] = (uint32) aux;
	o[4] = (uint32) (uint64) imm;
	o[5] = (uint32) ((uint64) imm >> 32);
	o[6] = o[7] = 0;
	return o;
}

static bool
col_is_float(const SiftGpuQuery *q, int slot)
{
	return q->store->kinds[q->leaf[slot]] == SIFT_CELL_F8;
}

static uint32
col_reg(SiftGpuEmit *b, const SiftGpuQuery *q, int slot)
{
	if (slot < 0 || slot >= q->ncols)
	{
		fail(b, "a column of the query is not among those the device reads");
		return 0;
	}
	if (b->col_reg[slot] < 0)
	{
		b->col_reg[slot] = (int) reg_new(b);
		raw_op(b, SIFT_VK_OP_LOAD_COL, col_is_float(q, slot) ? SIFT_VK_T_F64 : SIFT_VK_T_I64, 0, 0, b->col_reg[slot], slot, 0, 0, 0, 0, 0, 0);
	}
	return b->col_reg[slot];
}

static uint32
type_of(SiftGpuEmit *b, SiftVecType t)
{
	if (t == SVEC_BOOL || t == SVEC_I32)
		return SIFT_VK_T_I32;
	if (t == SVEC_I64 || t == SVEC_NUM64 || t == SVEC_F64)
		return t == SVEC_I64 ? SIFT_VK_T_I64 : t == SVEC_NUM64 ? SIFT_VK_T_NUM : SIFT_VK_T_F64;
	fail(b, "an expression of a type the device does not compute (a numeric of more than 18 digits, a string)");
	return SIFT_VK_T_I64;
}

/* What each of the four operand fields of an operation of SiftVecOpCode is: R register, S mask slot, A mask slot of the live rows (0: those of the scan), C column, - none */
static const char *const OPERANDS[] = {
	"RC--", "R---", "R---", "RR--", "RRR-", "RRR-", "RRR-", "RRR-", "RR--", "RR--", "SRR-", "SR--", "SSS-", "SSS-", "SS--", "SC--", "S---",
	"R---", "RR--", "RR-S", "SA--", "SAS-", "SAS-", "SAS-", "SAR-", "A---", "RR--", "RR--", "RR--", "SS-S", "S---", "SS--", "SAS-"
};

typedef struct Rename
{
	uint32		roff;
	uint32		soff;
	uint32		act;
} Rename;

static uint32
rename_field(char kind, uint32 v, const Rename *m)
{
	switch (kind)
	{
		case 'R':
			return m->roff + v;
		case 'S':
			return v == 0 ? 0 : m->soff + v;
		case 'A':
			return v == 0 ? m->act : m->soff + v;
		default:
			return v;
	}
}

static void
emit_op(SiftGpuEmit *b, const SiftGpuQuery *q, const SiftVecOp *o, const Rename *m)
{
	const char *kinds = OPERANDS[o->op];
	uint32		f[4] = {o->dst, o->a, o->b, o->c};
	uint32		fl = o->flags & VOP_FLAG_CONST_B,
				ty = type_of(b, o->type),
				tt = type_of(b, o->type_to);
	bool		real = o->type == SVEC_F64 && (o->op == VOP_LOAD_CONST || fl);
	int64		imm = o->imm_i64;

	for (int i = 0; i < 4; i++)
		f[i] = rename_field(kinds[i], f[i], m);
	if (o->op == VOP_LOAD_COL || o->op == VOP_MASK_IN)
	{
		if (o->aux < 0 || o->aux >= (o->op == VOP_LOAD_COL ? q->ncols : SIFT_VEXPR_MAX_MASKS))
			fail(b, "an input of the program is not a column of the plan");
		f[1] = o->op == VOP_LOAD_COL ? (uint32) o->aux : (uint32) q->mask_col[Min(o->aux, SIFT_VEXPR_MAX_MASKS - 1)];
	}
	if (o->op == VOP_MASK_CONST)
		fl = (o->aux ? 1 : 0) | 2;
	if (real)
		memcpy(&imm, &o->imm_f64, sizeof(imm));
	raw_op(b, (uint32) o->op, ty, tt, fl, f[0], f[1], f[2], f[3], rename_field('A', o->act, m), o->scale, o->aux, imm);
}

/* The program appended to the device's, its registers and masks renamed past those in use and the rows its operations are live for those of act; the register or mask slot of its result */
static uint32
emit_prog(SiftGpuEmit *b, const SiftGpuQuery *q, const SiftVexprProg *p, uint32 act)
{
	Rename		m = {b->nregs, b->nmasks - 1, act};

	if (p->wide || b->nregs + p->n_regs > MAX_REGS || b->nmasks + p->n_masks - 1 > MAX_SLOTS)
	{
		fail(b, "the expressions need more registers or masks than the device has");
		return 0;
	}
	b->nregs += p->n_regs;
	b->nmasks += p->n_masks - 1;
	for (int i = 0; i < p->n_ops; i++)
	{
		const SiftVecOp *o = &p->ops[i];

		if (o->op == VOP_SKIP_IF_EMPTY)
			continue;
		if (o->zone != NULL)
			fail(b, "an expression reads a timestamptz in the zone of the session");
		emit_op(b, q, o, &m);
	}
	return rename_field(p->out_is_bool ? 'S' : 'R', p->out, &m);
}

static int
int64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* column [NOT] IN (set): the device searches the sorted values */
static uint32
emit_in(SiftGpuEmit *b, const SiftGpuQuery *q, const SiftGpuIn *in)
{
	uint32		r = col_reg(b, q, in->col),
				slot = slot_new(b),
				m = 0;
	int64	   *s = palloc(Max(in->n, 1) * sizeof(int64));
	uint32	   *o = raw_op(b, SIFT_VK_OP_IN_SET, 0, 0, 0, slot, r, 0, 0, 0, 0, 0, 0);

	if (in->n == 0 || b->npool + 2 * in->n > POOL_WORDS)
		fail(b, "an empty IN list, or one that does not fit the program");
	else
	{
		memcpy(s, in->vals, in->n * sizeof(int64));
		qsort(s, in->n, sizeof(int64), int64_cmp);
		for (uint32 i = 0; i < in->n; i++)
			if (i == 0 || s[i] != s[i - 1])
			{
				b->pool[b->npool + 2 * m] = (uint32) (uint64) s[i];
				b->pool[b->npool + 2 * m + 1] = (uint32) ((uint64) s[i] >> 32);
				m++;
			}
		o[6] = b->npool;
		o[7] = m;
		b->npool += 2 * m;
	}
	pfree(s);
	if (!in->negate)
		return slot;
	r = slot_new(b);
	raw_op(b, SIFT_VK_OP_MASK_NOT, 0, 0, 0, r, slot, 0, 0, 0, 0, 0, 0);
	return r;
}

/* The mask slot of the rows that pass the WHERE and the IN lists; none when every row does */
static uint32
emit_where(SiftGpuEmit *b, const SiftGpuQuery *q)
{
	uint32		w = 0;

	if (q->where != NULL)
		w = emit_prog(b, q, q->where, 0);
	for (int i = 0; i < q->nin; i++)
	{
		uint32		s = emit_in(b, q, &q->in[i]),
					both = w == 0 ? s : slot_new(b);

		if (w != 0)
			raw_op(b, SIFT_VK_OP_MASK_AND, 0, 0, 0, both, w, s, 0, 0, 0, 0, 0);
		w = both;
	}
	return w == 0 ? SIFT_VK_NONE8 : w;
}

static int
slot_of_attno(const SiftGpuQuery *q, int attno)
{
	int			leaf = q->store->att_first[attno];

	for (int c = 0; c < q->ncols; c++)
		if (q->leaf[c] == leaf)
			return c;
	return -1;
}

/* The register of an expression of the query, or of its plain column; *is_float says what it holds */
static uint32
emit_value(SiftGpuEmit *b, const SiftGpuQuery *q, const SiftVexprProg *p, int attno, uint32 act, bool *is_float)
{
	if (p == NULL)
	{
		int			slot = slot_of_attno(q, attno);

		*is_float = slot >= 0 && col_is_float(q, slot);
		return col_reg(b, q, slot);
	}
	if (p->out_is_bool || p->out_type == SVEC_NUM128)
		fail(b, "an expression is a boolean or a numeric of more than 18 digits");
	*is_float = p->out_type == SVEC_F64;
	return emit_prog(b, q, p, act);
}

static uint32
agg_kind_of(SiftGpuEmit *b, SiftV6Kind kind, bool is_float)
{
	switch (kind)
	{
		case SV6_COUNT_STAR:
			return SIFT_VK_AK_COUNT_STAR;
		case SV6_COUNT:
			return SIFT_VK_AK_COUNT;
		case SV6_SUM:
		case SV6_AVG:
			if (is_float && sift_agg_exact_float)
				fail(b, "a float sum with sift.agg_exact_float adds in the order of the CPU");
			return is_float ? SIFT_VK_AK_SUM_F : SIFT_VK_AK_SUM_I;
		case SV6_MIN:
			return is_float ? SIFT_VK_AK_MIN_F : SIFT_VK_AK_MIN;
		case SV6_MAX:
			return is_float ? SIFT_VK_AK_MAX_F : SIFT_VK_AK_MAX;
		default:
			fail(b, "an aggregate the device does not compute (count distinct)");
			return 0;
	}
}

static void
emit_aggs(SiftGpuEmit *b, const SiftGpuQuery *q, SiftVkJob *job, uint32 where)
{
	for (int a = 0; a < q->spec->naggs; a++)
	{
		const SiftV6Agg *ag = &q->spec->agg[a];
		SiftVkAgg  *va = &job->agg[a];
		uint32		act = where;
		bool		is_float = false;

		va->freg = SIFT_VK_NONE8;
		va->vreg = SIFT_VK_NONE8;
		if (q->filter[a] != NULL)
		{
			uint32		fs = emit_prog(b, q, q->filter[a], where);

			act = slot_new(b);
			raw_op(b, SIFT_VK_OP_ACT_AND, 0, 0, 0, act, where, fs, 0, 0, 0, 0, 0);
			va->freg = (uint8) fs;
		}
		if (sift_v6_kind_reads_column(ag->kind))
			va->vreg = (uint8) emit_value(b, q, q->arg[a], ag->arg.attno, act, &is_float);
		va->kind = (uint8) agg_kind_of(b, ag->kind, is_float);
	}
}

static void
emit_keys(SiftGpuEmit *b, const SiftGpuQuery *q, SiftVkJob *job, uint32 where)
{
	for (int i = 0; i < q->spec->nkeys; i++)
	{
		const SiftV6Col *k = &q->spec->key[i];
		SiftV6Class cls = sift_v6_class(k->typid);
		bool		is_float;

		b->key_slot[i] = q->key[i] == NULL ? slot_of_attno(q, k->attno) : -1;
		if (q->key[i] == NULL && cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_BOOL)
			fail(b, "a group key that is a string, a float or a numeric");
		job->key_reg[i] = (uint8) emit_value(b, q, q->key[i], k->attno, where, &is_float);
		if (is_float)
			fail(b, "a group key that is a float");
	}
}

void
sift_gpu_emit_init(SiftGpuEmit *b)
{
	memset(b, 0, sizeof(*b));
	b->nmasks = 1;
	memset(b->col_reg, -1, sizeof(b->col_reg));
}

const char *
sift_gpu_emit_agg(SiftGpuEmit *b, const SiftGpuQuery *q, SiftVkJob *job)
{
	const SiftV6Spec *s = q->spec;
	uint32		act;

	if (s->nsets > 0 || s->nkeys > SIFT_VK_MAXKEYS || s->naggs > SIFT_VK_MAXAGGS || s->naggs == 0)
		return "grouping sets, or more keys or aggregates than the device holds";
	job->where_slot = emit_where(b, q);
	act = job->where_slot == SIFT_VK_NONE8 ? 0 : job->where_slot;
	emit_aggs(b, q, job, act);
	emit_keys(b, q, job, act);
	job->nkeys = s->nkeys;
	job->naggs = s->naggs;
	job->ncols = q->ncols;
	job->nops = b->nops;
	job->ops = b->ops;
	job->npool = b->npool;
	job->pool = b->pool;
	return b->why;
}

const char *
sift_gpu_emit_topn(SiftGpuEmit *b, const SiftGpuQuery *q, SiftVkJob *job, uint32 k, bool desc, bool nulls_first)
{
	bool		is_float;

	if (q->sort == NULL || k == 0 || k > SIFT_VK_GRANULE_ROWS * 8)
		return "no sort expression, or a LIMIT the device does not keep";
	job->where_slot = emit_where(b, q);
	job->sort_reg = emit_value(b, q, q->sort, 0, job->where_slot == SIFT_VK_NONE8 ? 0 : job->where_slot, &is_float);
	job->sort_float = is_float;
	job->topn_k = k;
	job->topn_desc = desc;
	job->topn_nulls = nulls_first;
	job->ncols = q->ncols;
	job->nops = b->nops;
	job->ops = b->ops;
	job->npool = b->npool;
	job->pool = b->pool;
	return b->why;
}
