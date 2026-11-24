/*
 * col/source_fold.c - the aggregate scan over the batches of a SiftV6Source: the columns the keys, aggregates, derived values, FILTER clauses and quals of the statement read are asked of the source as
 * vectors of the classes the groups take (an integer of any width as SVEC_I64, a float as SVEC_F64, a text as SVEC_STRVIEW), and each batch is narrowed by the quals no scan key says, derived, binned and
 * folded into the groups (or the pipe of a star join, or the emit of a participant of a parallel run). The rows a batch holds are those the source's own visibility and quals leave.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the relation */
#include "postgres.h"

#include "access/skey.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_bqual.h"
#include "agg/v6/v6_datebin_win_pg.h"
#include "agg/v6/v6_expr_num.h"
#include "agg/v6/v6_expr_str.h"
#include "agg/v6/v6_filter_eval.h"
#include "agg/v6/v6_fuse.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_star_run.h"
#include "agg/v6/v6_vexpr.h"
#include "col/source_fold.h"
#include "core/memtrack.h"
#include "nodes/pathnodes.h"

typedef struct FoldCol
{
	int			attno;
	SiftV6Class cls;
	bool		nan_ok;			/* a numeric whose every use is the argument of an aggregate that folds a NaN */
	int			slot;			/* of the source's input */
} FoldCol;

typedef struct FoldBin
{
	int			key;
	bool		month;
	SiftDateBin bin;
	const SiftVec *src;
	int64	   *out;
	SiftVec		vec;
} FoldBin;

typedef struct FoldDerived
{
	const SiftV6Derive *d;
	const SiftVec *src;
	SiftVec    *cond[SIFT_V6_MAX_CASE];
	const struct FoldDerived *prefix;
	SiftV6PgExpr *pg;
	const SiftVec *pgin[SIFT_V6_MAX_COLS];
	SiftVxOut	out;
} FoldDerived;

struct SiftSrcFold
{
	SiftSrcFoldArgs a;
	MemoryContext cx;
	SiftV6Input in;
	SiftV6Vexpr *quals;
	List	   *residual;		/* the clauses of a.quals the source does not compile: a batch qual of the fold decides them */
	int			nfold;
	FoldCol		col[SIFT_V6_MAX_COLS];
	SiftVec		vec[SIFT_V6_MAX_COLS];
	SiftVec   **keyvec;
	SiftVec   **argvec;
	SiftVec   **fact;
	int			nbins;
	FoldBin	   *bins;
	int			nderived;
	FoldDerived *derived;
	SiftV6Bq   *bq;
	SiftV6Pipe *pipe;
	SiftBatchSel bs;
	bool		count_only;
};

static FoldCol *
col_for(SiftSrcFold *f, SiftV6Col c, bool takes_nan)
{
	SiftV6Class cls;
	int			attno;

	if (c.derive > 0)
		c = f->a.spec->derive[c.derive - 1].src;
	cls = sift_v6_class(c.typid);
	attno = c.jleaf > 0 ? SIFT_V6_JLEAF_VATT + c.jleaf - 1 : c.attno;
	SIFT_INVARIANT((c.attno > 0 || c.attno == SIFT_V6_SCORE_ATTNO) && cls != SV6C_NONE, "column %d of relation %s is not one the source fold reads", c.attno, sift_rel_label(f->a.spec->relid));
	for (int i = 0; i < f->nfold; i++)
		if (f->col[i].attno == attno)
		{
			f->col[i].nan_ok &= takes_nan;
			return &f->col[i];
		}
	f->col[f->nfold] = (FoldCol) {.attno = attno,.cls = cls,.nan_ok = takes_nan,.slot = f->in.ncols};
	f->in.col[f->in.ncols++] = (SiftV6InCol) {.attno = attno,.typid = c.typid,.scale = c.scale,
		.type = cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64};
	return &f->col[f->nfold++];
}

static SiftVec *
vec_for(SiftSrcFold *f, SiftV6Col c, bool takes_nan)
{
	return &f->vec[col_for(f, c, takes_nan)->slot];
}

static int
slot_of(SiftSrcFold *f, const SiftV6Col *c)
{
	return c->src == SV6SRC_DIM ? sift_v6_pipe_dim_slot(f->pipe, c->dim, c->slot) : col_for(f, *c, false)->slot;
}

static void
setup_pipe(SiftSrcFold *f)
{
	const SiftV6Spec *spec = f->a.spec;
	const SiftV6Ext *x = spec->ext;
	SiftVecType *type = palloc(Max(f->in.ncols, 1) * sizeof(SiftVecType));

	for (int i = 0; i < f->in.ncols; i++)
		type[i] = f->in.col[i].type;
	f->pipe = sift_v6_pipe_new(f->cx, spec, f->a.group, f->in.ncols, type);
	sift_v6_star_attach(f->a.star, f->pipe);
	for (int d = 0; d < x->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			f->pipe->fslot[d][k] = slot_of(f, &(SiftV6Col) {.attno = x->dim[d].fatt[k],.typid = x->dim[d].ftype[k]});
	for (int i = 0; i < x->nrcol; i++)
		f->pipe->rslot[i] = slot_of(f, &x->rcol[i]);
	for (int i = 0; i < spec->nkeys; i++)
		f->pipe->key_slot[i] = slot_of(f, &spec->key[i]);
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		f->pipe->arg_slot[v] = sift_v6_vec_col(spec, v) == NULL ? -1 : slot_of(f, sift_v6_vec_col(spec, v));
	f->fact = palloc(Max(f->in.ncols, 1) * sizeof(SiftVec *));
	for (int i = 0; i < f->in.ncols; i++)
		f->fact[i] = &f->vec[i];
}

static void
setup_derived(SiftSrcFold *f)
{
	const SiftV6Spec *spec = f->a.spec;

	f->derived = palloc0(Max(spec->nderive, 1) * sizeof(FoldDerived));
	for (int k = 0; k < spec->nderive; k++)
	{
		FoldDerived *dv = &f->derived[f->nderived++];

		dv->d = &spec->derive[k];
		for (int j = 0; j < k && dv->prefix == NULL; j++)
			if (sift_vn_prefix_of(&spec->derive[j], dv->d))
				dv->prefix = &f->derived[j];
		dv->src = vec_for(f, dv->d->src, false);
		for (int i = 0; i < dv->d->ncond; i++)
			dv->cond[i] = vec_for(f, dv->d->cond[i], false);
		sift_vx_out_init(&dv->out, f->cx);
	}
	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].derive > 0)
			f->keyvec[i] = &f->derived[spec->key[i].derive - 1].out.vec;
	for (int j = 0; j < spec->naggs; j++)
		if (spec->agg[j].arg.derive > 0)
			f->argvec[j] = &f->derived[spec->agg[j].arg.derive - 1].out.vec;
}

static void
setup_bins(SiftSrcFold *f)
{
	const SiftV6Spec *spec = f->a.spec;

	f->bins = palloc0(Max(spec->nbins, 1) * sizeof(FoldBin));
	for (int b = 0; b < spec->nbins; b++)
	{
		FoldBin    *bn = &f->bins[f->nbins++];

		bn->key = spec->bin[b].key;
		bn->month = spec->bin[b].stride_us == SIFT_DATEBIN_MONTH;
		bn->bin = (SiftDateBin) {.stride_us = spec->bin[b].stride_us,.origin = spec->bin[b].origin,.stride_status = SDB_OK};
		bn->src = f->keyvec[bn->key];
		bn->out = MemoryContextAllocAligned(f->cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		f->keyvec[bn->key] = &bn->vec;
	}
}

static const SiftVec *
bq_col(void *arg, const SiftV6InCol *c)
{
	return vec_for(arg, (SiftV6Col) {.attno = c->attno,.typid = c->typid,.scale = c->scale}, false);
}

static void
setup_bq(SiftSrcFold *f)
{
	SiftV6Input in;

	List	   *clauses = list_concat_copy(f->a.spec->vqual, f->residual);

	if (clauses == NIL)
		return;
	if (f->a.spec->is_table)
		sift_v6_input_init_table(&in, f->a.host);
	else
		sift_v6_input_init_rel(&in, f->a.host);
	f->bq = sift_v6_bq_open(f->cx, clauses, &in, bq_col, f);
}

static void
setup_pgexprs(SiftSrcFold *f)
{
	for (int k = 0; k < f->nderived; k++)
		if (f->derived[k].d->kind == SVD_PG_EXPR)
			f->derived[k].pg = sift_v6_pgexpr_bind(f->cx, f->a.spec, f->derived[k].d, bq_col, f, f->derived[k].pgin);
}

/* Whether the source compiles the clause over the columns of the input: the others are the fold's to decide on the decoded batch */
static bool
source_decides(MemoryContext cx, const SiftSrcFold *f, Node *clause)
{
	SiftV6Input scratch = f->in;
	SiftV6Vexpr probe;
	MemoryContext trial = AllocSetContextCreate(cx, "sift source fold trial", ALLOCSET_SMALL_SIZES);
	const bool	ok = sift_v6_vexpr_quals(trial, list_make1(clause), 1, f->a.estate != NULL ? f->a.estate->es_param_list_info : NULL, &scratch, &probe);

	MemoryContextDelete(trial);
	return ok;
}

static void
setup_quals(SiftSrcFold *f)
{
	List	   *source = NIL;

	if (f->a.spec->is_table)
		sift_v6_input_init_table(&f->in, f->a.host);
	else
		sift_v6_input_init_rel(&f->in, f->a.host);
	if (f->a.estate != NULL)
		sift_v6_input_bind(&f->in, f->a.estate);
	foreach_ptr(Node, clause, f->a.quals)
		if (source_decides(f->cx, f, clause))
			source = lappend(source, clause);
		else
			f->residual = lappend(f->residual, clause);
	if (source == NIL)
		return;
	f->quals = palloc0(sizeof(SiftV6Vexpr));
	if (!sift_v6_vexpr_quals(f->cx, source, 1, f->a.estate != NULL ? f->a.estate->es_param_list_info : NULL, &f->in, f->quals))
		SIFT_INVARIANT(false, "the quals of the aggregate scan of %s compiled when each was tried alone", sift_rel_label(f->a.spec->relid));
}

static bool
vec_takes_nan(const SiftV6Spec *spec, int v)
{
	return (v < spec->naggs && sift_v6_kind_reads_column(spec->agg[v].kind) && sift_v6_agg_takes_nan(&spec->agg[v])) || sift_v6_vec_of_gen(spec, v);
}

SiftSrcFold *
sift_srcfold_new(const SiftSrcFoldArgs *a, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftSrcFold *f = sift_palloc0_aligned(SiftSrcFold, 1);
	const SiftV6Spec *spec = a->spec;
	const int	nvec = sift_v6_spec_nvec(spec);

	f->a = *a;
	f->cx = cx;
	setup_quals(f);
	f->count_only = spec->rows == false && spec->ext == NULL && spec->vqual == NIL && f->residual == NIL && sift_v6_spec_count_only(spec);
	f->keyvec = palloc0(Max(spec->nkeys, 1) * sizeof(SiftVec *));
	f->argvec = palloc0(Max(nvec, 1) * sizeof(SiftVec *));
	if (!f->count_only)
	{
		for (int i = 0; i < spec->nkeys; i++)
			if (spec->key[i].src != SV6SRC_DIM)
				f->keyvec[i] = vec_for(f, spec->key[i], false);
		for (int v = 0; v < nvec; v++)
			if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src != SV6SRC_DIM)
				f->argvec[v] = vec_for(f, *sift_v6_vec_col(spec, v), vec_takes_nan(spec, v));
		for (int d = 0; spec->ext != NULL && d < spec->ext->ndims; d++)
			for (int k = 0; k < spec->ext->dim[d].nkeys; k++)
				col_for(f, (SiftV6Col) {.attno = spec->ext->dim[d].fatt[k],.typid = spec->ext->dim[d].ftype[k]}, false);
		for (int i = 0; spec->ext != NULL && i < spec->ext->nrcol; i++)
			if (spec->ext->rcol[i].src == SV6SRC_ATTR)
				col_for(f, spec->ext->rcol[i], false);
		setup_derived(f);
		setup_bq(f);
		setup_pgexprs(f);
		setup_bins(f);
		if (spec->ext != NULL && spec->ext->ndims > 0)
			setup_pipe(f);
	}
	for (int i = 0; i < f->in.ncols; i++)
		f->vec[i].type = f->in.col[i].type;
	MemoryContextSwitchTo(old);
	return f;
}

/* A NaN that no aggregate folds (a count of the distinct values, a filter, an argument of sift.arg_max) is not left out as a NULL would be: the statement is refused */
static void
refuse_nan(const FoldCol *c, const SiftVec *v, const SiftBatchSel *bs)
{
	if (v->nan == NULL || c->nan_ok)
		return;
	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		if ((v->nan[w] & bs->mask[w]) != 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a numeric NaN cannot be used by this aggregate on the stored values"),
							errhint("Run the statement with sift.enable_agg = off.")));
}

static void
batch_rows(SiftSrcFold *f, const SiftV6Batch *b)
{
	SiftBatchSel *bs = &f->bs;
	const int	words = (b->n + 63) / 64;

	memset(bs->mask, 0, sizeof(bs->mask));
	for (int w = 0; w < words; w++)
		bs->mask[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
	if (b->n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
	bs->n_raw = b->n;
	bs->n_active = 0;
	for (int w = 0; w < words; w++)
		bs->n_active += pg_popcount64(bs->mask[w]);
	bs->all_active = bs->n_active == b->n;
	bs->has_sel = false;
}

static void
take_gate(SiftSrcFold *f)
{
	SiftBatchSel *bs = &f->bs;
	uint64		allowed[SIFT_VEC_WORDS];

	sift_clause_eval(f->a.gate, f->argvec + sift_v6_spec_fvec(f->a.spec), bs->n_raw, allowed);
	bs->n_active = 0;
	for (int w = 0; w < SIFT_VEC_WORDS; w++)
		bs->n_active += pg_popcount64(bs->mask[w] &= allowed[w]);
	bs->all_active = bs->n_active == bs->n_raw;
}

static void
derive_columns(SiftSrcFold *f)
{
	for (int k = 0; k < f->nderived; k++)
	{
		const FoldDerived *dv = &f->derived[k];

		if (dv->d->kind == SVD_PG_EXPR)
			sift_v6_pgexpr_derive(dv->pg, dv->pgin, &f->bs, &f->derived[k].out);
		else if (dv->d->kind == SVD_NUM_EXPR)
			sift_vn_derive(dv->d, dv->cond, dv->prefix ? &dv->prefix->out.vec : NULL, dv->prefix ? dv->prefix->d->ncond : 0, &f->bs, &f->derived[k].out);
		else
			sift_vx_derive(dv->d, dv->src, dv->cond, &f->bs, &f->derived[k].out);
	}
	if (f->a.spec->affine)
		sift_vx_affine_check(f->a.spec, f->keyvec[0], &f->bs);
}

static void
bin_keys(SiftSrcFold *f)
{
	for (int i = 0; i < f->nbins; i++)
	{
		FoldBin    *b = &f->bins[i];
		uint32		err_row;
		SiftDateBinStatus st = SDB_OK;

		if (b->month)
			sift_datemonth_batch(&f->bs, b->src, b->out);
		else
			st = sift_datebin_batch(&b->bin, &f->bs, b->src, b->out, &err_row);
		if (st != SDB_OK)
			sift_datebin_raise(st);
		b->vec = *b->src;
		b->vec.data = b->out;
	}
}

void
sift_srcfold_batch(SiftSrcFold *f, const SiftV6Batch *b)
{
	SiftBatchSel *bs = &f->bs;

	f->a.stats->batches++;
	batch_rows(f, b);
	if (bs->n_active == 0)
		return;
	if (f->count_only)
	{
		sift_v6_group_add_rows(f->a.group, bs->n_active);
		f->a.stats->rows += bs->n_active;
		return;
	}
	for (int i = 0; i < f->nfold; i++)
	{
		SiftVec    *v = &f->vec[f->col[i].slot];

		*v = b->col[f->col[i].slot];
		v->sel = NULL;
		refuse_nan(&f->col[i], v, bs);
	}
	if (f->bq != NULL)
		sift_v6_bq_filter(f->bq, bs);
	if (f->a.gate != NULL && bs->n_active > 0)
		take_gate(f);
	if (bs->n_active == 0)
		return;
	derive_columns(f);
	bin_keys(f);
	f->a.stats->rows += bs->n_active;
	if (f->a.emit != NULL)
		f->a.emit(f->a.emit_arg, bs, f->keyvec, f->argvec);
	else if (f->pipe != NULL)
		sift_v6_pipe_run(f->pipe, f->fact, bs);
	else
		sift_v6_group_update(f->a.group, bs, f->keyvec, f->argvec);
}

const SiftV6Input *
sift_srcfold_input(const SiftSrcFold *f)
{
	return &f->in;
}

void
sift_srcfold_run(SiftSrcFold *f, SiftSrcOpen open, void *open_arg)
{
	SiftV6Source *src = open(open_arg, &f->in, f->quals);
	SiftV6Batch b;

	while (src->next(src, &b))
		sift_srcfold_batch(f, &b);
	src->close(src);
}

void
sift_srcfold_arms(SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, const SiftSrcFoldArgs *common, SiftSrcOpen open, void *open_arg)
{
	for (int i = 0; i < cohort->narms; i++)
	{
		SiftSrcFoldArgs a = *common;
		SiftSrcFold *f;

		a.spec = arms[i].spec;
		a.group = arms[i].group;
		a.stats = arms[i].stats;
		a.gate = arms[i].gate;
		f = sift_srcfold_new(&a, sift_mem_allocset_create(CurrentMemoryContext, "sift source fold arm"));
		sift_srcfold_run(f, open, open_arg);
		sift_srcfold_free(f);
	}
}

StaticAssertDecl(SIFT_SRC_MAX_BOUNDS == SIFT_V6_MAX_BOUNDS, "a source takes the bounds the program gives");

int
sift_srcfold_bounds(const SiftV6Vexpr *quals, SiftSrcBound *out)
{
	for (int i = 0; i < quals->nbounds; i++)
		out[i] = (SiftSrcBound) {.slot = quals->bound[i].slot,.has_lo = quals->bound[i].has_lo,.has_hi = quals->bound[i].has_hi,.lo = quals->bound[i].lo,.hi = quals->bound[i].hi};
	return quals->nbounds;
}

void
sift_srcfold_count_chunk(SiftV6ScanStats *stats, const SiftTidBridge *bridge)
{
	stats->chunks++;
	stats->bridge_tids += bridge->ntids;
}

void
sift_srcfold_free(SiftSrcFold *f)
{
	MemoryContextDelete(f->cx);
}

/* ---- the quals an index scan says ---- */

static Var *
heap_var(Relation index, int att)
{
	const AttrNumber heap_att = index->rd_index->indkey.values[att - 1];
	Oid			typid;
	int32		typmod;
	Oid			coll;

	if (heap_att == 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a scan key of index \"%s\" is on an expression the stored columns do not give", RelationGetRelationName(index)),
						errhint("Run the statement with sift.enable_agg = off.")));
	get_atttypetypmodcoll(index->rd_index->indrelid, heap_att, &typid, &typmod, &coll);
	return makeVar(1, heap_att, typid, typmod, coll, 0);
}

static Node *
key_clause(Relation index, const ScanKeyData *k)
{
	Var		   *var = heap_var(index, k->sk_attno);
	const int	a = k->sk_attno - 1;
	const Oid	right = OidIsValid(k->sk_subtype) ? k->sk_subtype : var->vartype;
	Oid			opno;
	int16		typlen;
	bool		byval;

	if (k->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL))
	{
		NullTest   *nt = makeNode(NullTest);

		nt->arg = (Expr *) var;
		nt->nulltesttype = (k->sk_flags & SK_SEARCHNULL) ? IS_NULL : IS_NOT_NULL;
		return (Node *) nt;
	}
	opno = get_opfamily_member(index->rd_opfamily[a], var->vartype, right, k->sk_strategy);
	if (!OidIsValid(opno))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a scan key of index \"%s\" is not one the stored columns decide", RelationGetRelationName(index)),
						errhint("Run the statement with sift.enable_agg = off.")));
	if (k->sk_flags & SK_SEARCHARRAY)
	{
		ScalarArrayOpExpr *sa = makeNode(ScalarArrayOpExpr);
		const Oid	arr = get_array_type(right);

		get_typlenbyval(arr, &typlen, &byval);
		sa->opno = opno;
		sa->opfuncid = get_opcode(opno);
		sa->useOr = true;
		sa->inputcollid = k->sk_collation;
		sa->args = list_make2(var, makeConst(arr, -1, InvalidOid, typlen, k->sk_argument, false, byval));
		return (Node *) sa;
	}
	get_typlenbyval(right, &typlen, &byval);
	return (Node *) make_opclause(opno, BOOLOID, false, (Expr *) var, (Expr *) makeConst(right, -1, k->sk_collation, typlen, k->sk_argument, false, byval), InvalidOid, k->sk_collation);
}

List *
sift_srcfold_scankey_quals(Relation index, ScanKey keys, int nkeys)
{
	List	   *quals = NIL;

	for (int i = 0; i < nkeys; i++)
		if (keys[i].sk_strategy != COL_STRAT_JSONB_EXISTS && keys[i].sk_strategy != COL_STRAT_JSONB_TYPE)
			quals = lappend(quals, key_clause(index, &keys[i]));
	return quals;
}

static Node *
heap_vars_mutator(Node *node, Relation host)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, RestrictInfo))
		return heap_vars_mutator((Node *) ((RestrictInfo *) node)->clause, host);
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *v = copyObject((Var *) node);

		v->varno = 1;
		v->varnosyn = 1;
		if (host->rd_index != NULL)
			v->varattno = v->varattnosyn = heap_var(host, v->varattno)->varattno;
		return (Node *) v;
	}
	return expression_tree_mutator(node, heap_vars_mutator, host);
}

List *
sift_srcfold_heap_quals(Relation host, const List *indexqual)
{
	List	   *quals = NIL;

	foreach_ptr(Node, q, indexqual)
		quals = lappend(quals, heap_vars_mutator(q, host));
	return quals;
}
