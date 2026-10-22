/*
 * agg/two.c - two level aggregation: arms and cells
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* the cells of every arm to newcap groups, renumbered through remap (NULL: as they are); the touched list follows */
void
arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	SiftAggSpec *s = ex->spec;

	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		SiftAggArm *sa = &s->arm[k];
		uint64	   *nord;

		if (a->local)
			continue;
		nord = MemoryContextAlloc(ex->tcxt, newcap * sizeof(uint64));
		memset(nord, 0xff, newcap * sizeof(uint64));
		for (int ai = 0; ai < sa->ninner; ai++)
		{
			int64	   *nn = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64)),
					   *ni = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));
			double	   *nd = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
			uint8	   *nnf = MemoryContextAllocZero(ex->tcxt, newcap);

			for (uint32 g = 0; g < oldcap && a->ord; g++)
			{
				uint32		t = remap ? remap[g] : g;

				if (t == UINT32_MAX || a->ord[g] == UINT64_MAX)
					continue;
				nn[t] = a->n[ai][g];
				ni[t] = a->i[ai][g];
				nd[t] = a->d[ai][g];
				nnf[t] = a->nf[ai][g];
			}
			a->n[ai] = nn;
			a->i[ai] = ni;
			a->d[ai] = nd;
			a->nf[ai] = nnf;
		}
		for (uint32 g = 0; g < oldcap && a->ord; g++)
		{
			uint32		t = remap ? remap[g] : g;

			if (t != UINT32_MAX && a->ord[g] != UINT64_MAX)
				nord[t] = a->ord[g];
		}
		a->ord = nord;
	}
	if (remap)
		for (uint32 t = 0; t < ex->ntouch; t++)
			if (!ex->arm[ex->touched[t].arm].local)
				ex->touched[t].g = remap[ex->touched[t].g];
}

/* the arrays of a local arm for room cells */
static void
cells_alloc(AggExec *ex, ArmSt *a, int ninner, uint32 room)
{
	a->ccap = room;
	a->cid = MemoryContextAlloc(ex->cxt, (Size) room * a->nlk * sizeof(uint32));
	for (int ai = 0; ai < ninner; ai++)
	{
		a->n[ai] = MemoryContextAlloc(ex->cxt, room * sizeof(int64));
		a->i[ai] = MemoryContextAlloc(ex->cxt, room * sizeof(int64));
		a->d[ai] = MemoryContextAlloc(ex->cxt, room * sizeof(double));
		a->nf[ai] = MemoryContextAlloc(ex->cxt, room);
	}
}

/* the slot of the row's table that the ids of a cell start at */
static inline uint32
cell_slot(const ArmSt *a, const uint32 *ids)
{
	uint32		pos = ids_hash(ids, a->nlk, a->lcap);

	while ((a->ltab[pos] >> 32) == a->stamp)
		pos = (pos + 1) & (a->lcap - 1);
	return pos;
}

/* room for twice the cells, and a table of four slots each, in which the cells of the row go again */
static void
cells_grow(AggExec *ex, int k, ArmSt *a)
{
	int			ninner = ex->spec->arm[k].ninner;

	a->ccap *= 2;
	a->cid = repalloc(a->cid, (Size) a->ccap * a->nlk * sizeof(uint32));
	for (int ai = 0; ai < ninner; ai++)
	{
		a->n[ai] = repalloc(a->n[ai], a->ccap * sizeof(int64));
		a->i[ai] = repalloc(a->i[ai], a->ccap * sizeof(int64));
		a->d[ai] = repalloc(a->d[ai], a->ccap * sizeof(double));
		a->nf[ai] = repalloc(a->nf[ai], a->ccap);
	}
	pfree(a->ltab);
	a->lcap = 4 * a->ccap;
	a->ltab = MemoryContextAllocZero(ex->cxt, (Size) a->lcap * sizeof(uint64));
	for (uint32 c = 0; c < a->nc; c++)
		a->ltab[cell_slot(a, &a->cid[(Size) c * a->nlk])] = (uint64) a->stamp << 32 | (c + 1);
}

void
arms_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	ex->arm = MemoryContextAllocZero(ex->cxt, Max(s->narms, 1) * sizeof(ArmSt));
	ex->arm_mask = ~(uint32) 0;
	ex->cur_order = UINT64_MAX;
	ex->setg = MemoryContextAlloc(ex->cxt, Max(s->nsets, 1) * sizeof(uint32));
	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		int			ni = s->arm[k].ninner;

		a->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		a->ok = MemoryContextAlloc(ex->cxt, maxv);
		if (s->arm[k].hexpr)
		{
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			a->hstate = ExecInitExpr((Expr *) s->arm[k].hexpr, EXPR_PARENT(ex));
			MemoryContextSwitchTo(old);
		}
		a->iok = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->dx = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->ix = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->n = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->i = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->d = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->nf = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->amax = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double));
		for (int ai = 0; ai < ni; ai++)
		{
			a->iok[ai] = MemoryContextAlloc(ex->cxt, maxv);
			a->dx[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(double));
			a->ix[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(int64));
		}
		if (s->arm[k].hmask == 0)
			continue;
		a->local = true;
		memset(a->lpos, -1, sizeof(a->lpos));
		for (int j = 0; j < s->nkeys; j++)
			if ((s->arm[k].keymask | s->arm[k].hmask) >> j & 1)
			{
				a->lpos[j] = a->nlk;
				a->lk[a->nlk++] = j;
			}
		a->kid = MemoryContextAlloc(ex->cxt, (Size) s->nkeys * maxv * sizeof(uint32));
		cells_alloc(ex, a, ni, 16);
		a->lcap = 64;
		a->ltab = MemoryContextAllocZero(ex->cxt, a->lcap * sizeof(uint64));
		a->stamp = 1;
	}
	ex->captouch = 1024;
	ex->touched = MemoryContextAlloc(ex->cxt, ex->captouch * sizeof(Touch));
}

/* the inner aggregate ai of arm k for the tuples of the chunk: the ones it takes and their values */
static void
arm_inner_prepare(AggExec *ex, ArmSt *a, SiftAggAgg *ia, int ai, int n)
{
	bool		all;
	const uint32 *idx,
			   *out;
	const SiftSrcVec *v;

	agg_selection(ex, ia, ex->cstart, ex->cn, n, &all);
	for (int j = 0; j < n; j++)
		a->iok[ai][j] = all || sift_colsrc_bit(ex->okm, j);
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			v = get_vec(ex, ia->leaf);
			gather_idx(ex, ia->leaf, n, &idx);
			for (int j = 0; j < n; j++)
				a->ix[ai][j] = ((const int32 *) v->data)[idx[j]];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			if (ia->nvx)
				memcpy(a->dx[ai], eval_vx(ex, ia, n), n * sizeof(double));
			else
			{
				v = get_vec(ex, ia->leaf);
				gather_idx(ex, ia->leaf, n, &idx);
				for (int j = 0; j < n; j++)
					a->dx[ai][j] = ((const double *) v->data)[idx[j]];
			}
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
			out = combo_outs(ex, ia->combo);
			for (int j = 0; j < n; j++)
			{
				uint32		id = out[j] == COMBO_NONE ? 0 : combo_val(ex, ia->combo, out[j], ia->slot);

				a->iok[ai][j] &= id != 0;
				a->dx[ai][j] = id ? DatumGetFloat8(ex->cs[ia->combo].dat[id - 1]) : 0;
			}
			break;
		default:
			break;
	}
}

static void
cell_start(AggExec *ex, int k, ArmSt *a, uint32 g, uint64 row)
{
	if (!a->local)
	{
		a->ord[g] = row;
		ex->seen[g]++;
	}
	for (int ai = 0; ai < ex->spec->arm[k].ninner; ai++)
	{
		a->n[ai][g] = 0;
		a->i[ai][g] = 0;
		a->d[ai][g] = 0;
		a->nf[ai][g] = 0;
	}
	if (ex->ntouch == ex->captouch)
	{
		ex->captouch *= 2;
		ex->touched = repalloc(ex->touched, ex->captouch * sizeof(Touch));
	}
	ex->touched[ex->ntouch].arm = k;
	ex->touched[ex->ntouch++].g = g;
}

/* the cell of tuple j of a local arm in the row: made when the row has none with the tuple's key ids */
static uint32
cell_find(AggExec *ex, int k, ArmSt *a, int j, uint64 row)
{
	uint32		ids[SIFT_AGG_MAX_KEYS],
				pos,
				c;

	for (int x = 0; x < a->nlk; x++)
		ids[x] = a->kid[(Size) a->lk[x] * (VEC + PADW) + j];
	pos = ids_hash(ids, a->nlk, a->lcap);
	for (; (a->ltab[pos] >> 32) == a->stamp; pos = (pos + 1) & (a->lcap - 1))
	{
		c = (uint32) a->ltab[pos] - 1;
		if (memcmp(&a->cid[(Size) c * a->nlk], ids, a->nlk * sizeof(uint32)) == 0)
			return c;
	}
	if (a->nc == a->ccap)
	{
		cells_grow(ex, k, a);
		pos = cell_slot(a, ids);
	}
	c = a->nc++;
	memcpy(&a->cid[(Size) c * a->nlk], ids, a->nlk * sizeof(uint32));
	a->ltab[pos] = (uint64) a->stamp << 32 | (c + 1);
	cell_start(ex, k, a, c, row);
	return c;
}

static void
cell_add(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, int j)
{
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			a->i[ai][g] += a->ix[ai][j];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_X_SUM:
		case SAA_X_AVG:
			{
				double		x = a->dx[ai][j];

				a->d[ai][g] += x;
				if (!isfinite(x))
					a->nf[ai][g] = 1;
				else
					a->amax[ai] = Max(a->amax[ai], fabs(x));
			}
			break;
		default:
			break;
	}
	a->n[ai][g]++;
}

/* the value of inner aggregate ai in a cell: false for NULL */
static bool
cell_value(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, double *dv, int64 *iv)
{
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
			*iv = a->n[ai][g];
			return true;
		case SAA_SUM_INT:
			*iv = a->i[ai][g];
			return a->n[ai][g] > 0;
		case SAA_SUM_F64:
		case SAA_X_SUM:
			if (isinf(a->d[ai][g]) && !a->nf[ai][g])
				float_overflow_error();
			*dv = a->d[ai][g];
			return a->n[ai][g] > 0;
		case SAA_AVG_F64:
		case SAA_X_AVG:
			if (a->n[ai][g] == 0)
				return false;
			*dv = a->d[ai][g] / (double) a->n[ai][g];
			return true;
		default:
			elog(ERROR, "sift: unexpected inner aggregate");
	}
	return false;
}

static bool
cond_holds(const SiftAggCond *c, double dv, int64 iv)
{
	int			cmp = c->isint ? pg_cmp_s64(iv, c->ci) : float8_cmp_internal(dv, c->cd);

	switch ((SiftCmpOp) c->op)
	{
		case SCMP_EQ:
			return cmp == 0;
		case SCMP_NE:
			return cmp != 0;
		case SCMP_LT:
			return cmp < 0;
		case SCMP_LE:
			return cmp <= 0;
		case SCMP_GT:
			return cmp > 0;
		case SCMP_GE:
			return cmp >= 0;
	}
	return false;
}

/* the arm's HAVING that is not `inner aggregate op constant`, evaluated by PostgreSQL on the values of the cell */
static bool
cell_having(AggExec *ex, int k, uint32 g)
{
	SiftAggArm *sa = &ex->spec->arm[k];
	bool		isnull;
	Datum		d;

	for (int j = 0; j < sa->nhp; j++)
	{
		SiftAggAgg *ia = &sa->inner[sa->hsrc[j]];
		double		dv = 0;
		int64		iv = 0;
		bool		notnull = cell_value(&ex->arm[k], ia, sa->hsrc[j], g, &dv, &iv);

		bind_param(ex, j, ia->kind == SAA_SUM_F64 || ia->kind == SAA_AVG_F64 ? Float8GetDatum(dv) : Int64GetDatum(iv), !notnull);
	}
	d = ExecEvalExprSwitchContext(ex->arm[k].hstate, ex->lutcx, &isnull);
	ResetExprContext(ex->lutcx);
	return !isnull && DatumGetBool(d);
}

/* the id of key j in cell g of arm k (0: NULL, or a key the arm does not have) */
static inline uint32
cell_key(AggExec *ex, int k, uint32 g, int j)
{
	const ArmSt *a = &ex->arm[k];

	if (!a->local)
		return key_id(ex, j, g);
	return a->lpos[j] < 0 ? 0 : a->cid[(Size) g * a->nlk + a->lpos[j]];
}

/* the outer aggregates of group og take the cell (arm k, group g) of the row `row` */
static void
cell_into(AggExec *ex, int k, uint32 g, uint32 og, uint64 row)
{
	SiftAggArm *sa = &ex->spec->arm[k];
	ArmSt	   *a = &ex->arm[k];

	for (int oi = 0; oi < ex->spec->naggs; oi++)
	{
		SiftAggAgg *o = &ex->spec->agg[oi];
		AggAcc	   *acc = &ex->acc[oi];
		bool		pass = true;
		double		dv = 0;
		int64		iv = 0;

		for (int c = 0; pass && c < o->ncond; c++)
		{
			int			src = sa->colsrc[o->cond[c].col];

			pass = cell_value(a, &sa->inner[src], src, g, &dv, &iv) && cond_holds(&o->cond[c], dv, iv);
		}
		if (!pass || o->kind == SAA_GROUPING)
			continue;
		if (o->kind == SAA_O_COUNT_STAR)
		{
			acc->n[og]++;
			continue;
		}
		if (o->kind == SAA_O_COUNT_ROWS)
		{
			if (acc->last[og] != row)
			{
				acc->last[og] = row;
				acc->n[og]++;
			}
			continue;
		}
		if (o->kind == SAA_O_COUNT_KEY)
		{
			/* the cell's id of the key, 0 for NULL: an arm that does not group by it has none */
			uint32		id = cell_key(ex, k, g, o->slot);

			if (id != 0 && dist_add(ex, ex->dist[oi], og, id))
				acc->n[og]++;
			continue;
		}
		if (!cell_value(a, &sa->inner[sa->colsrc[o->col]], sa->colsrc[o->col], g, &dv, &iv))
			continue;
		acc->n[og]++;
		if (o->kind == SAA_O_SUM_I64)
		{
			if (pg_add_s64_overflow(acc->i[og], iv, &acc->i[og]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("sift: a sum of the aggregate scan exceeds bigint"),
						 errhint("Set sift.enable_agg to off for this query.")));
		}
		else
		{
			acc->d[og] += dv;
			if (!isfinite(dv))
				acc->nf[og] = 1;
		}
	}
}

/*
 * The group that cell g of arm k belongs to (in grouping set `set`): the outer keys the arm (and the set) have, and the set's number.
 * The cell itself is a group of the arm's keys with the hidden ones, which no group keeps.
 */
static uint32
cell_outer_gid(AggExec *ex, int k, uint32 g, int set)
{
	SiftAggSpec *s = ex->spec;
	uint32		mask = s->arm[k].keymask,
				ids[SIFT_AGG_MAX_KEYS] = {0},
				gid = 0;

	if (s->nsets)
	{
		mask &= s->setmask[set];
		ids[s->nkeys - 1] = set + 1;
	}
	for (int j = 0; j < s->nkeys; j++)
		if (mask >> j & 1)
			ids[j] = cell_key(ex, k, g, j);
	if (ex->hash)
		return hash_group(ex, ids);
	for (int j = 0; j < s->nkeys; j++)
		gid += ids[j] * ex->ks[j].mult;
	return gid;
}

/*
 * The cell (arm k, group g) of the row `row` is done: its group exists if it passes the HAVING of the arm, once per grouping set in the
 * sets' groups (the cell stands for the inner group, so a coarser set adds every cell it covers to its group).
 */
static void
flush_cell(AggExec *ex, int k, uint32 g, uint64 row)
{
	SiftAggSpec *s = ex->spec;
	SiftAggArm *sa = &s->arm[k];
	ArmSt	   *a = &ex->arm[k];

	if (!a->local)
		ex->seen[g]--;
	for (int h = 0; h < sa->nhaving; h++)
	{
		double		dv = 0;
		int64		iv = 0;

		if (!cell_value(a, &sa->inner[sa->having[h].col], sa->having[h].col, g, &dv, &iv) || !cond_holds(&sa->having[h], dv, iv))
			return;				/* the cell is no group of the arm */
	}
	if (a->hstate && !cell_having(ex, k, g))
		return;
	if (s->nsets == 0 && sa->hmask == 0)
	{
		ex->seen[g]++;
		cell_into(ex, k, g, g, row);
		return;
	}
	for (int set = 0; set < Max(s->nsets, 1); set++)
		ex->setg[set] = cell_outer_gid(ex, k, g, set);
	if (ex->hash && ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		ex->seen[ex->setg[set]]++;
		cell_into(ex, k, g, ex->setg[set], row);
	}
}

void
arms_flush(AggExec *ex)
{
	for (uint32 t = 0; t < ex->ntouch; t++)
		flush_cell(ex, ex->touched[t].arm, ex->touched[t].g, ex->cur_order);
	ex->ntouch = 0;
	for (int k = 0; k < ex->spec->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];

		if (!a->local || a->nc == 0)
			continue;
		a->nc = 0;
		if (++a->stamp == 0)
		{
			memset(a->ltab, 0, a->lcap * sizeof(uint64));
			a->stamp = 1;
		}
	}
}

/* the tuples of the chunk through every arm: the tuples of a row come together, so a cell lives for one row */
void
arm_chunk(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;
	int			n = t->n;
	uint32		gen;

	ex->n_tuples += n;
	do
	{
		gen = ex->gen;
		for (int k = 0; k < s->narms && gen == ex->gen; k++)
		{
			ArmSt	   *a = &ex->arm[k];

			ex->arm_mask = s->arm[k].keymask | s->arm[k].hmask;
			if (a->local)
			{
				/* the cell is found by the ids; the group table is the outer keys' (and the set's) and needs room for their ids only */
				uint32		growmask = s->arm[k].keymask | (s->nsets ? (uint32) 1 << (s->nkeys - 1) : 0);

				compute_kids(ex, n, a->kid);
				for (int j = 0; !ex->hash && j < s->nkeys; j++)
					if ((growmask >> j & 1) && key_used(&ex->ks[j]) > ex->ks[j].cap)
					{
						direct_grow(ex);
						break;
					}
				continue;
			}
			compute_gids(ex, n);
			memcpy(a->gid, ex->gid, n * sizeof(uint32));
		}
	} while (gen != ex->gen);
	ex->arm_mask = ~(uint32) 0;
	for (int k = 0; k < s->narms; k++)
	{
		SiftAggArm *sa = &s->arm[k];
		ArmSt	   *a = &ex->arm[k];

		memset(a->ok, 1, n);
		for (int f = 0; f < sa->nfilter; f++)
		{
			const SiftAggPred *p = sa->filter[f];
			const uint32 *out = combo_outs(ex, p->leaf);

			for (int j = 0; j < n; j++)
				a->ok[j] &= combo_val(ex, p->leaf, out[j], p->op) != 0;
		}
		for (int ai = 0; ai < sa->ninner; ai++)
			arm_inner_prepare(ex, a, &sa->inner[ai], ai, n);
	}
	for (int j = 0; j < n; j++)
	{
		uint64		row = ex->row_base + ex->ridx[j];

		if (row != ex->cur_order)
		{
			arms_flush(ex);
			ex->cur_order = row;
		}
		for (int k = 0; k < s->narms; k++)
		{
			ArmSt	   *a = &ex->arm[k];
			SiftAggArm *sa = &s->arm[k];
			uint32		g;

			if (!a->ok[j])
				continue;
			if (a->local)
				g = cell_find(ex, k, a, j, row);
			else if (a->ord[(g = a->gid[j])] != row)
				cell_start(ex, k, a, g, row);
			for (int ai = 0; ai < sa->ninner; ai++)
				if (a->iok[ai][j])
					cell_add(a, &sa->inner[ai], ai, g, j);
		}
	}
	for (int k = 0; k < s->narms; k++)
		for (int ai = 0; ai < s->arm[k].ninner; ai++)
			if ((s->arm[k].inner[ai].kind == SAA_AVG_F64 || s->arm[k].inner[ai].kind == SAA_X_AVG) &&
				8.0 * ex->n_tuples * ex->arm[k].amax[ai] * ex->arm[k].amax[ai] >= DBL_MAX)
				float_overflow_error();
}
