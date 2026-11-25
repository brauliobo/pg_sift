/*
 * agg/v6/v6_gen.c - SV6_GEN_PG: the states, arguments, transition, combine and final of the generic aggregates in the groups of SiftAggV6 (v6_gen.h)
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "optimizer/plancat.h"
#include "parser/parse_agg.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/compat.h"
#include "agg/group/ht.h"
#include "agg/v6/group_nan.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_gen.h"
#include "agg/v6/v6_gen_sort.h"
#include "agg/v6/v6_osa.h"

/* ---- the argument values ---- */

static Datum
nan_numeric(void)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static Datum
int_vec_datum(const V6GenArg *a, int scale, const SiftVec *v, uint16 r)
{
	const int64 x = v->type == SVEC_I32 ? ((const int32 *) v->data)[r] : v->type == SVEC_BOOL ? ((const uint8 *) v->data)[r] : ((const int64 *) v->data)[r];

	if (a->typid == NUMERICOID)
		return sift_v6_scaled_numeric(sift_i128_of_i64(x), scale);
	return v6g_int_datum(a->typid, x);
}

/* the value of row r of a vector as a Datum of the argument's type, in the current memory context */
static Datum
vec_datum(const V6GenArg *a, int scale, const SiftVec *v, uint16 r, bool *isnull)
{
	*isnull = !is_valid(v, r);
	if (*isnull)
	{
		*isnull = !v6acc_nan_row(v, r);
		return *isnull ? (Datum) 0 : nan_numeric();
	}
	switch (v->type)
	{
		case SVEC_F64:
			return a->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[r]) : Float8GetDatum(((const double *) v->data)[r]);
		case SVEC_NUM128:
			return sift_v6_scaled_numeric(((const SiftI128 *) v->data)[r], scale);
		case SVEC_STRVIEW:
			{
				const SiftStrView *s = &((const SiftStrView *) v->data)[r];

				return PointerGetDatum(cstring_to_text_with_len((const char *) sift_strview_bytes(s), s->len));
			}
		case SVEC_DICT32:
			{
				const SiftGranuleDict *d = v->dict;
				const uint32 code = ((const uint32 *) v->data)[r];

				return PointerGetDatum(cstring_to_text_with_len((const char *) d->bytes + d->off[code], d->off[code + 1] - d->off[code]));
			}
		case SVEC_DATUM:
			return ((const Datum *) v->data)[r];
		default:
			return int_vec_datum(a, scale, v, r);
	}
}

static Datum
col_datum(const V6Gen *gen, const V6GenAgg *ga, int c, SiftVec *const *argvec, uint16 r, bool *isnull)
{
	const V6GenCol *col = &ga->col[c];

	return vec_datum(&(V6GenArg) {.typid = col->typid}, col->scale, argvec[col->vec], r, isnull);
}

void
v6_gen_args_of(V6Gen *gen, V6GenAgg *ga, SiftVec *const *argvec, uint16 r, Datum *args, bool *nulls)
{
	for (int k = 0; k < ga->nargs; k++)
	{
		const V6GenArg *a = &ga->arg[k];

		if (a->expr != NULL)
		{
			for (int v = 0; v < a->nvars; v++)
			{
				const int	c = a->var[v];

				gen->slot->tts_values[ga->col[c].att - 1] = col_datum(gen, ga, c, argvec, r, &gen->slot->tts_isnull[ga->col[c].att - 1]);
			}
			args[k] = ExecEvalExprSwitchContext(a->expr, gen->tmp, &nulls[k]);
		}
		else if (a->varies)
			args[k] = col_datum(gen, ga, a->col, argvec, r, &nulls[k]);
		else
		{
			args[k] = a->k;
			nulls[k] = a->knull;
		}
	}
}

/* ---- the state of a group ---- */

/* initialize_aggregate */
static void
state_init(V6GenAgg *ga, V6Acc *acc)
{
	acc->n = V6GEN_INIT;
	if (ga->initnull)
		acc->n |= V6GEN_NULL | (ga->strict ? V6GEN_NOVAL : 0);
	else
	{
		MemoryContext old = MemoryContextSwitchTo(ga->cx->ecxt_per_tuple_memory);

		acc->v0 = datumCopy(ga->initval, ga->tbyval, ga->tlen);
		MemoryContextSwitchTo(old);
	}
}

/* ExecAggCopyTransValue: a new pass-by-reference state is copied into the aggregate's memory, the old one freed */
static void
keep_state(V6GenAgg *ga, V6Acc *acc, int64 f, Datum ov, Datum nv, bool nvnull)
{
	if (!ga->tbyval && DatumGetPointer(nv) != DatumGetPointer(ov))
	{
		if (!nvnull)
		{
			MemoryContext old = MemoryContextSwitchTo(ga->cx->ecxt_per_tuple_memory);

			if (!(DatumIsReadWriteExpandedObject(nv, false, ga->tlen) && MemoryContextGetParent(DatumGetEOHP(nv)->eoh_context) == CurrentMemoryContext))
				nv = datumCopy(nv, ga->tbyval, ga->tlen);
			MemoryContextSwitchTo(old);
		}
		if (!(f & V6GEN_NULL))
		{
			if (DatumIsReadWriteExpandedObject(ov, false, ga->tlen))
				DeleteExpandedObject(ov);
			else
				pfree(DatumGetPointer(ov));
		}
	}
	acc->v0 = (uint64) nv;
	acc->n = (f & ~(V6GEN_NULL | V6GEN_NOVAL)) | (nvnull ? V6GEN_NULL : 0);
}

void
v6_gen_call_context(V6Gen *gen, V6GenAgg *ga, bool final)
{
	gen->as->curpertrans = final ? NULL : &ga->pertrans;
	gen->as->curperagg = final ? &ga->peragg : NULL;
	gen->as->curaggcontext = ga->cx;
}

/* advance_transition_function: the transition function on one input; its temporary memory is the batch's */
void
v6_gen_advance(V6Gen *gen, V6GenAgg *ga, V6Acc *acc, const Datum *args, const bool *nulls)
{
	FunctionCallInfo fc = ga->tfc;
	int64		f;
	Datum		nv,
				ov;
	MemoryContext old;

	if (!(acc->n & V6GEN_INIT))
		state_init(ga, acc);
	f = acc->n;
	if (ga->strict)
	{
		for (int k = 0; k < ga->ntrans; k++)
			if (nulls[k])
				return;
		if (f & V6GEN_NOVAL)
		{
			old = MemoryContextSwitchTo(ga->cx->ecxt_per_tuple_memory);
			acc->v0 = (uint64) datumCopy(args[0], ga->tbyval, ga->tlen);
			MemoryContextSwitchTo(old);
			acc->n = f & ~(V6GEN_NOVAL | V6GEN_NULL);
			return;
		}
		if (f & V6GEN_NULL)
			return;
	}
	ov = (Datum) acc->v0;
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & V6GEN_NULL) != 0;
	for (int k = 0; k < ga->ntrans; k++)
	{
		fc->args[k + 1].value = args[k];
		fc->args[k + 1].isnull = nulls[k];
	}
	fc->isnull = false;
	v6_gen_call_context(gen, ga, false);
	old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);
	nv = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	keep_state(ga, acc, f, ov, nv, fc->isnull);
}

/* advance_combine_function: the state s of another table joins the state d of the same group */
void
v6_gen_combine(V6Gen *gen, int j, V6Acc *d, const V6Acc *s)
{
	V6GenAgg   *ga = gen->agg[j];
	FunctionCallInfo fc = ga->cfc;
	int64		f;
	Datum		ov,
				res;
	MemoryContext old;

	if (!(s->n & V6GEN_INIT))
		return;
	if (!(d->n & V6GEN_INIT))
		state_init(ga, d);
	f = d->n;
	if ((s->n & V6GEN_NULL) && ga->combinefn.fn_strict)
		return;
	if (ga->combinefn.fn_strict && (f & V6GEN_NULL))
	{
		old = MemoryContextSwitchTo(ga->cx->ecxt_per_tuple_memory);
		d->v0 = (uint64) datumCopy((Datum) s->v0, ga->tbyval, ga->tlen);
		MemoryContextSwitchTo(old);
		d->n = f & ~(V6GEN_NULL | V6GEN_NOVAL);
		return;
	}
	ov = (Datum) d->v0;
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & V6GEN_NULL) != 0;
	fc->args[1].value = (Datum) s->v0;
	fc->args[1].isnull = (s->n & V6GEN_NULL) != 0;
	fc->isnull = false;
	v6_gen_call_context(gen, ga, false);
	old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);
	res = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	keep_state(ga, d, f, ov, res, fc->isnull);
	ResetExprContext(gen->tmp);
}

/* finalize_aggregate: the result of the group, in the current memory context */
static Datum
final_value(V6Gen *gen, V6GenAgg *ga, V6Acc *acc, bool *isnull)
{
	const int64 f = acc->n;
	Datum		res;

	if (!ga->has_final)
	{
		*isnull = (f & V6GEN_NULL) != 0;
		return (Datum) acc->v0;
	}
	{
		FunctionCallInfo fc = ga->ffc;
		bool		anynull = (f & V6GEN_NULL) != 0;

		v6_gen_call_context(gen, ga, true);
		fc->args[0].value = MakeExpandedObjectReadOnly((Datum) acc->v0, anynull, ga->tlen);
		fc->args[0].isnull = anynull;
		for (int i = 1; i < ga->nfinal; i++)
		{
			fc->args[i].value = i <= ga->ndirect ? ga->dir[i - 1] : (Datum) 0;
			fc->args[i].isnull = i <= ga->ndirect ? ga->dirnull[i - 1] : true;
			anynull |= fc->args[i].isnull;
		}
		if (ga->finalfn.fn_strict && anynull)
		{
			*isnull = true;
			return (Datum) 0;
		}
		fc->isnull = false;
		res = FunctionCallInvoke(fc);
		*isnull = fc->isnull;
	}
	return res;
}

/* the result of a group whose inputs went through the transition function (or none did): made once, kept in the results' memory */
void
v6_gen_final(V6Gen *gen, V6GenAgg *ga, V6Acc *acc)
{
	MemoryContext old = MemoryContextSwitchTo(gen->res);
	bool		isnull;
	Datum		res;

	if (!(acc->n & V6GEN_INIT))
		state_init(ga, acc);
	res = final_value(gen, ga, acc, &isnull);
	if (!isnull && !ga->rbyval)
		res = datumCopy(res, ga->rbyval, ga->rlen);
	MemoryContextSwitchTo(old);
	acc->v0 = (uint64) res;
	acc->n = (acc->n & V6GEN_INIT) | V6GEN_DONE | (isnull ? V6GEN_RNULL : 0);
}

Datum
v6_gen_datum(V6Gen *gen, int j, V6Acc *acc, bool *isnull)
{
	V6GenAgg   *ga = gen->agg[j];
	Datum		res;

	if (acc->n & V6GEN_DONE)
	{
		*isnull = (acc->n & V6GEN_RNULL) != 0;
		return (Datum) acc->v0;
	}
	if (!(acc->n & V6GEN_INIT))
		state_init(ga, acc);
	res = final_value(gen, ga, acc, isnull);
	if (!*isnull && !ga->rbyval)
		res = datumCopy(res, ga->rbyval, ga->rlen);
	return res;
}

/* aggregate j of a group takes row r of the batch */
void
v6_gen_fold(V6Gen *gen, int j, V6Acc *acc, SiftVec *const *argvec, uint16 r)
{
	V6GenAgg   *ga = gen->agg[j];
	Datum		args[FUNC_MAX_ARGS];
	bool		nulls[FUNC_MAX_ARGS];
	MemoryContext old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);

	v6_gen_args_of(gen, ga, argvec, r, args, nulls);
	if (!ga->sorted)
		v6_gen_advance(gen, ga, acc, args, nulls);
	else if (!ga->ordered || v6_osa_takes(ga, nulls))
	{
		if (acc->v1 == 0)
			acc->v1 = ++ga->nsid;
		acc->aux++;
		v6_gen_sort_put(ga->sort, (uint32) acc->v1 - 1, args, nulls);
	}
	MemoryContextSwitchTo(old);
	ResetExprContext(gen->tmp);
}

/* ---- the scan has ended ---- */

typedef void (*V6AccFn) (void *arg, int j, V6Acc *acc);

/* every state of aggregate j in the groups of every set */
static void
each_acc(SiftV6Group *g, int j, V6AccFn fn, void *arg)
{
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];
		const SiftAggSlot64 *slot;

		if (set->nk == 0)
		{
			if (set->plain_row)
				fn(arg, j, &set->plain[j]);
			continue;
		}
		SIFT_HT_FOREACH(&set->ht, slot)
			fn(arg, j, (V6Acc *) (sift_ht_ext(&set->ht, slot) + g->off[j]));
	}
}

typedef struct V6GenIndex
{
	V6Gen	   *gen;
	V6GenAgg   *ga;
	V6Acc	  **accs;
} V6GenIndex;

static void
index_acc(void *arg, int j, V6Acc *acc)
{
	V6GenIndex *ix = arg;

	if (acc->v1 != 0)
		ix->accs[acc->v1 - 1] = acc;
	else
		v6_gen_final(ix->gen, ix->ga, acc);
}

void
v6_gen_finish(V6Gen *gen, SiftV6Group *g)
{
	for (int j = 0; j < gen->spec->naggs; j++)
	{
		V6GenAgg   *ga = gen->agg[j];
		V6GenIndex	ix = {gen, ga};

		if (ga == NULL || !ga->sorted)
			continue;
		/* check-memory: allow one pointer for each group the scan met, which the group table holds as well */
		ix.accs = palloc0(Max(ga->nsid, 1) * sizeof(V6Acc *));
		each_acc(g, j, index_acc, &ix);
		v6_gen_sort_drain(gen, ga, ix.accs, ga->nsid);
		v6_gen_sort_end(ga->sort);
		ga->sort = NULL;
		pfree(ix.accs);
	}
}

/* ---- the aggregates of a statement ---- */

static Datum
const_arg(Expr *e, MemoryContext cx, Oid typid, bool *isnull)
{
	int16		len;
	bool		byval;
	ExprContext *ec = CreateStandaloneExprContext();
	Datum		d = ExecEvalExprSwitchContext(ExecInitExpr(e, NULL), ec, isnull);

	get_typlenbyval(typid, &len, &byval);
	if (!*isnull)
	{
		MemoryContext old = MemoryContextSwitchTo(cx);

		d = datumCopy(d, byval, len);
		MemoryContextSwitchTo(old);
	}
	FreeExprContext(ec, true);
	return d;
}

static void
check_acl(const Aggref *ref, const Form_pg_aggregate ag, Oid owner)
{
	AclResult	ar = object_aclcheck(ProcedureRelationId, ref->aggfnoid, GetUserId(), ACL_EXECUTE);

	if (ar != ACLCHECK_OK)
		aclcheck_error(ar, OBJECT_AGGREGATE, sift_func_name(ref->aggfnoid));
	foreach_oid(fn, list_make2_oid(ag->aggtransfn, ag->aggfinalfn))
	{
		if (!OidIsValid(fn))
			continue;
		ar = object_aclcheck(ProcedureRelationId, fn, owner, ACL_EXECUTE);
		if (ar != ACLCHECK_OK)
			aclcheck_error(ar, OBJECT_FUNCTION, sift_func_name(fn));
	}
}

/* the functions of the aggregate, ready to be called through fmgr with the AggState as their context */
static void
open_functions(V6Gen *gen, V6GenAgg *ga, Form_pg_aggregate ag, Oid *inputTypes, int numArguments)
{
	Aggref	   *ref = ga->ref;
	Expr	   *e;

	build_aggregate_transfn_expr(inputTypes, numArguments, list_length(ref->aggdirectargs), ref->aggvariadic, ga->transtype, ref->inputcollid, ag->aggtransfn,
								 InvalidOid, &e, NULL);
	fmgr_info(ag->aggtransfn, &ga->transfn);
	fmgr_info_set_expr((Node *) e, &ga->transfn);
	ga->strict = ga->transfn.fn_strict;
	ga->tfc = palloc0(SizeForFunctionCallInfo(1 + ga->ntrans));
	InitFunctionCallInfoData(*ga->tfc, &ga->transfn, 1 + ga->ntrans, ref->inputcollid, (Node *) gen->as, NULL);
	ga->ndirect = list_length(ref->aggdirectargs);
	if (OidIsValid(ag->aggfinalfn))
	{
		ga->has_final = true;
		ga->nfinal = ag->aggfinalextra ? numArguments + 1 : ga->ndirect + 1;
		build_aggregate_finalfn_expr(inputTypes, ga->nfinal, ga->transtype, ref->aggtype, ref->inputcollid, ag->aggfinalfn, &e);
		fmgr_info(ag->aggfinalfn, &ga->finalfn);
		fmgr_info_set_expr((Node *) e, &ga->finalfn);
		ga->ffc = palloc0(SizeForFunctionCallInfo(ga->nfinal));
		InitFunctionCallInfoData(*ga->ffc, &ga->finalfn, ga->nfinal, ref->inputcollid, (Node *) gen->as, NULL);
	}
	if (OidIsValid(ag->aggcombinefn))
	{
		ga->has_combine = true;
		fmgr_info(ag->aggcombinefn, &ga->combinefn);
		ga->cfc = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*ga->cfc, &ga->combinefn, 2, ref->inputcollid, (Node *) gen->as, NULL);
	}
	if (OidIsValid(ag->aggserialfn))
	{
		build_aggregate_serialfn_expr(ag->aggserialfn, &e);
		fmgr_info(ag->aggserialfn, &ga->serialfn);
		fmgr_info_set_expr((Node *) e, &ga->serialfn);
		ga->sfc = palloc0(SizeForFunctionCallInfo(1));
		InitFunctionCallInfoData(*ga->sfc, &ga->serialfn, 1, InvalidOid, (Node *) gen->as, NULL);
		build_aggregate_deserialfn_expr(ag->aggdeserialfn, &e);
		fmgr_info(ag->aggdeserialfn, &ga->deserialfn);
		fmgr_info_set_expr((Node *) e, &ga->deserialfn);
		ga->dfc = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*ga->dfc, &ga->deserialfn, 2, InvalidOid, (Node *) gen->as, NULL);
	}
}

static int
att_pos(const List *atts, int att)
{
	for (int i = 0; i < list_length(atts); i++)
		if (list_nth_int(atts, i) == att)
			return i;
	return -1;
}

/* the Var an argument is, when it is one or a relabeling of one: the column the scan reads as it is */
static const Var *
plain_var(const Expr *e)
{
	while (IsA(e, RelabelType))
		e = ((const RelabelType *) e)->arg;
	return IsA(e, Var) ? (const Var *) e : NULL;
}

/* the distinct columns of the table the arguments of the call read, in the order they first appear: the order of the columns of the aggregate in the spec */
static List *
argument_columns(const Aggref *ref)
{
	List	   *atts = NIL;

	foreach_node(TargetEntry, te, ref->args)
		foreach_node(Var, v, pull_var_clause((Node *) te->expr, PVC_RECURSE_PLACEHOLDERS))
			if (!list_member_int(atts, v->varattno))
				atts = lappend_int(atts, v->varattno);
	return atts;
}

static void
init_arguments(V6Gen *gen, V6GenAgg *ga, const SiftV6Agg *a)
{
	List	   *atts = argument_columns(ga->ref);
	int			k = 0;

	ga->ncols = list_length(atts);
	for (int c = 0; c < ga->ncols; c++)
	{
		const SiftV6Col *col = c == 0 ? &a->arg : c == 1 ? &a->arg2 : &a->xcol[c - 2];

		ga->col[c] = (V6GenCol) {.att = list_nth_int(atts, c),.typid = col->typid,.scale = col->scale,
			.vec = c == 0 ? ga->j : c == 1 ? gen->spec->naggs + ga->j : sift_v6_agg_xvec(gen->spec, ga->j) + c - 2};
		gen->natts = Max(gen->natts, ga->col[c].att);
	}
	foreach_node(TargetEntry, te, ga->ref->args)
	{
		V6GenArg   *arg = &ga->arg[k++];
		const Var  *pv = plain_var(te->expr);
		List	   *vars = pull_var_clause((Node *) te->expr, PVC_RECURSE_PLACEHOLDERS);

		arg->typid = exprType((Node *) te->expr);
		arg->varies = vars != NIL;
		if (vars == NIL)
			arg->k = const_arg(te->expr, gen->cx, arg->typid, &arg->knull);
		else if (pv != NULL)
			arg->col = att_pos(atts, pv->varattno);
		else
		{
			arg->expr = ExecInitExpr(te->expr, NULL);
			arg->var = palloc(list_length(vars) * sizeof(int));
			foreach_node(Var, v, vars)
				arg->var[arg->nvars++] = att_pos(atts, v->varattno);
		}
	}
}

static V6GenAgg *
agg_new(V6Gen *gen, int j, Aggref *ref, int sort_kb)
{
	V6GenAgg   *ga = MemoryContextAllocZero(gen->cx, sizeof(V6GenAgg));
	const SiftV6Agg *a = &gen->spec->agg[j];
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid)),
				proc = SearchSysCache1(PROCOID, ObjectIdGetDatum(ref->aggfnoid));
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments = get_aggregate_argtypes(ref, inputTypes);
	Form_pg_aggregate ag;

	SIFT_INVARIANT(HeapTupleIsValid(tup) && HeapTupleIsValid(proc), "cache lookup failed for aggregate %u", ref->aggfnoid);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	ga->j = j;
	ga->spec = a;
	ga->ref = ref;
	ga->nargs = list_length(ref->args);
	foreach_node(TargetEntry, te, ref->args)
		ga->ntrans += !te->resjunk;
	ga->arg = palloc0(Max(ga->nargs, 1) * sizeof(V6GenArg));
	ga->ordered = ref->aggkind != AGGKIND_NORMAL;
	ga->hyp = ref->aggkind == AGGKIND_HYPOTHETICAL;
	ga->distinct = ref->aggdistinct != NIL;
	ga->sorted = ga->ordered || ga->distinct || ref->aggorder != NIL;
	check_acl(ref, ag, ((Form_pg_proc) GETSTRUCT(proc))->proowner);
	ga->transtype = resolve_aggregate_transtype(ref->aggfnoid, ag->aggtranstype, inputTypes, numArguments);
	get_typlenbyval(ga->transtype, &ga->tlen, &ga->tbyval);
	get_typlenbyval(ref->aggtype, &ga->rlen, &ga->rbyval);
	open_functions(gen, ga, ag, inputTypes, numArguments);
	{
		Datum		txt = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &ga->initnull);

		if (!ga->initnull)
		{
			Oid			infn,
						ioparam;

			getTypeInputInfo(ga->transtype, &infn, &ioparam);
			ga->initval = OidInputFunctionCall(infn, TextDatumGetCString(txt), ioparam, -1);
		}
	}
	ReleaseSysCache(proc);
	ReleaseSysCache(tup);
	ga->dir = palloc0(Max(ga->ndirect, 1) * sizeof(Datum));
	ga->dirnull = palloc0(Max(ga->ndirect, 1) * sizeof(bool));
	foreach_ptr(Expr, d, ref->aggdirectargs)
		ga->dir[foreach_current_index(d)] = const_arg(d, gen->cx, exprType((Node *) d), &ga->dirnull[foreach_current_index(d)]);
	init_arguments(gen, ga, a);
	ga->cx = CreateStandaloneExprContext();
	ga->pertrans.aggref = ref;
	ga->pertrans.aggshared = false;
	ga->peragg.aggref = ref;
	if (ga->sorted)
		ga->sort = v6_gen_sort_begin(ga, gen->cx, sort_kb);
	return ga;
}

/* the sorted aggregates share work_mem: each sort holds an equal part of it, and spills to disk beyond */
static int
sort_share_kb(const SiftV6Spec *spec)
{
	int			sorted = 0;

	foreach_node(Aggref, a, spec->genref)
		sorted += a->aggkind != AGGKIND_NORMAL || a->aggdistinct != NIL || a->aggorder != NIL;
	return Max(work_mem / Max(sorted, 1), 64);
}

/* the row the expressions read: attributes no argument reads are NULL integers, the others have the type of their column */
static void
make_slot(V6Gen *gen)
{
	TupleDesc	desc = CreateTemplateTupleDesc(gen->natts);

	for (int i = 1; i <= gen->natts; i++)
		TupleDescInitEntry(desc, i, "a", INT4OID, -1, 0);
	for (int j = 0; j < gen->spec->naggs; j++)
		for (int c = 0; gen->agg[j] != NULL && c < gen->agg[j]->ncols; c++)
			TupleDescInitEntry(desc, gen->agg[j]->col[c].att, "a", gen->agg[j]->col[c].typid, -1, 0);
	sift_tupdesc_finalize(desc);
	gen->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	ExecClearTuple(gen->slot);
	memset(gen->slot->tts_isnull, true, gen->natts * sizeof(bool));
	ExecStoreVirtualTuple(gen->slot);
	gen->tmp->ecxt_scantuple = gen->slot;
}

V6Gen *
v6_gen_new(const SiftV6Spec *spec, MemoryContext cx, uint64 limit)
{
	V6Gen	   *gen;
	MemoryContext old = MemoryContextSwitchTo(cx);
	const int	sort_kb = sort_share_kb(spec);
	int			at = 0;

	gen = palloc0(sizeof(V6Gen));
	gen->spec = spec;
	gen->cx = cx;
	gen->limit = limit;
	gen->as = makeNode(AggState);
	gen->tmp = CreateStandaloneExprContext();
	gen->as->tmpcontext = gen->tmp;
	gen->res = AllocSetContextCreate(cx, "sift agg generic results", ALLOCSET_SMALL_SIZES);
	gen->agg = palloc0(Max(spec->naggs, 1) * sizeof(V6GenAgg *));
	for (int j = 0; j < spec->naggs; j++)
		if (v6g_is_gen(&spec->agg[j]))
		{
			gen->agg[j] = agg_new(gen, j, list_nth_node(Aggref, spec->genref, at++), sort_kb);
			gen->sorted |= gen->agg[j]->sorted;
		}
	if (gen->natts > 0)
		make_slot(gen);
	MemoryContextSwitchTo(old);
	return gen;
}

void
v6_gen_free(V6Gen *gen)
{
	for (int j = 0; j < gen->spec->naggs; j++)
		if (gen->agg[j] != NULL)
		{
			if (gen->agg[j]->sort != NULL)
				v6_gen_sort_end(gen->agg[j]->sort);
			FreeExprContext(gen->agg[j]->cx, true);
		}
	if (gen->slot != NULL)
		ExecDropSingleTupleTableSlot(gen->slot);
	FreeExprContext(gen->tmp, true);
	MemoryContextDelete(gen->res);
}

uint64
v6_gen_limit(const V6Gen *gen)
{
	return gen->limit;
}

uint64
v6_gen_bytes(const V6Gen *gen)
{
	uint64		bytes = MemoryContextMemAllocated(gen->res, true);

	for (int j = 0; j < gen->spec->naggs; j++)
		if (gen->agg[j] != NULL)
			bytes += MemoryContextMemAllocated(gen->agg[j]->cx->ecxt_per_tuple_memory, true);
	return bytes;
}

bool
v6_gen_can_spill(const V6Gen *gen)
{
	for (int j = 0; j < gen->spec->naggs; j++)
	{
		const V6GenAgg *ga = gen->agg[j];

		if (ga != NULL && (ga->sorted || !ga->has_combine || (ga->transtype == INTERNALOID && (ga->sfc == NULL || ga->dfc == NULL))))
			return false;
	}
	return true;
}

/* a group table that cannot spill holds more than its memory: the planner's estimate of the groups was far off */
void
v6_gen_no_spill(const V6Gen *gen)
{
	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the groups of an aggregate that cannot spill outgrow the memory of the group table"),
					errhint("Raise work_mem or hash_mem_multiplier, or run the statement with sift.enable_agg = off.")));
}

/* ---- the planner ---- */

bool
sift_v6_spec_has_gen(const SiftV6Spec *spec)
{
	for (int j = 0; j < spec->naggs; j++)
		if (v6g_is_gen(&spec->agg[j]))
			return true;
	return false;
}

bool
sift_v6_vec_of_gen(const SiftV6Spec *spec, int v)
{
	for (int j = 0; j < spec->naggs; j++)
		if (v6g_is_gen(&spec->agg[j]) &&
			(v == j || v == spec->naggs + j || (v >= sift_v6_agg_xvec(spec, j) && v < sift_v6_agg_xvec(spec, j) + spec->agg[j].nxcols)))
			return true;
	return false;
}

/* the bytes of a group: its slot, the states of its aggregates and what a sort or a state of a typical aggregate keeps beside them */
#define V6GEN_GROUP_BYTES	128

bool
sift_v6_gen_fits(const SiftV6Spec *spec, double groups)
{
	return !sift_v6_spec_has_gen(spec) ||
		(groups > 0 && groups * (sizeof(SiftAggSlot64) + 1 + (double) spec->naggs * sizeof(V6Acc) + V6GEN_GROUP_BYTES) <= (double) get_hash_memory_limit());
}

/* does the aggregate show a transition or final function that is volatile */
static bool
volatile_fns(Oid agg)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg));
	Form_pg_aggregate ag;
	bool		vol;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for aggregate %u", agg);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	vol = func_volatile(ag->aggtransfn) == PROVOLATILE_VOLATILE || (OidIsValid(ag->aggfinalfn) && func_volatile(ag->aggfinalfn) == PROVOLATILE_VOLATILE);
	ReleaseSysCache(tup);
	return vol;
}

static bool
has_params(Node *n, void *context)
{
	return n != NULL && (IsA(n, Param) || expression_tree_walker(n, has_params, context));
}

/* an expression the executor evaluates by itself: of the columns of the row or of none (then once), stable, and reading nothing the scan cannot give it */
static bool
stable_arg(const Expr *e)
{
	return !contain_volatile_functions((Node *) e) && !contain_subplans((Node *) e) && !contain_agg_clause((Node *) e) && !contain_window_function((Node *) e) &&
		!expression_returns_set((Node *) e) && !has_params((Node *) e, NULL);
}

static int
var_position(const List *vars, const Var *v)
{
	foreach_node(Var, w, vars)
		if (w->varno == v->varno && w->varattno == v->varattno)
			return foreach_current_index(w);
	return -1;
}

/* the distinct columns the arguments read, in the order they first appear: the order of the columns of the aggregate in the spec */
static List *
argument_vars(Aggref *a)
{
	List	   *vars = NIL;

	foreach_node(TargetEntry, te, a->args)
		foreach_node(Var, v, pull_var_clause((Node *) te->expr, PVC_RECURSE_PLACEHOLDERS))
			if (var_position(vars, v) < 0)
				vars = lappend(vars, v);
	return vars;
}

/* a column of any relation of the statement as the attribute the expressions of the aggregates read: its position among the columns that the generic aggregates of the statement read */
static Node *
attribute_of(Node *n, List **vars)
{
	if (n != NULL && IsA(n, Var))
	{
		Var		   *v = (Var *) n;
		int			at = var_position(*vars, v);

		if (at < 0)
		{
			*vars = lappend(*vars, copyObject(v));
			at = list_length(*vars) - 1;
		}
		return (Node *) makeVar(1, at + 1, v->vartype, v->vartypmod, v->varcollid, 0);
	}
	return expression_tree_mutator(n, attribute_of, vars);
}

Aggref *
sift_v6_gen_compile(V6Plan *p, Aggref *a, SiftV6Agg *out)
{
	Aggref	   *ref;
	int			col = 0;

	if (a->agglevelsup != 0 || a->aggsplit != AGGSPLIT_SIMPLE || list_length(a->args) > FUNC_MAX_ARGS || volatile_fns(a->aggfnoid) ||
		(a->aggkind != AGGKIND_NORMAL && !sift_v6_osa_eligible(a)) || (a->aggdistinct != NIL && a->aggkind != AGGKIND_NORMAL) ||
		(a->aggdistinct != NIL && list_length(a->aggdistinct) != list_length(a->args)))
		return NULL;
	foreach_ptr(Node, d, a->aggdirectargs)
		if (contain_var_clause(d) || !stable_arg((Expr *) d))
			return NULL;
	*out = (SiftV6Agg) {.kind = a->aggkind == AGGKIND_NORMAL ? SV6_GEN_PG : SV6_OSA,.restype = a->aggtype};
	foreach_node(TargetEntry, te, a->args)
		if (!stable_arg(te->expr))
			return NULL;
	foreach_node(Var, v, argument_vars(a))
	{
		if (col == V6GEN_MAX_COLS || !sift_v6_plan_read_column(p, (Expr *) v, true, col == 0 ? &out->arg : col == 1 ? &out->arg2 : &out->xcol[col - 2]))
			return NULL;
		col++;
	}
	if (col == 0)
		return NULL;
	out->nxcols = Max(col - 2, 0);
	ref = copyObject(a);
	foreach_node(TargetEntry, te, ref->args)
		te->expr = (Expr *) attribute_of((Node *) te->expr, &p->gen_vars);
	record_plan_function_dependency(p->root, a->aggfnoid);
	return ref;
}
