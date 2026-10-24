/*
 * agg/gen.c - generic aggregates (SAA_GEN): state, arguments, advance, combine, final, spill
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
#include "col/hll.h"

static Datum
gen_eval_once(AggExec *ex, ExprState *e, Oid typid, bool *isnull)
{
	int16		len;
	bool		byval;
	Datum		d = ExecEvalExprSwitchContext(e, ex->lutcx, isnull);

	get_typlenbyval(typid, &len, &byval);
	d = *isnull ? (Datum) 0 : datumCopy(d, byval, len);
	ResetExprContext(ex->lutcx);
	return d;
}

/*
 * ORDER BY or DISTINCT in the call: a tuplesort of (group number, the arguments that vary) on the group number and the sort keys,
 * as nodeAgg sorts the input of such an aggregate (sort operator, input collation and NULLS FIRST/LAST of each key; DISTINCT sorts on
 * its own list, which starts with the ORDER BY keys); work_mem bounded. DISTINCT then drops an input equal to the one before it.
 */
void
gen_sort_begin(GenSt *st)
{
	const SiftAggGen *g = st->g;
	Aggref	   *ref = st->aggref;
	List	   *sl = g->ordered ? NIL : ref->aggdistinct != NIL ? ref->aggdistinct : ref->aggorder;	/* WITHIN GROUP: the transition function sorts */
	int			nv = 0,
				nk = 1;
	AttrNumber	att[SIFT_AGG_MAX_GARGS + 1] = {1};
	Oid			ops[SIFT_AGG_MAX_GARGS + 1] = {Int4LessOperator},
				coll[SIFT_AGG_MAX_GARGS + 1] = {InvalidOid};
	bool		nf[SIFT_AGG_MAX_GARGS + 1] = {false};
	TupleDesc	desc;

	for (int k = 0; k < g->nargs; k++)
		st->vcol[k] = g->arg[k].nleaves > 0 || g->arg[k].mapped ? nv++ : -1;
	desc = CreateTemplateTupleDesc(1 + nv);
	TupleDescInitEntry(desc, 1, "gid", INT4OID, -1, 0);
	for (int k = 0; k < g->nargs; k++)
		if (st->vcol[k] >= 0)
			TupleDescInitEntry(desc, 2 + st->vcol[k], "a", g->arg[k].typid, -1, 0);
	foreach_node(SortGroupClause, sc, sl)
	{
		int			k = 0;
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);

		while (list_nth(ref->args, k) != te)
			k++;
		if (st->vcol[k] < 0)
			continue;
		att[nk] = 2 + st->vcol[k];
		ops[nk] = sc->sortop;
		coll[nk] = exprCollation((Node *) te->expr);
		nf[nk] = sc->nulls_first;
		nk++;
		if (g->distinct)
		{
			fmgr_info(get_opcode(sc->eqop), &st->deq[st->ndcol]);
			get_typlenbyval(g->arg[k].typid, &st->dlen[st->ndcol], &st->dbyval[st->ndcol]);
			st->dcol[st->ndcol++] = st->vcol[k];
		}
	}
	sift_tupdesc_finalize(desc);
	st->pcx = AllocSetContextCreate(CurrentMemoryContext, "sift agg distinct", ALLOCSET_SMALL_SIZES);
	st->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	st->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	st->sort = tuplesort_begin_heap(desc, nk, att, ops, coll, nf, work_mem, NULL, TUPLESORT_NONE);
	st->tmode = true;
}

/* an input of the sort path: its group's number (given when the group's first input arrives) and the arguments that vary */
void
gen_sort_put(GenSt *st, AggAcc *acc, uint32 gid, const Datum *args, const bool *nulls)
{
	if (acc->i[gid] == 0)
		acc->i[gid] = ++st->nsid;
	ExecClearTuple(st->put);
	st->put->tts_values[0] = Int32GetDatum((int32) (acc->i[gid] - 1));
	st->put->tts_isnull[0] = false;
	for (int k = 0; k < st->g->nargs; k++)
		if (st->vcol[k] >= 0)
		{
			st->put->tts_values[1 + st->vcol[k]] = args[k];
			st->put->tts_isnull[1 + st->vcol[k]] = nulls[k];
		}
	ExecStoreVirtualTuple(st->put);
	tuplesort_puttupleslot(st->sort, st->put);
}

/* DISTINCT ON: the sort support of each ORDER BY key of the call (operator, input collation and NULLS FIRST/LAST, as the sort of nodeAgg has them) */
static void
gen_first_begin(GenSt *st)
{
	const SiftAggGen *g = st->g;
	Aggref	   *ref = st->aggref;

	st->fsup = palloc0(Max(list_length(ref->aggorder), 1) * sizeof(SortSupportData));
	foreach_node(SortGroupClause, sc, ref->aggorder)
	{
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);
		SortSupport ssup = &st->fsup[st->nfk];
		int			k = 0;

		while (list_nth(ref->args, k) != te)
			k++;
		ssup->ssup_cxt = CurrentMemoryContext;
		ssup->ssup_collation = exprCollation((Node *) te->expr);
		ssup->ssup_nulls_first = sc->nulls_first;
		PrepareSortSupportFromOrderingOp(sc->sortop, ssup);
		st->fkey[st->nfk++] = k;
		st->iskey[k] = true;
	}
	for (int k = 0; k < g->nargs; k++)
		get_typlenbyval(g->arg[k].typid, &st->alen[k], &st->abyval[k]);
}

static GenSt *
gen_new(AggExec *ex, const SiftAggAgg *a, int ai)
{
	GenSt	   *st = palloc0(sizeof(GenSt));
	const SiftAggGen *g = a->gen;
	Aggref	   *ref = (Aggref *) g->aggref;
	HeapTuple	tup,
				proc;
	Form_pg_aggregate ag;
	Oid			owner,
				inputTypes[FUNC_MAX_ARGS];
	int			numArguments = get_aggregate_argtypes(ref, inputTypes),
				ntrans = 1 + g->ntrans;
	Expr	   *transfnexpr,
			   *finalfnexpr;
	AclResult	ar;
	int			k = 0;

	st->g = g;
	st->ai = ai;
	st->aggref = ref;
	ar = object_aclcheck(ProcedureRelationId, ref->aggfnoid, GetUserId(), ACL_EXECUTE);
	if (ar != ACLCHECK_OK)
		aclcheck_error(ar, OBJECT_AGGREGATE, get_func_name(ref->aggfnoid));
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	proc = SearchSysCache1(PROCOID, ObjectIdGetDatum(ref->aggfnoid));
	SIFT_INVARIANT(HeapTupleIsValid(tup) && HeapTupleIsValid(proc), "cache lookup failed for aggregate %u", ref->aggfnoid);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	owner = ((Form_pg_proc) GETSTRUCT(proc))->proowner;
	ReleaseSysCache(proc);
	foreach_oid(fn, list_make2_oid(ag->aggtransfn, ag->aggfinalfn))
	{
		if (!OidIsValid(fn))
			continue;
		ar = object_aclcheck(ProcedureRelationId, fn, owner, ACL_EXECUTE);
		if (ar != ACLCHECK_OK)
			aclcheck_error(ar, OBJECT_FUNCTION, get_func_name(fn));
	}
	st->transtype = resolve_aggregate_transtype(ref->aggfnoid, ag->aggtranstype, inputTypes, numArguments);
	get_typlenbyval(st->transtype, &st->tlen, &st->tbyval);
	get_typlenbyval(ref->aggtype, &st->rlen, &st->rbyval);
	build_aggregate_transfn_expr(inputTypes, numArguments, list_length(ref->aggdirectargs), ref->aggvariadic, st->transtype,
								 ref->inputcollid, ag->aggtransfn, InvalidOid, &transfnexpr, NULL);
	fmgr_info(ag->aggtransfn, &st->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &st->transfn);
	st->strict = st->transfn.fn_strict;
	st->tfc = palloc0(SizeForFunctionCallInfo(ntrans));
	InitFunctionCallInfoData(*st->tfc, &st->transfn, ntrans, ref->inputcollid, (Node *) ex->gas, NULL);
	st->ndirect = list_length(ref->aggdirectargs);
	if (OidIsValid(ag->aggfinalfn))
	{
		st->has_final = true;
		st->nfinal = ag->aggfinalextra ? numArguments + 1 : st->ndirect + 1;
		build_aggregate_finalfn_expr(inputTypes, st->nfinal, st->transtype, ref->aggtype, ref->inputcollid, ag->aggfinalfn,
									 &finalfnexpr);
		fmgr_info(ag->aggfinalfn, &st->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &st->finalfn);
		st->ffc = palloc0(SizeForFunctionCallInfo(st->nfinal));
		InitFunctionCallInfoData(*st->ffc, &st->finalfn, st->nfinal, ref->inputcollid, (Node *) ex->gas, NULL);
	}
	if (g->parallel)
	{
		Expr	   *e;

		fmgr_info(ag->aggcombinefn, &st->combinefn);
		st->cfc = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*st->cfc, &st->combinefn, 2, ref->inputcollid, (Node *) ex->gas, NULL);
		if (OidIsValid(ag->aggserialfn))
		{
			build_aggregate_serialfn_expr(ag->aggserialfn, &e);
			fmgr_info(ag->aggserialfn, &st->serialfn);
			fmgr_info_set_expr((Node *) e, &st->serialfn);
			st->sfc = palloc0(SizeForFunctionCallInfo(1));
			InitFunctionCallInfoData(*st->sfc, &st->serialfn, 1, InvalidOid, (Node *) ex->gas, NULL);
			build_aggregate_deserialfn_expr(ag->aggdeserialfn, &e);
			fmgr_info(ag->aggdeserialfn, &st->deserialfn);
			fmgr_info_set_expr((Node *) e, &st->deserialfn);
			st->dfc = palloc0(SizeForFunctionCallInfo(2));
			InitFunctionCallInfoData(*st->dfc, &st->deserialfn, 2, InvalidOid, (Node *) ex->gas, NULL);
		}
	}
	{
		Datum		txt = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &st->initnull);

		if (!st->initnull)
		{
			Oid			infn,
						ioparam;

			getTypeInputInfo(st->transtype, &infn, &ioparam);
			st->initval = OidInputFunctionCall(infn, TextDatumGetCString(txt), ioparam, -1);
		}
	}
	ReleaseSysCache(tup);
	foreach_ptr(Node, d, ref->aggdirectargs)
	{
		st->dir[k] = gen_eval_once(ex, ExecInitExpr((Expr *) d, EXPR_PARENT(ex)), exprType(d), &st->dirnull[k]);
		k++;
	}
	for (k = 0; k < g->nargs; k++)
	{
		const SiftAggGenArg *ar2 = &g->arg[k];

		if (ar2->nleaves == 0 && !ar2->mapped)
			st->argk[k] = gen_eval_once(ex, ExecInitExpr((Expr *) ar2->expr, EXPR_PARENT(ex)), ar2->typid, &st->argknull[k]);
		else if (ar2->expr)
			st->argx[k] = ExecInitExpr((Expr *) ar2->expr, EXPR_PARENT(ex));
	}
	st->cx = CreateStandaloneExprContext();
	st->pertrans.aggref = ref;
	st->pertrans.aggshared = false;
	st->peragg.aggref = ref;
	st->peragg.transno = 0;
	if (g->distinct)
	{
		TypeCacheEntry *tc = lookup_type_cache(g->arg[g->darg].typid, TYPECACHE_CMP_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO);

		st->cmp = &tc->cmp_proc_finfo;
		st->eq = &tc->eq_opr_finfo;
	}
	if (g->sorted || g->ordered)
		gen_sort_begin(st);
	if (g->first)
	{
		st->has_final = false;
		gen_first_begin(st);
	}
	return st;
}

/* the generic aggregates of the spec, ready to run (the scan's context, after the expression context exists) */
void
gen_open(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	int			leader = -1;

	ex->gst = palloc0(Max(s->naggs, 1) * sizeof(GenSt *));
	for (int ai = 0; ai < s->naggs; ai++)
		if (s->agg[ai].kind == SAA_GEN)
		{
			if (ex->gas == NULL)
			{
				ex->gas = makeNode(AggState);
				ex->gas->tmpcontext = ex->lutcx;
				ex->gres = AllocSetContextCreate(ex->cxt, "sift agg results", ALLOCSET_DEFAULT_SIZES);
			}
			ex->gst[ai] = gen_new(ex, &s->agg[ai], ai);
			ex->gst[ai]->follower = s->agg[ai].gen->first && leader >= 0;
			if (s->agg[ai].gen->first && leader < 0)
				leader = ai;
		}
	MemoryContextSwitchTo(old);
}

void
gen_close(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai])
		{
			if (ex->gst[ai]->sort)
				tuplesort_end(ex->gst[ai]->sort);
			FreeExprContext(ex->gst[ai]->cx, true);
		}
}

/* a leaf value as a Datum of the leaf's type, in the current memory context */
static Datum
gen_leaf_value(const SiftSrcLeaf *l, const SiftSrcVec *v, uint32 i, bool *isnull)
{
	*isnull = v->isnull && v->isnull[i];
	if (*isnull)
		return (Datum) 0;
	switch (l->vtype)
	{
		case SCV_I32:
			return leaf_datum(l->typid, ((const int32 *) v->data)[i]);
		case SCV_I64:
			if (l->typid == NUMERICOID && v->ovf && sift_colsrc_bit(v->ovf, i))
				return scaled_numeric(SIFT_NX_NAN, 0);
			if (l->typid == NUMERICOID)
				return NumericGetDatum(int64_div_fast_to_numeric(((const int64 *) v->data)[i], l->scale));
			return Int64GetDatum(((const int64 *) v->data)[i]);
		case SCV_F64:
			return l->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[i]) :
				Float8GetDatum(((const double *) v->data)[i]);
		case SCV_CODE:
			{
				uint32		code = ((const uint32 *) v->data)[i];

				if (l->wide)
					return sift_aggwide_datum(l->typid, v->dict->bytes + v->dict->off[code], v->dict->off[code + 1] - v->dict->off[code]);
				return PointerGetDatum(cstring_to_text_with_len(v->dict->bytes + v->dict->off[code],
																v->dict->off[code + 1] - v->dict->off[code]));
			}
	}
	return (Datum) 0;
}

/* argument k of tuple j */
void
gen_arg(AggExec *ex, GenSt *st, const GenIn *in, int j, int k, Datum *arg, bool *isnull)
{
	const SiftAggGenArg *ar = &st->g->arg[k];

	if (ar->mapped)
	{
		uint32		id = in->cval[k][(size_t) in->cout[k][j] * ex->spec->combo[ar->combo].nslots + ar->slot];

		*isnull = id == 0;
		*arg = id ? ex->aot[st->ai].val[id - 1] : (Datum) 0;
	}
	else if (ar->nleaves == 0)
	{
		*arg = st->argk[k];
		*isnull = st->argknull[k];
	}
	else if (ar->expr == NULL)
		*arg = gen_leaf_value(&ex->lay->leaf[ar->leaf[0]], in->vec[k][0], in->idx[k][0][j], isnull);
	else
	{
		for (int p = 0; p < ar->nleaves; p++)
		{
			bool		pnull;
			Datum		d = gen_leaf_value(&ex->lay->leaf[ar->leaf[p]], in->vec[k][p], in->idx[k][p][j], &pnull);

			bind_param(ex, p, d, pnull);
		}
		*arg = ExecEvalExprSwitchContext(st->argx[k], ex->lutcx, isnull);
	}
}

/* initialize_aggregate */
static void
gen_state_init(GenSt *st, AggAcc *acc, uint32 g)
{
	acc->nf[g] = GEN_INIT;
	if (st->initnull)
		acc->nf[g] |= GEN_NULL | (st->strict ? GEN_NOVAL : 0);
	else
	{
		MemoryContext old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);

		acc->v[g] = datumCopy(st->initval, st->tbyval, st->tlen);
		MemoryContextSwitchTo(old);
	}
}

/* ExecAggCopyTransValue: a new pass-by-reference state is copied into the aggregate's memory, the old one freed */
static void
gen_keep_state(GenSt *st, AggAcc *acc, uint32 g, uint8 f, Datum ov, Datum nv, bool nvnull)
{
	MemoryContext old;

	if (!st->tbyval && DatumGetPointer(nv) != DatumGetPointer(ov))
	{
		if (!nvnull)
		{
			old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
			if (!(DatumIsReadWriteExpandedObject(nv, false, st->tlen) &&
				  MemoryContextGetParent(DatumGetEOHP(nv)->eoh_context) == CurrentMemoryContext))
				nv = datumCopy(nv, st->tbyval, st->tlen);
			MemoryContextSwitchTo(old);
		}
		if (!(f & GEN_NULL))
		{
			if (DatumIsReadWriteExpandedObject(ov, false, st->tlen))
				DeleteExpandedObject(ov);
			else
				pfree(DatumGetPointer(ov));
		}
	}
	acc->v[g] = nv;
	acc->nf[g] = (f & ~(GEN_NULL | GEN_NOVAL)) | (nvnull ? GEN_NULL : 0);
}

/* advance_transition_function: the transition function on one input; its temporary memory is the scan's per-tuple memory */
void
gen_advance(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const Datum *args, const bool *nulls)
{
	FunctionCallInfo fc = st->tfc;
	int			nargs = st->g->ntrans;
	uint8		f;
	Datum		nv,
				ov;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	if (st->strict)
	{
		for (int k = 0; k < nargs; k++)
			if (nulls[k])
				return;
		if (f & GEN_NOVAL)
		{
			old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
			acc->v[g] = datumCopy(args[0], st->tbyval, st->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = f & ~(GEN_NOVAL | GEN_NULL);
			return;
		}
		if (f & GEN_NULL)
			return;
	}
	ov = acc->v[g];
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & GEN_NULL) != 0;
	for (int k = 0; k < nargs; k++)
	{
		fc->args[k + 1].value = args[k];
		fc->args[k + 1].isnull = nulls[k];
	}
	fc->isnull = false;
	ex->gas->curpertrans = &st->pertrans;
	ex->gas->curperagg = NULL;
	ex->gas->curaggcontext = st->cx;
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	nv = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	gen_keep_state(st, acc, g, f, ov, nv, fc->isnull);
}

/* advance_combine_function: the partial state nv of a worker joins the state of group g */
static void
gen_combine(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, Datum nv)
{
	FunctionCallInfo fc = st->cfc;
	uint8		f;
	Datum		ov,
				res;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	if (st->combinefn.fn_strict && (f & GEN_NULL))
	{
		old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
		acc->v[g] = datumCopy(nv, st->tbyval, st->tlen);
		MemoryContextSwitchTo(old);
		acc->nf[g] = f & ~(GEN_NULL | GEN_NOVAL);
		return;
	}
	ov = acc->v[g];
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & GEN_NULL) != 0;
	fc->args[1].value = nv;
	fc->args[1].isnull = false;
	fc->isnull = false;
	ex->gas->curpertrans = &st->pertrans;
	ex->gas->curperagg = NULL;
	ex->gas->curaggcontext = st->cx;
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	res = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	gen_keep_state(st, acc, g, f, ov, res, fc->isnull);
}

/* a unit's stored sketch joins the state of sift.approx_count_distinct (group 0) through its combine function, as a worker's partial state would */
void
gen_merge_hll(AggExec *ex, int ai, const uint8 *blob)
{
	SiftHll    *h = MemoryContextAlloc(ex->ucxt, sizeof(SiftHll));

	sift_hll_init(h, ex->ucxt);
	sift_hll_merge_blob(h, blob);
	gen_combine(ex, ex->gst[ai], &ex->acc[ai], 0, PointerGetDatum(h));
}

/* a worker's partial state of group g, appended to vb as a length (0: none) and bytes: the serial function's, else the datum's */
void
gen_state_put(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, StringInfo vb)
{
	uint8		f = acc->nf[g];
	uint32		len = 0;

	if (st->g->first)
	{
		gen_first_put(st, acc, g, vb);
		return;
	}
	if (!(f & GEN_INIT) || (f & GEN_NULL))
	{
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		return;
	}
	if (st->sfc)
	{
		bytea	   *r;

		st->sfc->args[0].value = acc->v[g];
		st->sfc->args[0].isnull = false;
		st->sfc->isnull = false;
		ex->gas->curpertrans = &st->pertrans;
		ex->gas->curperagg = NULL;
		ex->gas->curaggcontext = st->cx;
		r = DatumGetByteaPP(FunctionCallInvoke(st->sfc));
		len = VARSIZE_ANY(r);
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		appendBinaryStringInfo(vb, (char *) r, len);
	}
	else
	{
		Size		sz = datumEstimateSpace(acc->v[g], false, st->tbyval, st->tlen);
		char	   *at;

		len = sz;
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		enlargeStringInfo(vb, sz);
		at = vb->data + vb->len;
		datumSerialize(acc->v[g], false, st->tbyval, st->tlen, &at);
		vb->len += sz;
	}
}

/* the next partial state of the blob (the layout of gen_state_put) joins group g */
void
gen_state_merge(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const char **cur)
{
	uint32		len;
	Datum		nv;
	MemoryContext old;

	memcpy(&len, *cur, sizeof(len));
	*cur += sizeof(len);
	if (len == 0)
		return;
	if (st->g->first)
	{
		old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
		gen_first_merge(st, acc, g, *cur);
		MemoryContextSwitchTo(old);
		*cur += len;
		ResetExprContext(ex->lutcx);
		return;
	}
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	if (st->dfc)
	{
		bytea	   *r = palloc(len);

		memcpy(r, *cur, len);
		st->dfc->args[0].value = PointerGetDatum(r);
		st->dfc->args[0].isnull = false;
		st->dfc->args[1].value = (Datum) 0;
		st->dfc->args[1].isnull = false;
		st->dfc->isnull = false;
		ex->gas->curpertrans = &st->pertrans;
		ex->gas->curperagg = NULL;
		ex->gas->curaggcontext = st->cx;
		nv = FunctionCallInvoke(st->dfc);
	}
	else
	{
		const char *at = *cur;
		bool		isnull;

		nv = datumRestore((char **) &at, &isnull);
	}
	*cur += len;
	MemoryContextSwitchTo(old);
	gen_combine(ex, st, acc, g, nv);
	ResetExprContext(ex->lutcx);
}

/* finalize_aggregate, once per group; the result lives as long as the scan */
void
gen_final(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g)
{
	uint8		f;
	Datum		res;
	bool		rnull;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	old = MemoryContextSwitchTo(ex->gres);
	if (!st->has_final)
	{
		res = acc->v[g];
		rnull = (f & GEN_NULL) != 0;
	}
	else
	{
		FunctionCallInfo fc = st->ffc;
		bool		anynull = (f & GEN_NULL) != 0;

		ex->gas->curperagg = &st->peragg;
		ex->gas->curpertrans = NULL;
		ex->gas->curaggcontext = st->cx;
		fc->args[0].value = MakeExpandedObjectReadOnly(acc->v[g], anynull, st->tlen);
		fc->args[0].isnull = anynull;
		for (int i = 1; i < st->nfinal; i++)
		{
			fc->args[i].value = i <= st->ndirect ? st->dir[i - 1] : (Datum) 0;
			fc->args[i].isnull = i <= st->ndirect ? st->dirnull[i - 1] : true;
			anynull |= fc->args[i].isnull;
		}
		if (st->finalfn.fn_strict && anynull)
		{
			res = (Datum) 0;
			rnull = true;
		}
		else
		{
			fc->isnull = false;
			res = FunctionCallInvoke(fc);
			rnull = fc->isnull;
		}
	}
	if (!rnull && !st->rbyval)
		res = datumCopy(res, st->rbyval, st->rlen);
	MemoryContextSwitchTo(old);
	acc->r[g] = res;
	acc->nf[g] = f | GEN_DONE | (rnull ? GEN_RNULL : 0);
}
