/*
 * agg/init.c - scan setup from the spec: states, predicates, keys, combos, tables, buffers
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

static bool
param_walker(Node *n, Param **found)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < SIFT_AGG_INIT_PARAM)
	{
		*found = (Param *) n;
		return true;
	}
	return expression_tree_walker(n, param_walker, found);
}

/* the placeholder Param 0 of an expression: the type its input has */
static Param *
find_param(Node *expr)
{
	Param	   *p = NULL;

	param_walker(expr, &p);
	if (p == NULL)
		elog(ERROR, "sift: aggregate plan expression without its input");
	return p;
}

/*
 * The output of an ARRAY sub-select (an InitPlan of the query, its Param offset by SIFT_AGG_INIT_PARAM) is the constant of the expression: the sub-select
 * runs now, the first time the lookup table needs it as in the normal plan, and the lookup table's own parameters are not the executor's.
 */
static Node *
init_param_mutator(Node *n, AggExec *ex)
{
	Param	   *p = (Param *) n;
	EState	   *estate = ex->css.ss.ps.state;
	ParamExecData *prm;
	int16		len;
	bool		byval;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Param) || p->paramkind != PARAM_EXEC || p->paramid < SIFT_AGG_INIT_PARAM)
		return expression_tree_mutator(n, init_param_mutator, ex);
	prm = &estate->es_param_exec_vals[p->paramid - SIFT_AGG_INIT_PARAM];
	if (prm->execPlan != NULL)
		ExecSetParamPlan(prm->execPlan, GetPerTupleExprContext(estate));
	get_typlenbyval(p->paramtype, &len, &byval);
	return (Node *) makeConst(p->paramtype, p->paramtypmod, p->paramcollid, len, prm->value, prm->isnull, byval);
}

/*
 * The expression of a lookup table. A sub-plan in it (its state, hash table and the scans it starts) lives in the query's memory: PostgreSQL ends
 * the sub-plans after this node, which frees its own memory.
 */
static ExprState *
init_lut_expr(AggExec *ex, Node *expr)
{
	MemoryContext old = MemoryContextSwitchTo(contain_subplans(expr) ? ex->css.ss.ps.state->es_query_cxt : ex->cxt);
	ExprState  *e = ExecInitExpr((Expr *) init_param_mutator(expr, ex), EXPR_PARENT(ex));

	MemoryContextSwitchTo(old);
	return e;
}

void
extend_layout(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcLayout *lay = MemoryContextAlloc(ex->cxt, sizeof(SiftSrcLayout));

	ex->nbase = ex->lay->nleaves;
	if (s->nvleaf == 0)
		return;
	*lay = *ex->lay;
	lay->leaf = MemoryContextAllocZero(ex->cxt, (ex->nbase + s->nvleaf) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, ex->lay->leaf, ex->nbase * sizeof(SiftSrcLeaf));
	for (int j = 0; j < s->nvleaf; j++)
	{
		SiftSrcLeaf *v = &lay->leaf[ex->nbase + j];

		for (int i = 0; i < ex->nbase && sift_aggvleaf_member(&s->vleaf[j]); i++)
			if (lay->leaf[i].parent == s->vleaf[j].obj)
				*(strcmp(lay->leaf[i].name, "key") == 0 ? &ex->vl[j].key : &ex->vl[j].val) = i;
		v->id = ex->nbase + j;
		v->attno = lay->leaf[s->vleaf[j].obj].attno;
		v->parent = -1;
		v->kind = SCL_SCALAR;
		v->vtype = s->vleaf[j].types ? SCV_CODE : s->vleaf[j].count ? SCV_I32 : lay->leaf[ex->vl[j].val].vtype;
		v->typid = s->vleaf[j].types ? TEXTOID : s->vleaf[j].count ? INT4OID : lay->leaf[ex->vl[j].val].typid;
	}
	lay->nleaves += s->nvleaf;
	ex->lay = lay;
}

/* does a combo take a column of the rows combo ci matched (a lookup key or expression over an earlier lookup's output)? */
static bool
combo_read(AggExec *ex, int ci)
{
	for (int o = 0; o < ex->spec->ncombo; o++)
		for (int p = 0; p < ex->spec->combo[o].nleaves; p++)
			if (o != ci && ex->spec->combo[o].leaf[p] < 0 && ex->spec->combo[o].refcombo[p] == ci)
				return true;
	return false;
}

/* a join whose keys the scan's key filter has tested passes every tuple that reaches it (an anti join does not, and neither a repeated key of an inner join) */
static void
semi_pass(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];

	if (!cs->kfilt || j->kind == SJ_ANTI || (j->kind == SJ_INNER && !map_unique(cs->map)))
		return;
	cs->spass = true;
	ex->njpass++;
}

static void
semi_lut_build(AggExec *ex, int ci)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	const MapTab *m = cs->map;
	int64		lo = PG_INT32_MAX,
				hi = PG_INT32_MIN;
	bool	   *keep;

	if ((j->kind != SJ_SEMI && j->kind != SJ_ANTI && j->kind != SJ_INNER) || j->nkey != 1 || j->keypos[0] < 0 || cb->nleaves != 1 || m->ktext[0] ||
		cb->leaf[0] < 0 || cb->leaf[0] >= ex->nbase || !sift_agg_semi_lut || combo_read(ex, ci) ||
		cb->nslots != (j->fslot >= 0 ? 1 : 0) || (j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
		return;
	if (ex->lay->leaf[cb->leaf[0]].vtype != SCV_I32)
	{
		semi_pass(ex, ci);
		return;
	}
	/* the rest of the ON clause (the table's own conditions) holds or not per row of the table: once, not per probe; a key outside int32 matches no leaf */
	keep = palloc(Max(m->nrow, 1) * sizeof(bool));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		keep[r] = m->kint[r] >= PG_INT32_MIN && m->kint[r] <= PG_INT32_MAX;
		if (keep[r] && j->fslot >= 0 && !m->filtered)
		{
			bool		isnull;
			Datum		d;

			for (int c = 0; c < j->ncol; c++)
				bind_param(ex, cb->nleaves + c, m->col[(size_t) r * m->ncol + c], m->cnull[(size_t) r * m->ncol + c]);
			d = ExecEvalExprSwitchContext(cs->slot[j->fslot], ex->lutcx, &isnull);
			keep[r] = !isnull && DatumGetBool(d);
			if ((r & 1023) == 1023)
				ResetExprContext(ex->lutcx);
		}
		if (keep[r])
		{
			lo = Min(lo, m->kint[r]);
			hi = Max(hi, m->kint[r]);
		}
	}
	ResetExprContext(ex->lutcx);
	if (lo <= hi && hi - lo + 1 > (int64) sift_agg_semi_lut_size * 1024)
	{
		pfree(keep);
		semi_pass(ex, ci);
		return;
	}
	cs->slo = lo <= hi ? (int32) lo : 0;
	cs->snlut = lo <= hi ? (uint32) (hi - lo + 1) : 0;
	cs->slut = MemoryContextAllocZero(ex->cxt, cs->snlut + SIFT_AGG_PAD);
	for (uint32 r = 0; r < m->nrow; r++)
		if (keep[r])
		{
			/* an inner join repeats a tuple per row of its key: only a key that holds once is a membership test */
			if (cs->slut[m->kint[r] - cs->slo] && j->kind == SJ_INNER)
			{
				pfree(cs->slut);
				cs->slut = NULL;
				cs->snlut = 0;
				pfree(keep);
				return;
			}
			cs->slut[m->kint[r] - cs->slo] = 1;
		}
	pfree(keep);
	ex->nsemi++;
	ex->semi_bytes += cs->snlut;
}

/* the tuples of `in` that pass a semi (the key is in the table) or anti (it is not, or NULL) join of the table, to the next stage */

/* ---------- the scan ---------- */

void
init_states(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->ps = MemoryContextAllocZero(ex->cxt, Max(s->npreds_total, 1) * sizeof(PredState));
	ex->ks = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(KeyState));
}

static void
register_pred(AggExec *ex, SiftAggPred *p)
{
	PredState  *st;

	if (p == NULL)
		return;
	st = &ex->ps[p->id];
	if (p->kind == SAP_LUT)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		st->expr = init_lut_expr(ex, p->expr);
		st->param = find_param(p->expr);
		rt_init(&st->rt, 1);
		if (ex->lay->leaf[p->leaf].vtype == SCV_I32)
			st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < p->nchild; i++)
		register_pred(ex, p->child[i]);
}

/* every predicate the scan evaluates: the conjuncts of the rows, the FILTER of the aggregates and of the inner aggregates of the arms */
void
register_preds(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->npred; i++)
		register_pred(ex, s->pred[i]);
	for (int i = 0; i < s->naggs; i++)
		register_pred(ex, s->agg[i].filter);
	for (int k = 0; k < s->narms; k++)
		for (int i = 0; i < s->arm[k].ninner; i++)
			register_pred(ex, s->arm[k].inner[i].filter);
}

/* The interval each leaf's comparisons leave: a unit whose zone misses it holds no row that passes (docs/COL-PLAN.md, "Zone pruning") */
void
prune_init(AggExec *ex)
{
	List	   *cmps = sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase);

	ex->prune = MemoryContextAlloc(ex->cxt, Max(list_length(cmps), 1) * sizeof(SiftSrcPrune));
	foreach_ptr(SiftAggPred, p, cmps)
	{
		PredState  *st = &ex->ps[p->id];
		SiftSrcPrune *z = NULL;
		int64		lo = PG_INT64_MIN,
					hi = PG_INT64_MAX;

		if (!st->ready)
			eval_operand(ex, p, st);
		if (st->cnull)
			continue;
		if (p->kind == SAP_IN)
		{
			if (st->in_nv == 0)
				continue;
			lo = st->in_lo;
			hi = st->in_hi;
		}
		else
		{
			bool		num = ex->lay->leaf[p->leaf].typid == NUMERICOID;
			SiftCmpOp	op = num ? (SiftCmpOp) st->nop : (SiftCmpOp) p->op;
			int64		c = num ? (int64) st->cn : st->ci;

			if (num && st->konst >= 0)
			{
				/* one result for every number (a constant no scaled integer equals, or beyond all of them): the whole range passes, or no value does */
				if (st->konst == 0)
				{
					lo = PG_INT64_MAX;
					hi = PG_INT64_MIN;
				}
			}
			else
			{
				if (op != SCMP_GT && op != SCMP_GE)
					hi = op == SCMP_LT && c > PG_INT64_MIN ? c - 1 : c;
				if (op != SCMP_LT && op != SCMP_LE)
					lo = op == SCMP_GT && c < PG_INT64_MAX ? c + 1 : c;
			}
		}
		for (int k = 0; k < ex->nprune; k++)
			if (ex->prune[k].leaf == p->leaf)
				z = &ex->prune[k];
		if (z == NULL)
		{
			z = &ex->prune[ex->nprune++];
			*z = (SiftSrcPrune) {p->leaf, PG_INT64_MIN, PG_INT64_MAX};
		}
		z->hi = Min(z->hi, hi);
		z->lo = Max(z->lo, lo);
	}
}

/* The text tests of the query a source can run on the encoded form: they prune its units and narrow the rows of each (process_unit) */
void
str_init(AggExec *ex)
{
	SiftSrcStrTest *all = palloc(Max(ex->spec->npred, 1) * sizeof(SiftSrcStrTest));
	int			n = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (sift_aggspec_str_test(ex->spec->pred[i], ex->lay, ex->nbase, &all[n]))
			ex->ps[ex->spec->pred[i]->id].str = &all[n++];
	if (n > 0)
		sift_colsrc_prune_str(ex->src, all, n);
}

/*
 * The conjuncts the source can apply on its stored integer values, as filters: a comparison of an integer-valued leaf with a constant (a
 * numeric as its scaled integer) and a short IN list. The text tests have their own path (str_init).
 */
#define FILTER_MAX_SET	8

/* a comparison or short IN list of an integer-valued leaf */
static bool
int_filter(AggExec *ex, SiftAggPred *p, SiftSrcFilter *f)
{
	PredState  *st = &ex->ps[p->id];
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];

	if (lf->vtype != SCV_I32 && lf->vtype != SCV_I64)
		return false;
	if (!st->ready)
		eval_operand(ex, p, st);
	if (st->cnull)
		return false;
	f->leaf = p->leaf;
	if (p->kind == SAP_CMP)
	{
		bool		num = lf->typid == NUMERICOID;

		if (num && st->konst >= 0)
		{
			/* every value passes (a NaN too: not null) or none does; the NaN rows of the others are the executor's */
			if (st->konst == 0 || !st->nan_res)
				return false;
			f->op = SSF_NOTNULL;
		}
		else
		{
			f->op = (SiftSrcFilterOp) (num ? st->nop : p->op);
			f->val = num ? (int64) st->cn : st->ci;
		}
		return true;
	}
	{
		int64	   *set = palloc(FILTER_MAX_SET * sizeof(int64));
		int			n = 0;

		if ((p->op && st->in_hasnull) || st->in_htab)
			return false;
		for (uint32 i = 0; i < st->in_nlut; i++)
			if (st->in_lut[i] && n++ < FILTER_MAX_SET)
				set[n - 1] = (int64) st->in_lo + i;
		if (n == 0 || n > FILTER_MAX_SET)
			return false;
		f->op = p->op ? SSF_NOTIN : SSF_IN;
		f->nset = n;
		f->set = set;
		return true;
	}
}

void
filter_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcFilter f[SIFT_COLSRC_MAX_FILTERS];
	int			at[SIFT_COLSRC_MAX_FILTERS],
				n = 0;
	uint64		ok;

	ex->pushed = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(bool));
	ex->pflt = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(SiftSrcFilter));
	for (int i = 0; i < s->npred && n < SIFT_COLSRC_MAX_FILTERS; i++)
	{
		SiftAggPred *p = s->pred[i];

		if (p->level != 0 || p->late || p->leaf >= ex->nbase || (p->kind != SAP_CMP && p->kind != SAP_IN))
			continue;
		memset(&f[n], 0, sizeof(SiftSrcFilter));
		if (int_filter(ex, p, &f[n]))
			at[n++] = i;
	}
	/* the source tests the stored rows of a unit, the rows of a match set may be renumbered: with a search the executor tests them */
	ok = n > 0 && s->search == NULL ? sift_colsrc_set_filters(ex->src, f, n) : 0;
	for (int k = 0; k < n; k++)
		if (ok >> k & 1)
		{
			ex->pushed[at[k]] = true;
			ex->pflt[at[k]] = f[k];
			ex->nfilter++;
		}
}

/* the leaves the source reads again from the heap for the rows that hold a value they cannot store */
void
recheck_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE)
		{
			sift_colsrc_set_recheck(ex->src, s->leaves[i], s->recheck[i]);
			ex->recheck = true;
		}
}

/*
 * The conjuncts on the JSON type of a leaf's path (SAP_TYPE) are the source's: it keeps the rows of the types and raises for one whose value
 * it cannot give. The other rows may hold such a value in this leaf, which its reads then give as NULL.
 */
void
type_init(AggExec *ex)
{
	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_TYPE)
		{
			sift_colsrc_tolerate_ovf(ex->src, ex->spec->pred[i]->leaf);
			ex->pushed[i] = true;
			ex->ntype++;
		}
}

/*
 * The conjuncts on wide leaves (SAP_WIDE) are the source's: their operands are evaluated once, as Datums of the leaf's type (an IN list as the
 * elements of its array, a NULL operand as isnull), and the source returns the rows that pass test k of wide_filters.
 */
static void
wide_operand(AggExec *ex, const SiftAggPred *p, SiftSrcWideTest *t)
{
	bool		isnull;
	ExprState  *e;
	Datum		d;
	Datum	   *vals = palloc(sizeof(Datum));
	bool	   *nulls = palloc(sizeof(bool));
	int16		len;
	bool		byval;
	char		align;

	if (p->op == SWT_ISNULL || p->op == SWT_NOTNULL)
		return;
	e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	get_typlenbyvalalign(p->argtype, &len, &byval, &align);
	t->nvals = 1;
	nulls[0] = isnull;
	if (isnull)
		vals[0] = (Datum) 0;
	else if (p->op == SWT_IN || p->op == SWT_NOTIN)
		deconstruct_array(DatumGetArrayTypePCopy(d), p->argtype, len, byval, align, &vals, &nulls, &t->nvals);
	else
		vals[0] = datumCopy(d, byval, len);
	t->vals = vals;
	t->isnull = nulls;
}

void
wide_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcWideTest t[SIFT_AGG_MAX_WIDE];
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	int			n = 0;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE)
		{
			t[n] = (SiftSrcWideTest) {.leaf = s->pred[i]->leaf, .op = s->pred[i]->op};
			wide_operand(ex, s->pred[i], &t[n++]);
			ex->pushed[i] = true;
		}
	if (n > 0 && sift_colsrc_set_wide(ex->src, t, n) != n)
		elog(ERROR, "sift: the source did not take the conjuncts on wide leaves");
	ex->nwide = n;
	MemoryContextSwitchTo(old);
}

/* The TID range of the SAP_TID conjuncts: the scan reads the superblocks it touches, and a heap's edge rows are looked up for their visible version */
void
tid_init(AggExec *ex, Snapshot snapshot)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->tid_pred = -1;
	ex->tid_sb_hi = PG_UINT32_MAX;
	for (int i = 0; i < s->npred && ex->tid_pred < 0; i++)
		if (s->pred[i]->kind == SAP_TID)
			ex->tid_pred = s->pred[i]->id;
	if (ex->tid_pred >= 0)
	{
		sift_aggspec_tid_range(s, &ex->tid_lo, &ex->tid_hi);
		ex->tid_sb_hi = 0;
		ex->tid_rows = palloc(VEC * sizeof(uint32));
		ex->tid_tids = palloc(VEC * sizeof(ItemPointerData));
		if (ex->tid_lo < ex->tid_hi)
		{
			ex->tid_blo = ex->tid_lo >> 16;
			ex->tid_bhi = (ex->tid_hi - 1) >> 16;
			ex->tid_sb_lo = ex->tid_blo / SIFT_SB_PAGES;
			ex->tid_sb_hi = ex->tid_bhi / SIFT_SB_PAGES + 1;
		}
		if (!ex->lay->is_table)
		{
			ex->tid_vis = palloc(sizeof(SiftVis));
			sift_vis_begin(ex->tid_vis, ex->heap, snapshot, NULL, 0);
			ex->tid_slot = table_slot_create(ex->heap, NULL);
		}
	}
	MemoryContextSwitchTo(old);
}

/* what a key keeps for the whole scan: its expression and the type of its input; the ids are the table's (table_init) */
void
register_key(AggExec *ex, int ki)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcLeaf *lf = key_lf(ex, ki);
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	if (sk->kind == SAK_SET)
		k->ident = true;
	else if (sk->kind == SAK_LUT)
	{
		k->expr = init_lut_expr(ex, sk->expr);
		k->param = find_param(sk->expr);
		k->bucket = (int64) sk->bucket * USECS_PER_SEC;
	}
	else if (sk->kind == SAK_NUM)
	{
		k->numk = true;
		idmap_init(&k->im);
	}
	else if (sk->kind == SAK_LEAF)
	{
		/* the leaf itself: the placeholder has the key's type */
		k->param = makeNode(Param);
		k->param->paramkind = PARAM_EXEC;
		k->param->paramtype = sk->typid;
		k->param->paramtypmod = sk->typmod;
		k->param->paramcollid = sk->collid;
		k->ident = lf->vtype == SCV_I32 || lf->vtype == SCV_F64;
		if (lf->vtype == SCV_F64)
			ident_go_wide(ex, k);
	}
	MemoryContextSwitchTo(old);
}

void
register_combos(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	bool		preserved = false;

	for (int j = 0; j < s->njoin; j++)
		preserved |= sift_aggjoin_preserves(s->join[j].kind);

	ex->comcxt = AllocSetContextCreate(ex->cxt, "sift agg combo", ALLOCSET_SMALL_SIZES);
	ex->cs = palloc0(Max(s->ncombo, 1) * sizeof(ComboSt));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		SiftAggCombo *cb = &s->combo[ci];
		ComboSt    *cs = &ex->cs[ci];

		cs->ninputs = Max(cb->nleaves, 1);
		cs->cxt = AllocSetContextCreate(ex->cxt, "sift agg combo cache", ALLOCSET_DEFAULT_SIZES);
		cs->resettable = cb->join < 0;
		cs->by_id = !preserved && cb->join < 0 && cb->nleaves == 1 && cb->leaf[0] >= 0 && leaf_wide(&ex->lay->leaf[cb->leaf[0]]);
		cs->slot = palloc(Max(cb->nslots, 1) * sizeof(ExprState *));
		cs->tlen = palloc0(Max(cb->nslots, 1) * sizeof(int16));
		cs->tbyval = palloc0(Max(cb->nslots, 1) * sizeof(bool));
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			cs->slot[sl] = ExecInitExpr((Expr *) cb->slot[sl].expr, EXPR_PARENT(ex));
			get_typlenbyval(exprType(cb->slot[sl].expr), &cs->tlen[sl], &cs->tbyval[sl]);
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			const SiftSrcLeaf *lf;

			if (cb->leaf[p] < 0)
			{
				cs->resettable = false;
				continue;
			}
			lf = &ex->lay->leaf[cb->leaf[p]];

			cs->resettable &= lf->vtype != SCV_CODE;
			if (lf->typid == NUMERICOID && lf->vtype == SCV_I64 && lf->scale > 0)
				cs->unit[p] = DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("0.%0*d", lf->scale, 1)), ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(-1));
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if ((s->agg[ai].kind == SAA_X_SUM || s->agg[ai].kind == SAA_X_AVG) && s->agg[ai].restype == NUMERICOID && s->agg[ai].combo == ci)
			{
				if (!cs->decode)
					cs->decode = palloc0(Max(cb->nslots, 1) * sizeof(bool));
				cs->decode[s->agg[ai].slot] = true;
			}
		combo_reset(ex, ci);
		if (cb->join >= 0)
			foreach_ptr(Node, arg, s->join[cb->join].args)
				cs->args = lappend(cs->args, ExecInitExpr((Expr *) arg, EXPR_PARENT(ex)));
		for (int k = 0; cb->join >= 0 && k < s->join[cb->join].nkey; k++)
			if (s->join[cb->join].keyexpr[k])
				cs->kx[k] = ExecInitExpr((Expr *) s->join[cb->join].keyexpr[k], EXPR_PARENT(ex));
		ex->clbase[ci] = ex->ncl;
		ex->ncl += cb->nleaves;
		if (cb->join >= 0)
			ex->jcombo[ex->nj++] = ci;
	}
	MemoryContextSwitchTo(old);
}

/*
 * A tuple of an inner or semi join needs a row of the table with its key, so the keys of the loaded table are a conjunct of the scan,
 * `leaf IN (keys)`: the leaf's zones, the source's filters and the row bitmaps apply it before a tuple is made, where the join would
 * resolve every tuple through the combo cache. The table is loaded here (the table's own conditions already hold for its rows).
 */
static bool
join_filter_leaf(AggExec *ex, int ci, int k, int *leaf)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const SiftSrcLeaf *lf;

	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->keypos[k] < 0 || cb->level != 0)
		return false;
	*leaf = cb->leaf[j->keypos[k]];
	if (*leaf < 0 || *leaf >= ex->nbase)
		return false;
	lf = &ex->lay->leaf[*leaf];
	return lf->kind == SCL_SCALAR && lf->parent < 0 && (lf->vtype == SCV_I32 || lf->typid == INT8OID);
}

static SiftAggPred *
join_filter_pred(AggExec *ex, int ci, int k, int leaf, int id)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const MapTab *m = ex->cs[ci].map;
	PredState  *st = &ex->ps[id];
	SiftAggPred *p;
	int64	   *vals = palloc(Max(m->nrow, 1) * sizeof(int64));
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	int			nv = 0;

	for (uint32 r = 0; r < m->nrow; r++)
	{
		int64		v = m->kint[(size_t) r * m->nkey + k];

		if (ex->lay->leaf[leaf].vtype == SCV_I32 && (v < PG_INT32_MIN || v > PG_INT32_MAX))
			continue;				/* no int4 value equals it */
		vals[nv++] = v;
		lo = Min(lo, v);
		hi = Max(hi, v);
	}
	in_build(st, vals, nv, lo, hi);
	pfree(vals);
	st->ready = true;
	p = palloc0(sizeof(SiftAggPred));
	p->kind = SAP_IN;
	p->id = id;
	p->leaf = leaf;
	p->leaf2 = -1;
	p->argtype = ex->lay->leaf[leaf].typid;
	p->desc = psprintf("%s IN (keys of %s)", ex->lay->leaf[leaf].name, j->desc);
	return p;
}

void
join_filters(AggExec *ex, MemoryContext caller)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	List	   *add = NIL;
	int			total = s->npreds_total;

	for (int j = 0; j < s->njoin; j++)
		if (sift_aggjoin_preserves(s->join[j].kind))
			goto done;
	if (!sift_agg_join_filter || s->topn)
		goto done;
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggJoin *j;
		int			leaf;

		if (s->combo[ci].join < 0)
			continue;
		j = &s->join[s->combo[ci].join];
		for (int k = 0; k < j->nkey; k++)
		{
			SiftAggPred *p;

			if (!join_filter_leaf(ex, ci, k, &leaf))
				continue;
			if (ex->cs[ci].map == NULL)
			{
				/* a child plan runs in the context the scan was started in */
				MemoryContextSwitchTo(caller);
				ex->cs[ci].map = map_load(ex, ci);
				MemoryContextSwitchTo(ex->cxt);
			}
			if (ex->cs[ci].map->ktext[k])
				continue;
			/* the table of the predicate is the state of the id it is given: make room first */
			ex->ps = repalloc(ex->ps, (total + 1) * sizeof(PredState));
			memset(&ex->ps[total], 0, sizeof(PredState));
			if ((p = join_filter_pred(ex, ci, k, leaf, total)) != NULL)
			{
				add = lappend(add, p);
				total++;
				ex->cs[ci].kfilt = j->nkey == 1 && (ex->cs[ci].map->filtered || j->fslot < 0);
			}
		}
	}
	if (add != NIL)
	{
		SiftAggSpec *c = palloc(sizeof(SiftAggSpec));

		*c = *s;
		c->pred = palloc((s->npred + list_length(add)) * sizeof(SiftAggPred *));
		memcpy(c->pred, s->pred, s->npred * sizeof(SiftAggPred *));
		foreach_ptr(SiftAggPred, p, add)
			c->pred[c->npred++] = p;
		c->npreds_total = total;
		ex->spec = c;
		ex->njfilter = list_length(add);
	}
done:
	MemoryContextSwitchTo(old);
}

/* An empty group table, all in tcxt: key ids, group arrays, count(DISTINCT) sets */
void
table_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint64		total = 1;

	ex->hash = false;
	ex->direct_max = ex->lanes || ex->lane_serial ? LANE_DIRECT_MAX : DIRECT_MAX;
	ex->ngroups = 0;
	ex->hkey = NULL;
	ex->hkcap = 0;
	ex->hslot = NULL;
	ex->nhslot = 0;
	ex->row_base = 0;
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &ex->ks[j];
		KeyState	keep = {.expr = k->expr, .param = k->param, .ident = k->ident, .numk = k->numk, .bucket = k->bucket};

		*k = keep;
		ot_init(&k->ot, s->key[j].eqop, s->key[j].typid, s->key[j].collid);
		rt_init(&k->rt, sizeof(uint32));
		k->cap = 4;
		k->nullid = UINT32_MAX;
		if (s->key[j].kind == SAK_SET)
			for (int i = 0; i < s->nsets; i++)
				ident_add(ex, k, i);
		else if (k->numk)
			idmap_init(&k->im);
		else if ((s->key[j].kind == SAK_LEAF && key_lf(ex, j)->vtype == SCV_F64) || k->bucket)
			ident_go_wide(ex, k);
	}
	/* room for the groups the planner expects of one key, instead of doubling the arrays as the ids come */
	if (s->nkeys == 1 && !ex->lanes && s->est_groups > PRESIZE_MIN && s->est_groups <= ex->direct_max)
		ex->ks[0].cap = pg_nextpower2_32((uint32) s->est_groups);
	for (int j = 0; j < s->nkeys; j++)
		total = Min((uint64) ex->direct_max + 1, total * ex->ks[j].cap);
	if (total > ex->direct_max)
	{
		hash_init(ex);
		total = 1024;
	}
	else
		direct_layout(ex);
	ex->gcap = total;
	ex->seen = palloc0(total * sizeof(int64));
	ex->acc = acc_alloc(ex, total);
	arms_resize(ex, total, NULL, 0);
	ex->dset = palloc0(Max(s->naggs, 1) * sizeof(DSet));
	ex->dist = palloc0(Max(s->naggs, 1) * sizeof(Dist *));
	ex->aot = palloc0(Max(s->naggs, 1) * sizeof(OutTable));
	ex->amax = palloc0(Max(s->naggs, 1) * sizeof(double));
	ex->amap = palloc0(Max(s->naggs, 1) * sizeof(IdMap));
	ex->osa = palloc0(Max(s->naggs, 1) * sizeof(Osa *));
	ex->oval = palloc0(Max(s->naggs, 1) * sizeof(double *));
	ex->xcmp = palloc0(Max(s->naggs, 1) * sizeof(FmgrInfo *));
	ex->xlen = palloc0(Max(s->naggs, 1) * sizeof(int16));
	ex->xbyval = palloc0(Max(s->naggs, 1) * sizeof(bool));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];
		TypeCacheEntry *tc;

		if (a->kind == SAA_X_MIN || a->kind == SAA_X_MAX)
		{
			ex->xcmp[i] = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
			get_typlenbyval(a->argtype, &ex->xlen[i], &ex->xbyval[i]);
		}
		if (is_ordered_agg(a))
			ex->osa[i] = a->share == i ? osa_new(ex) : ex->osa[a->share];
		if (a->num.n && (a->kind == SAA_COUNT_DISTINCT || a->distinct))
			idmap_init(&ex->amap[i]);
		if (a->kind == SAA_GEN)
		{
			/* the hash set of a DISTINCT aggregate holds the byte images of its values; so does the table of the values of a joined table */
			for (int k = 0; k < a->gen->nargs; k++)
				if (a->gen->arg[k].mapped)
					ot_init_image(&ex->aot[i], a->gen->arg[k].typid);
			if (a->gen->distinct && !a->gen->sorted)
				ot_init_image(&ex->aot[i], a->gen->arg[a->gen->darg].typid);
			continue;
		}
		if (a->kind == SAA_COUNT_DISTINCT)
			ex->dist[i] = dist_new(ex, !a->num.n && a->leaf >= 0 && ex->lay->leaf[a->leaf].vtype != SCV_CODE);
		if ((a->kind >= SAA_M_COUNT && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG)
			ex->dset[i].counted = a->kind == SAA_M_SUM || a->kind == SAA_M_AVG || a->kind == SAA_M_PG;
		else if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || ex->lay->leaf[a->leaf].vtype != SCV_CODE)
			continue;
		tc = lookup_type_cache(a->argtype, TYPECACHE_EQ_OPR);
		ot_init(&ex->aot[i], tc->eq_opr, a->argtype, a->collid);
	}
	MemoryContextSwitchTo(old);
}

/* forgets every group (the dictionary maps hold key ids of the table) */
void
table_reset(AggExec *ex)
{
	int			nk = Max(ex->spec->nkeys + ex->spec->naggs, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].kmap)
			memset(ex->dslot[i].kmap, 0, nk * sizeof(uint32 *));
	dist_end_all(ex);
	MemoryContextReset(ex->tcxt);
	/* the output rows of a combo hold ids of the key tables just forgotten */
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		ComboSt    *cs = &ex->cs[ci];

		/* the direct table forgets the ids of the entries only: it can be megabytes */
		for (uint32 x = 0; cs->dir && x < cs->nent; x++)
			if (!cs->null_input[x] && cs->vid[x] < cs->dircap)
				cs->dir[cs->vid[x]] = 0;
		cs->nent = cs->nout = 0;
		memset(cs->ht, 0, cs->nht * sizeof(uint32));
	}
	table_init(ex);
}

void
scan_buffers(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	arms_init(ex);

	ex->t0.sel = MemoryContextAlloc(ex->cxt, (maxv + 16) * sizeof(uint32));
	ex->t0.tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->t0.ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
	{
		Tup		   *st = &ex->stage[x];

		st->sel = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		for (int y = 0; y <= x; y++)
			st->cout[ex->jcombo[y]] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		ex->seid[x] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	}
	for (int p = 0; p < SIFT_AGG_MAX_CLEAVES; p++)
		ex->cvid[p] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		ex->cfree[ci] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		if (s->combo[ci].join >= 0)
		{
			if (ex->cs[ci].map == NULL)
				ex->cs[ci].map = map_load(ex, ci);
			if (s->join[s->combo[ci].join].params == NIL)
				semi_lut_build(ex, ci);
		}
	}
	ex->calive = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->peid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pti = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pri = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->kids = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
	ex->cu32 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->c64 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	ex->okb = MemoryContextAlloc(ex->cxt, maxv);
	ex->fwin = MemoryContextAllocZero(ex->cxt, maxv);
	ex->nb = MemoryContextAlloc(ex->cxt, maxv);
	ex->vstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * maxv * sizeof(double));
	ex->pix = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->okm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm2 = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpu32 = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
}
