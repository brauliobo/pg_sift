/*
 * agg/combo/probe.c - combos: the direct probe of a correlated child
 * A correlated child of the shape [Limit 1] -> [Result with a one-time filter] -> Index Scan or Index Only Scan on a btree, whose index conditions are equalities on
 * its first columns, runs once for each distinct key of a combo. The keys a chunk adds are collected, their index conditions evaluated by the
 * child's own nodes, and the batch is probed in the order of the keys by a descent of the btree that keeps the pages of the key before while the next
 * key stays inside their ranges and compares the columns as bytes where the opclass is a plain byte order (integers, text under the C collation,
 * the empty-array case of arrays). What the index API would do per key (executor node, scan descriptor, generic compare through the
 * opclass, page lock round trips) is the 75 % of such a statement; here the same index pages are read in the same way (a share lock on one page at
 * a time, the right link followed when a page split or was deleted since its downlink was read), the entries of the key are walked in index order,
 * a heap tuple is fetched through the table AM with the statement's snapshot (so versions, HOT chains, dead entries and visibility are the AM's; an Index Only
 * Scan asks the heap only where the visibility map does not say all-visible, with the leaf pinned until it has) and the child's own filter and projections are
 * applied to it. A key the shape or the index does not allow, or a page that does not look as expected,
 * is left to the child: the result is the same rows in the same order.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "executor/nodeIndexscan.h"
#include "nodes/nodeFuncs.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/uuid.h"

#include "agg/lookup/int.h"
#include "agg/combo/probe.h"

/* ExecAllocTableSlot takes the flags of the slot from PostgreSQL 19 */
#if PG_VERSION_NUM >= 190000
#define PG19_SLOT_FLAGS , 0
#else
#define PG19_SLOT_FLAGS
#endif

/* the shared instrumentation of a parallel index scan node is a PostgreSQL 18 field */
#if PG_VERSION_NUM >= 180000
#define DP_SHARED_INFO(p) ((p) != NULL)
#else
#define DP_SHARED_INFO(p) false
#endif

/*
 * The parameters of the child hold the values of the key evaluated last when the batch is probed: a filter or a projection of the child that reads one (or runs a
 * sub-plan, which may) would see the wrong key's value, so such a child runs for each key.
 */
static bool
dp_reads_params(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) || IsA(n, SubPlan) || IsA(n, AlternativeSubPlan))
		return true;
	return expression_tree_walker(n, dp_reads_params, context);
}

static bool
dp_int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

/*
 * Can the child be probed directly? The shape and the index conditions are examined once; false ends it for the combo.
 */
static bool
dp_shape(AggExec *ex, int ci, DProbe *dp)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	QueryDesc  *wq = ex->wchild ? ex->wchild[cb->join] : NULL;
	PlanState  *ps;
	Scan	   *sc;
	ScanDirection dir;
	Relation	irel;
	bool		heapkeyspace,
				allequalimage;
	bool		seen[INDEX_MAX_KEYS] = {false};

	/* the index scan takes the predicate locks of a serializable transaction (a page lock finds a phantom); the probe does not */
	if (IsolationIsSerializable())
		return false;
	if (ex->cmemo && ex->cmemo[cb->join])
		return false;
	if (cs->fruns != NULL || cs->fmap != NULL || j->params == NIL || list_length(j->params) != list_length(cs->args))
		return false;
	dp->estate = wq ? wq->estate : ex->css.ss.ps.state;
	ps = wq ? wq->planstate : list_nth(dp->estate->es_subplanstates, j->subplan - 1);
	dp->child = ps;
	dp->limit = 0;
	if (IsA(ps, LimitState))
	{
		LimitState *ls = (LimitState *) ps;
		Limit	   *lp = (Limit *) ps->plan;
		Const	   *cn;

		if (ls->ps.qual != NULL || ls->limitOffset != NULL || lp->limitOption != LIMIT_OPTION_COUNT || lp->limitCount == NULL ||
			!IsA(lp->limitCount, Const))
			return false;
		cn = (Const *) lp->limitCount;
		if (cn->constisnull || DatumGetInt64(cn->constvalue) != 1)
			return false;
		dp->limit = 1;
		ps = outerPlanState(ps);
		if (ps == NULL)
			return false;
	}
	if (IsA(ps, ResultState))
	{
		dp->res = (ResultState *) ps;
		if (dp->res->ps.qual != NULL || dp->res->ps.ps_ProjInfo == NULL || outerPlanState(ps) == NULL)
			return false;
		dp->p2 = dp->res->ps.ps_ProjInfo;
		dp->ec2 = dp->res->ps.ps_ExprContext;
		ps = outerPlanState(ps);
	}
	if (IsA(ps, IndexScanState))
	{
		IndexScanState *iss = (IndexScanState *) ps;

		sc = &((IndexScan *) ps->plan)->scan;
		dir = ((IndexScan *) ps->plan)->indexorderdir;
		if (iss->iss_NumOrderByKeys != 0 || DP_SHARED_INFO(iss->iss_SharedInfo) || iss->iss_PscanLen != 0)
			return false;
		dp->skeys = iss->iss_ScanKeys;
		dp->nskeys = iss->iss_NumScanKeys;
		dp->rkeys = iss->iss_RuntimeKeys;
		dp->nrkeys = iss->iss_NumRuntimeKeys;
		dp->rctx = iss->iss_RuntimeContext;
		irel = dp->irel = iss->iss_RelationDesc;
		dp->hrel = iss->ss.ss_currentRelation;
		dp->qual = iss->ss.ps.qual;
		dp->p1 = iss->ss.ps.ps_ProjInfo;
		dp->ec1 = iss->ss.ps.ps_ExprContext;
		dp->scan = iss->ss.ss_ScanTupleSlot;
	}
	else if (IsA(ps, IndexOnlyScanState))
	{
		IndexOnlyScanState *ios = (IndexOnlyScanState *) ps;

		sc = &((IndexOnlyScan *) ps->plan)->scan;
		dir = ((IndexOnlyScan *) ps->plan)->indexorderdir;
		if (ios->ioss_NumOrderByKeys != 0 || DP_SHARED_INFO(ios->ioss_SharedInfo) || ios->ioss_PscanLen != 0 || ios->ioss_NameCStringCount != 0)
			return false;
		dp->ioss = true;			/* the recheck of a lossy index is not needed: a btree returns its tuples whole */
		dp->skeys = ios->ioss_ScanKeys;
		dp->nskeys = ios->ioss_NumScanKeys;
		dp->rkeys = ios->ioss_RuntimeKeys;
		dp->nrkeys = ios->ioss_NumRuntimeKeys;
		dp->rctx = ios->ioss_RuntimeContext;
		irel = dp->irel = ios->ioss_RelationDesc;
		dp->hrel = ios->ss.ss_currentRelation;
		dp->qual = ios->ss.ps.qual;
		dp->p1 = ios->ss.ps.ps_ProjInfo;
		dp->ec1 = ios->ss.ps.ps_ExprContext;
		dp->scan = ios->ss.ss_ScanTupleSlot;
	}
	else
		return false;
	if (dp_reads_params((Node *) sc->plan.qual, NULL) || dp_reads_params((Node *) sc->plan.targetlist, NULL) ||
		(dp->res != NULL && (dp_reads_params((Node *) dp->res->ps.plan->targetlist, NULL) || dp_reads_params((Node *) dp->res->ps.plan->qual, NULL))))
		return false;
	if (dir != ForwardScanDirection || dp->nskeys < 1 || dp->nskeys > INDEX_MAX_KEYS)
		return false;
	if (irel == NULL || dp->hrel == NULL || irel->rd_rel->relam != BTREE_AM_OID || irel->rd_index->indisvalid == false || dp->scan == NULL)
		return false;
	/* the visibility map is the heap's, and the slot has the columns of the index tuple */
	if (dp->ioss && (dp->hrel->rd_rel->relam != HEAP_TABLE_AM_OID || dp->scan->tts_tupleDescriptor->natts != RelationGetDescr(irel)->natts))
		return false;
	dp->nk = dp->nskeys;
	dp->nkeyatts = IndexRelationGetNumberOfKeyAttributes(irel);
	if (dp->nk > dp->nkeyatts)
		return false;
	_bt_metaversion(irel, &heapkeyspace, &allequalimage);
	if (!heapkeyspace)
		return false;
	for (int s = 0; s < dp->nk; s++)
	{
		ScanKey		sk = &dp->skeys[s];
		int			i = sk->sk_attno - 1;

		if (sk->sk_strategy != BTEqualStrategyNumber || (sk->sk_flags & ~SK_ISNULL) != 0 || i < 0 || i >= dp->nk || seen[i] ||
			(sk->sk_subtype != irel->rd_opcintype[i] && !(dp_int_type(sk->sk_subtype) && dp_int_type(irel->rd_opcintype[i]))))
			return false;
		seen[i] = true;
		dp->keyat[i] = s;
	}
	for (int i = 0; i < dp->nk; i++)
	{
		DCol	   *c = &dp->col[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(irel), i);
		int16		opt = irel->rd_indoption[i];

		c->len = att->attlen;
		c->byval = att->attbyval;
		c->align = att->attalign;
		c->alignb = att->attalign == TYPALIGN_CHAR ? 1 : att->attalign == TYPALIGN_SHORT ? 2 : att->attalign == TYPALIGN_INT ? 4 : 8;
		c->desc = (opt & INDOPTION_DESC) != 0;
		c->nullsfirst = (opt & INDOPTION_NULLS_FIRST) != 0;
		c->coll = irel->rd_indcollation[i];
		c->cmp = index_getprocinfo(irel, i + 1, BTORDER_PROC);
		c->kind = DC_GENERIC;
		switch (c->cmp->fn_oid)
		{
			case F_BTINT2CMP:
				c->kind = c->len == 2 && c->byval ? DC_INT : DC_GENERIC;
				break;
			case F_BTINT4CMP:
				c->kind = c->len == 4 && c->byval ? DC_INT : DC_GENERIC;
				break;
			case F_BTINT8CMP:
				c->kind = c->len == 8 && c->byval ? DC_INT : DC_GENERIC;
				break;
			case F_DATE_CMP:
				c->kind = c->len == 4 && c->byval ? DC_INT : DC_GENERIC;
				break;
			case F_TIMESTAMP_CMP:
			case F_TIMESTAMPTZ_CMP:
				c->kind = c->len == 8 && c->byval ? DC_INT : DC_GENERIC;
				break;
			case F_UUID_CMP:
				c->kind = c->len == UUID_LEN && !c->byval ? DC_UUID : DC_GENERIC;
				break;
			case F_BTTEXTCMP:
				c->kind = c->len != -1 ? DC_GENERIC : OidIsValid(c->coll) && sift_collate_is_c(c->coll) ? DC_TEXT : DC_TEXTC;
				break;
			case F_BTTEXT_PATTERN_CMP:
				c->kind = c->len == -1 ? DC_TEXT : DC_GENERIC;
				break;
			case F_BTARRAYCMP:
				c->kind = c->len == -1 ? DC_ARRAY : DC_GENERIC;
				break;
			default:
				break;
		}
		/* an integer column compared with the key of another integer type (a smallint column and a literal): the integers themselves are compared (dp_key) */
		if (dp->skeys[dp->keyat[i]].sk_subtype != irel->rd_opcintype[i] && (c->kind != DC_INT || irel->rd_opfamily[i] != INTEGER_BTREE_FAM_OID))
			return false;
	}
	dp->snap = dp->estate->es_snapshot;
	if (dp->snap == NULL || dp->ec1 == NULL || (dp->nrkeys > 0 && dp->rctx == NULL))
		return false;
	return true;
}

/* the combo's join: is a correlated child probed directly? Examines the shape on the first call */
bool
combo_probe_on(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	DProbe	   *dp = cs->dp;

	if (cb->join < 0 || ex->spec->join[cb->join].params == NIL)
		return false;
	if (dp == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		dp = cs->dp = palloc0(sizeof(DProbe));
		dp->state = sift_agg_probe_direct && dp_shape(ex, ci, dp) ? 1 : -1;
		if (dp->state > 0)
		{
			map_note_table(ex, dp->hrel);
			dp->bcxt = AllocSetContextCreate(ex->cxt, "sift direct probe", ALLOCSET_DEFAULT_SIZES);
			if (dp->ioss)
				dp->tslot = ExecAllocTableSlot(&dp->estate->es_tupleTable, RelationGetDescr(dp->hrel), table_slot_callbacks(dp->hrel) PG19_SLOT_FLAGS);
			dp->ignore_dead = !TransactionStartedDuringRecovery();
			dp->reads0 = dp_reads(dp->hrel);
			for (int i = 0; i < DP_MAX_LEVELS; i++)
				dp->lv[i].hik = palloc(BLCKSZ / 2);
		}
		MemoryContextSwitchTo(old);
	}
	return dp->state > 0;
}

/* the entry e of the combo was made on its first sight: its child is probed with the rest of the batch (combo_probe_flush) */
void
combo_probe_defer(AggExec *ex, int ci, uint32 e)
{
	DProbe	   *dp = ex->cs[ci].dp;

	if (dp->npend == DP_MAX_PEND)
		combo_probe_flush(ex, ci);
	if (dp->npend == dp->cap)
	{
		MemoryContext old = MemoryContextSwitchTo(dp->bcxt);

		dp->cap = Max(dp->cap * 2, 256);
		dp->pend = dp->pend ? repalloc(dp->pend, dp->cap * sizeof(uint32)) : palloc(dp->cap * sizeof(uint32));
		dp->keys = dp->keys ? repalloc(dp->keys, dp->cap * sizeof(DKey)) : palloc(dp->cap * sizeof(DKey));
		MemoryContextSwitchTo(old);
	}
	dp->pend[dp->npend++] = e;
}

/* the result of the key an entry waits for, when the batch gave it (combo_child_rows) */
bool
combo_probe_take(AggExec *ex, int ci, uint32 *start, int *n)
{
	DProbe	   *dp = ex->cs[ci].dp;

	if (dp == NULL || dp->state <= 0 || ex->cs[ci].dkey == NULL || ex->cs[ci].dkey->state != DKS_ROWS)
		return false;
	*start = ex->cs[ci].dkey->start;
	*n = ex->cs[ci].dkey->n;
	return true;
}

/* the key of pending entry p: the child's own nodes evaluate the one-time filter and the index conditions over the entry's parameters */
static void
dp_key(AggExec *ex, int ci, DProbe *dp, DKey *k, uint32 e)
{
	ComboSt    *cs = &ex->cs[ci];
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ListCell   *lp,
			   *la;

	k->state = DKS_TODO;
	k->n = 0;
	k->start = 0;
	forboth(lp, j->params, la, cs->args)
	{
		ParamExecData *prm = &dp->estate->es_param_exec_vals[lfirst_int(lp)];

		prm->value = ExecEvalExprSwitchContext(lfirst(la), ex->lutcx, &prm->isnull);
		Assert(prm->execPlan == NULL);
	}
	if (dp->res != NULL && dp->res->resconstantqual != NULL)
	{
		ResetExprContext(dp->ec2);
		if (!ExecQual(dp->res->resconstantqual, dp->ec2))
		{
			k->state = DKS_ROWS;
			return;
		}
	}
	if (dp->nrkeys > 0)
	{
		ResetExprContext(dp->rctx);
		ExecIndexEvalRuntimeKeys(dp->rctx, dp->rkeys, dp->nrkeys);
	}
	k->a = MemoryContextAlloc(dp->bcxt, dp->nk * sizeof(DArg));
	for (int i = 0; i < dp->nk; i++)
	{
		ScanKey		sk = &dp->skeys[dp->keyat[i]];
		const DCol *c = &dp->col[i];
		DArg	   *a = &k->a[i];
		MemoryContext old;

		if (sk->sk_flags & SK_ISNULL)
		{
			k->state = DKS_ROWS;	/* an equality with NULL is never true */
			return;
		}
		old = MemoryContextSwitchTo(dp->bcxt);
		if (c->len == -1)
		{
			struct varlena *v = (struct varlena *) PG_DETOAST_DATUM_COPY(sk->sk_argument);

			a->d = PointerGetDatum(v);
			a->p = VARDATA_ANY(v);
			a->n = VARSIZE_ANY_EXHDR(v);
		}
		else
		{
			a->d = datumCopy(sk->sk_argument, c->byval, c->len);
			if (c->kind == DC_INT)
			{
				/* a key of another integer type is read at its own width (the column's is the width of date, timestamp and the integer types alike) */
				int			w = sk->sk_subtype == INT2OID ? 2 : sk->sk_subtype == INT4OID ? 4 : sk->sk_subtype == INT8OID ? 8 : c->len;

				a->i = w == 2 ? DatumGetInt16(a->d) : w == 4 ? DatumGetInt32(a->d) : DatumGetInt64(a->d);
			}
			else if (c->kind == DC_UUID)
				a->p = DatumGetPointer(a->d);
		}
		MemoryContextSwitchTo(old);
		if (c->kind == DC_INT && c->len < 8 && (a->i < -(INT64CONST(1) << (c->len * 8 - 1)) || a->i >= (INT64CONST(1) << (c->len * 8 - 1))))
		{
			k->state = DKS_ROWS;	/* the key is beyond the range of the column: no entry equals it */
			return;
		}
	}
	dp_abbrev(dp, k);
}

/* the sink of a combo: the rows of a key go to the combo's map, after the child's own filter and projections */
typedef struct ComboSink
{
	DSink		s;
	AggExec    *ex;
	int			ci;
	DProbe	   *dp;
} ComboSink;

static void
combo_sink_begin(DSink *s, DKey *k)
{
	ComboSink  *cs = (ComboSink *) s;

	k->start = cs->ex->cs[cs->ci].map->nrow;
}

/* an index-only entry the child's filter rejects needs no visibility check */
static bool
combo_sink_keep(DSink *s)
{
	DProbe	   *dp = ((ComboSink *) s)->dp;

	ResetExprContext(dp->ec1);
	dp->ec1->ecxt_scantuple = dp->scan;
	return ExecQual(dp->qual, dp->ec1);
}

static bool
combo_sink_row(DSink *s)
{
	ComboSink  *sk = (ComboSink *) s;
	AggExec    *ex = sk->ex;
	DProbe	   *dp = sk->dp;
	ComboSt    *cs = &ex->cs[sk->ci];
	const SiftAggCombo *cb = &ex->spec->combo[sk->ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	TupleTableSlot *slot = dp->scan;
	MemoryContext old;

	if (!dp->ioss)
	{
		ResetExprContext(dp->ec1);
		dp->ec1->ecxt_scantuple = dp->scan;
		if (!ExecQual(dp->qual, dp->ec1))
			return false;
	}
	if (dp->p1)
		slot = ExecProject(dp->p1);
	if (dp->p2)
	{
		ResetExprContext(dp->ec2);
		dp->ec2->ecxt_outertuple = slot;
		slot = ExecProject(dp->p2);
	}
	old = MemoryContextSwitchTo(ex->cxt);
	map_row(ex, cs->map, slot, j, cb, NULL);
	MemoryContextSwitchTo(old);
	return true;
}

void
combo_probe_flush(AggExec *ex, int ci)
{
	ComboSt    *cs = &ex->cs[ci];
	DProbe	   *dp = cs->dp;
	int			n = dp->npend;
	DKey	  **ord;
	instr_time	t0;
	int			nprobe = 0;

	if (n == 0)
		return;
	INSTR_TIME_SET_CURRENT(t0);
	/* the keys, in the order the entries were made */
	for (int p = 0; p < n; p++)
	{
		uint32		e = dp->pend[p];
		MemoryContext old = MemoryContextSwitchTo(ex->comcxt);

		combo_bind_leaves(ex, ci, &cs->vid[(size_t) e * cs->ninputs]);
		dp->keys[p].pend = p;
		dp_key(ex, ci, dp, &dp->keys[p], e);
		MemoryContextSwitchTo(old);
		ResetExprContext(ex->lutcx);
		MemoryContextReset(ex->comcxt);
	}
	ord = MemoryContextAlloc(dp->bcxt, n * sizeof(DKey *));
	for (int p = 0; p < n; p++)
		if (dp->keys[p].state == DKS_TODO)
			ord[nprobe++] = &dp->keys[p];
	if (nprobe > 0)
	{
		ComboSink	sink = {{combo_sink_begin, combo_sink_keep, combo_sink_row}, ex, ci, dp};

		dp->fetch = sift_table_index_fetch_begin(dp->hrel);
		ex->probe_direct += dp_batch(dp, ord, nprobe, &sink.s);
		ExecClearTuple(dp->scan);
		if (dp->tslot != NULL)
			ExecClearTuple(dp->tslot);
		/* the fetch keeps the pins of its last heap page (and, from PostgreSQL 19, of its visibility map page) until it ends; none is held between batches */
		table_index_fetch_end(dp->fetch);
		dp->fetch = NULL;
	}
	ex->child_warmed += dp->asked;
	dp->asked = 0;
	ex->probe_leaves += dp->leaves;
	ex->probe_pins += dp->pins;
	ex->probe_heap += dp->heap;
	ex->probe_vm += dp->vm;
	dp->leaves = dp->pins = dp->heap = dp->vm = 0;
	map_loaded(ex, 0, t0);
	for (int p = 0; p < n; p++)
	{
		uint32		e = dp->pend[p];

		cs->dkey = &dp->keys[p];
		combo_make(ex, ci, &cs->vid[(size_t) e * cs->ninputs], &cs->ent[e]);
	}
	cs->dkey = NULL;
	dp->npend = 0;
	dp->cap = 0;
	dp->pend = NULL;
	dp->keys = NULL;
	dp->tids = NULL;
	dp->tup = NULL;
	dp->captids = 0;
	dp->abuf = NULL;
	dp->alen = dp->acap = 0;
	MemoryContextReset(dp->bcxt);
}
