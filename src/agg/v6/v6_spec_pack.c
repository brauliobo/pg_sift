/*
 * agg/v6/v6_spec_pack.c - the spec of SiftAggV6 as the lists a plan carries (custom_private): the integers of its keys, aggregates, sets, FILTER columns, bins and partitions, then its quals, its FILTER
 * clauses, its quals on jsonb keys and its Ext (v6_ext.h).
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "core/errctx.h"

static List *
put_col(List *l, const SiftV6Col *c)
{
	l = lappend_int(lappend_int(lappend_int(l, c->attno), (int) c->typid), c->scale);
	return lappend_int(lappend_int(lappend_int(lappend_int(lappend_int(lappend_int(l, c->jleaf), (int) c->jtype), (int) c->src), c->dim), c->slot), (int) c->collid);
}

static SiftV6Col
take_col(const List *l, int *at)
{
	SiftV6Col	c = {.attno = list_nth_int(l, *at),.typid = (Oid) list_nth_int(l, *at + 1),.scale = list_nth_int(l, *at + 2),.jleaf = list_nth_int(l, *at + 3),
	.jtype = list_nth_int(l, *at + 4) != 0,.src = (SiftV6Src) list_nth_int(l, *at + 5),.dim = list_nth_int(l, *at + 6),.slot = list_nth_int(l, *at + 7),
	.collid = (Oid) list_nth_int(l, *at + 8)};

	*at += 9;
	return c;
}

static List *
put_int64(List *l, int64 v)
{
	return lappend_int(lappend_int(l, (int) (uint32) v), (int) (uint32) ((uint64) v >> 32));
}

static int64
take_int64(const List *l, int *at)
{
	const uint64 v = (uint64) (uint32) list_nth_int(l, *at) | ((uint64) (uint32) list_nth_int(l, *at + 1) << 32);

	*at += 2;
	return (int64) v;
}

static int64
f64_bits(double d)
{
	int64		v;

	memcpy(&v, &d, sizeof(v));
	return v;
}

static double
bits_f64(int64 v)
{
	double		d;

	memcpy(&d, &v, sizeof(d));
	return d;
}

static List *
desc_list(const char *desc)
{
	return desc != NULL ? list_make1(makeString(pstrdup(desc))) : NIL;
}

List *
sift_v6_spec_pack(const SiftV6Spec *s)
{
	List	   *ints = list_make4_int((int) s->relid, s->nkeys, s->naggs, s->is_table),
			   *l;

	for (int i = 0; i < s->nkeys; i++)
		ints = put_col(ints, &s->key[i]);
	for (int j = 0; j < s->naggs; j++)
	{
		ints = lappend_int(lappend_int(lappend_int(ints, s->agg[j].kind), (int) s->agg[j].restype), s->agg[j].nxcols);
		ints = put_int64(ints, f64_bits(s->agg[j].frac));
		ints = lappend_int(put_col(put_col(ints, &s->agg[j].arg), &s->agg[j].arg2), s->agg[j].fclause);
		for (int c = 0; c < s->agg[j].nxcols; c++)
			ints = put_col(ints, &s->agg[j].xcol[c]);
	}
	ints = lappend_int(ints, s->nsets);
	for (int i = 0; i < s->nsets; i++)
		ints = lappend_int(ints, (int) s->set_mask[i]);
	ints = lappend_int(ints, s->nfuncs);
	for (int f = 0; f < s->nfuncs; f++)
	{
		ints = lappend_int(ints, s->func[f].nargs);
		for (int i = 0; i < s->func[f].nargs; i++)
			ints = lappend_int(ints, s->func[f].key[i]);
	}
	ints = lappend_int(ints, s->nfcols);
	for (int i = 0; i < s->nfcols; i++)
		ints = put_col(ints, &s->fcol[i]);
	ints = lappend_int(ints, s->nbins);
	for (int i = 0; i < s->nbins; i++)
		ints = put_int64(put_int64(lappend_int(ints, s->bin[i].key), s->bin[i].stride_us), s->bin[i].origin);
	ints = lappend_int(ints, s->nparts);
	for (int k = 0; k < s->nparts; k++)
		ints = lappend_int(ints, (int) s->part_index[k]);
	ints = lappend_int(ints, s->part_ncols);
	for (int i = 0; i < s->nparts * s->part_ncols; i++)
		ints = lappend_int(ints, s->part_attno[i]);
	ints = list_concat(ints, list_make4_int(s->par_strategy, s->par_workers, (int) s->text_indexoid, s->constant_groups));
	ints = put_int64(put_int64(lappend_int(ints, s->tid_range), (int64) s->tid_lo), (int64) s->tid_hi);
	ints = sift_v6_derive_pack(ints, s);
	l = list_make5(ints, s->indexqual, s->filter, s->jsonqual, sift_v6_ext_pack(s->ext));
	l = list_concat(l, list_make5(s->text_queries, s->vqual, s->pgexpr, s->genref, desc_list(s->topn_desc)));
	return list_concat(l, list_make2(desc_list(s->agg_desc), desc_list(s->join_desc)));
}

SiftV6Spec *
sift_v6_spec_unpack(const List *l)
{
	const List *ints = linitial(l);
	SiftV6Spec *s = palloc0(sizeof(SiftV6Spec));
	int			at = 4;

	s->relid = (Oid) list_nth_int(ints, 0);
	s->is_table = list_nth_int(ints, 3);
	s->nkeys = list_nth_int(ints, 1);
	s->naggs = list_nth_int(ints, 2);
	s->key = palloc(Max(s->nkeys, 1) * sizeof(SiftV6Col));
	s->agg = palloc(Max(s->naggs, 1) * sizeof(SiftV6Agg));
	for (int i = 0; i < s->nkeys; i++)
		s->key[i] = take_col(ints, &at);
	for (int j = 0; j < s->naggs; j++)
	{
		SiftV6Agg  *a = &s->agg[j];

		*a = (SiftV6Agg) {.kind = list_nth_int(ints, at),.restype = (Oid) list_nth_int(ints, at + 1),.nxcols = list_nth_int(ints, at + 2)};
		at += 3;
		a->frac = bits_f64(take_int64(ints, &at));
		a->arg = take_col(ints, &at);
		a->arg2 = take_col(ints, &at);
		a->fclause = list_nth_int(ints, at++);
		for (int c = 0; c < a->nxcols; c++)
			a->xcol[c] = take_col(ints, &at);
	}
	s->nsets = list_nth_int(ints, at++);
	s->set_mask = palloc(Max(s->nsets, 1) * sizeof(uint32));
	for (int i = 0; i < s->nsets; i++)
		s->set_mask[i] = (uint32) list_nth_int(ints, at++);
	s->nfuncs = list_nth_int(ints, at++);
	s->func = palloc(Max(s->nfuncs, 1) * sizeof(SiftV6Func));
	for (int f = 0; f < s->nfuncs; f++)
	{
		s->func[f].nargs = list_nth_int(ints, at++);
		for (int i = 0; i < s->func[f].nargs; i++)
			s->func[f].key[i] = list_nth_int(ints, at++);
	}
	s->nfcols = list_nth_int(ints, at++);
	for (int i = 0; i < s->nfcols; i++)
		s->fcol[i] = take_col(ints, &at);
	s->nbins = list_nth_int(ints, at++);
	for (int i = 0; i < s->nbins; i++)
	{
		s->bin[i].key = list_nth_int(ints, at++);
		s->bin[i].stride_us = take_int64(ints, &at);
		s->bin[i].origin = take_int64(ints, &at);
	}
	s->nparts = list_nth_int(ints, at++);
	s->part_index = palloc(Max(s->nparts, 1) * sizeof(Oid));
	for (int k = 0; k < s->nparts; k++)
		s->part_index[k] = (Oid) list_nth_int(ints, at++);
	s->part_ncols = list_nth_int(ints, at++);
	s->part_attno = palloc(Max(s->nparts * s->part_ncols, 1) * sizeof(AttrNumber));
	for (int i = 0; i < s->nparts * s->part_ncols; i++)
		s->part_attno[i] = (AttrNumber) list_nth_int(ints, at++);
	s->par_strategy = list_nth_int(ints, at++);
	s->par_workers = list_nth_int(ints, at++);
	s->text_indexoid = (Oid) list_nth_int(ints, at++);
	s->constant_groups = list_nth_int(ints, at++);
	s->tid_range = list_nth_int(ints, at++);
	s->tid_lo = (uint64) take_int64(ints, &at);
	s->tid_hi = (uint64) take_int64(ints, &at);
	sift_v6_derive_unpack(s, ints, at);
	s->text_queries = list_nth(l, 5);
	s->indexqual = lsecond(l);
	s->filter = lthird(l);
	s->nfilters = list_length(s->filter);
	s->jsonqual = lfourth(l);
	s->vqual = list_nth(l, 6);
	s->pgexpr = list_nth(l, 7);
	s->genref = list_nth(l, 8);
	s->topn_desc = list_nth(l, 9) != NIL ? strVal(linitial(list_nth(l, 9))) : NULL;
	s->agg_desc = list_nth(l, 10) != NIL ? strVal(linitial(list_nth(l, 10))) : NULL;
	s->join_desc = list_nth(l, 11) != NIL ? strVal(linitial(list_nth(l, 11))) : NULL;
	s->ext = sift_v6_ext_unpack(list_nth(l, 4));
	return s;
}

SiftV6Ext *
sift_v6_ext_new(void)
{
	return palloc0(sizeof(SiftV6Ext));
}

List *
sift_v6_ext_pack(const SiftV6Ext *x)
{
	List	   *ints;

	if (x == NULL)
		return NIL;
	ints = list_make1_int(x->ndims);
	for (int d = 0; d < x->ndims; d++)
	{
		const SiftV6XDim *xd = &x->dim[d];

		ints = lappend_int(lappend_int(ints, (int) xd->mode), xd->nkeys);
		for (int k = 0; k < xd->nkeys; k++)
		{
			ints = lappend_int(lappend_int(lappend_int(lappend_int(ints, xd->fatt[k]), (int) xd->ftype[k]), (int) xd->ktype[k]), xd->kcol[k]);
			ints = lappend_int(lappend_int(lappend_int(lappend_int(lappend_int(ints, (int) xd->fsrc[k]), xd->fdim[k]), xd->fslot[k]), (int) xd->kvarno[k]), xd->kx[k]);
		}
		ints = lappend_int(ints, xd->npay);
		for (int c = 0; c < xd->npay; c++)
			ints = lappend_int(lappend_int(ints, xd->pcol[c]), (int) xd->ptype[c]);
	}
	ints = lappend_int(ints, x->nrcol);
	for (int c = 0; c < x->nrcol; c++)
		ints = list_concat(ints, list_make5_int(x->rcol[c].attno, (int) x->rcol[c].typid, (int) x->rcol[c].src, x->rcol[c].dim, x->rcol[c].slot));
	ints = lappend_int(ints, x->unnest);
	ints = list_concat(ints, list_make4_int((int) x->elem.fn, x->elem.attno, (int) x->elem.type, x->elem.group_leaf));
	ints = list_concat(ints, list_make5_int(x->elem.left, x->elem.semi, (int) x->elem.vread, x->ntests, x->elem.ncols));
	for (int c = 0; c < x->elem.ncols; c++)
		ints = list_concat(ints, list_make3_int(x->elem.col_leaf[c], (int) x->elem.col_type[c], x->elem.col_kind[c]));
	for (int t = 0; t < x->ntests; t++)
	{
		const SiftV6XTest *xt = &x->test[t];

		ints = list_concat(ints, list_make3_int(xt->need, xt->nullable, xt->ncmps));
		for (int c = 0; c < xt->ncmps; c++)
			ints = list_concat(ints, list_make4_int(xt->cmp[c].op, xt->cmp[c].elem, xt->cmp[c].outer, (int) xt->cmp[c].outer_type));
	}
	ints = list_concat(ints, list_make2_int(x->elem.key_leaf, x->elem.val_leaf));
	for (int c = 0; c < x->elem.ncols; c++)
		ints = lappend_int(ints, x->elem.col_att[c]);
	return list_concat(lappend(lappend(lappend(list_make5(ints, x->where, x->elem_where, x->elem_names, x->heap_where), x->resid), x->elem_array), x->kexpr),
					   list_make3(x->pair_where, x->col_expr, x->test_quals));
}

SiftV6Ext *
sift_v6_ext_unpack(const List *l)
{
	const List *ints;
	SiftV6Ext  *x;
	int			at = 0;

	if (l == NIL)
		return NULL;
	ints = linitial(l);
	x = sift_v6_ext_new();
	x->ndims = list_nth_int(ints, at++);
	for (int d = 0; d < x->ndims; d++)
	{
		SiftV6XDim *xd = &x->dim[d];

		xd->mode = (SiftV6JoinMode) list_nth_int(ints, at++);
		xd->nkeys = list_nth_int(ints, at++);
		for (int k = 0; k < xd->nkeys; k++, at += 9)
		{
			xd->fatt[k] = list_nth_int(ints, at);
			xd->ftype[k] = (Oid) list_nth_int(ints, at + 1);
			xd->ktype[k] = (Oid) list_nth_int(ints, at + 2);
			xd->kcol[k] = (AttrNumber) list_nth_int(ints, at + 3);
			xd->fsrc[k] = (SiftV6Src) list_nth_int(ints, at + 4);
			xd->fdim[k] = list_nth_int(ints, at + 5);
			xd->fslot[k] = list_nth_int(ints, at + 6);
			xd->kvarno[k] = (Index) list_nth_int(ints, at + 7);
			xd->kx[k] = list_nth_int(ints, at + 8);
		}
		xd->npay = list_nth_int(ints, at++);
		for (int c = 0; c < xd->npay; c++, at += 2)
		{
			xd->pcol[c] = (AttrNumber) list_nth_int(ints, at);
			xd->ptype[c] = (Oid) list_nth_int(ints, at + 1);
		}
	}
	x->nrcol = list_nth_int(ints, at++);
	for (int c = 0; c < x->nrcol; c++, at += 5)
		x->rcol[c] = (SiftV6Col) {.attno = list_nth_int(ints, at),.typid = (Oid) list_nth_int(ints, at + 1),.src = (SiftV6Src) list_nth_int(ints, at + 2),
		.dim = list_nth_int(ints, at + 3),.slot = list_nth_int(ints, at + 4)};
	x->unnest = list_nth_int(ints, at++);
	x->elem = (SiftV6XElem) {.fn = (SiftV6ElemFn) list_nth_int(ints, at),.attno = list_nth_int(ints, at + 1),.type = (Oid) list_nth_int(ints, at + 2),
	.group_leaf = list_nth_int(ints, at + 3),.left = list_nth_int(ints, at + 4),.semi = list_nth_int(ints, at + 5),
	.vread = (SiftV6ValueRead) list_nth_int(ints, at + 6),.ncols = list_nth_int(ints, at + 8)};
	x->ntests = list_nth_int(ints, at + 7);
	at += 9;
	for (int c = 0; c < x->elem.ncols; c++, at += 3)
	{
		x->elem.col_leaf[c] = list_nth_int(ints, at);
		x->elem.col_type[c] = (Oid) list_nth_int(ints, at + 1);
		x->elem.col_kind[c] = list_nth_int(ints, at + 2) != 0;
	}
	for (int t = 0; t < x->ntests; t++)
	{
		SiftV6XTest *xt = &x->test[t];

		*xt = (SiftV6XTest) {.need = list_nth_int(ints, at),.nullable = list_nth_int(ints, at + 1) != 0,.ncmps = list_nth_int(ints, at + 2)};
		at += 3;
		for (int c = 0; c < xt->ncmps; c++, at += 4)
			xt->cmp[c] = (SiftV6XCmp) {.op = list_nth_int(ints, at),.elem = list_nth_int(ints, at + 1),.outer = list_nth_int(ints, at + 2),.outer_type = (Oid) list_nth_int(ints, at + 3)};
	}
	x->elem.key_leaf = list_nth_int(ints, at);
	x->elem.val_leaf = list_nth_int(ints, at + 1);
	at += 2;
	for (int c = 0; c < x->elem.ncols; c++)
		x->elem.col_att[c] = list_nth_int(ints, at++);
	x->where = lsecond(l);
	x->elem_where = lthird(l);
	x->elem_names = lfourth(l);
	x->heap_where = list_nth(l, 4);
	x->resid = list_nth(l, 5);
	x->elem_array = list_nth(l, 6);
	x->kexpr = list_nth(l, 7);
	x->pair_where = list_nth(l, 8);
	x->col_expr = list_nth(l, 9);
	x->test_quals = list_nth(l, 10);
	return x;
}

static AttrNumber
column_position(const Plan *plan, Index varno, AttrNumber attno)
{
	foreach_node(TargetEntry, te, plan->targetlist)
		if (IsA(te->expr, Var) && ((Var *) te->expr)->varattno == attno && (varno == 0 || ((Var *) te->expr)->varno == varno))
			return te->resno;
	SIFT_INVARIANT(false, "a column of a dimension of the star join is in the output of its plan");
	return 0;
}

SiftV6Col
sift_v6_xdim_key_col(const SiftV6XDim *xd, int k)
{
	return (SiftV6Col) {.attno = xd->fatt[k],.typid = xd->ftype[k],.src = xd->fsrc[k],.dim = xd->fdim[k],.slot = xd->fslot[k]};
}

void
sift_v6_ext_resolve(SiftV6Ext *x, const List *custom_plans)
{
	for (int d = 0; d < x->ndims; d++)
	{
		SiftV6XDim *xd = &x->dim[d];
		const Plan *plan = list_nth(custom_plans, d);

		for (int k = 0; k < xd->nkeys; k++)
			xd->kcol[k] = column_position(plan, xd->kvarno[k], xd->kcol[k]);
		for (int c = 0; c < xd->npay; c++)
			xd->pcol[c] = column_position(plan, 0, xd->pcol[c]);
	}
}
