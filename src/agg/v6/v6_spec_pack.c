/*
 * agg/v6/v6_spec_pack.c - the spec of SiftAggV6 as the lists a plan carries (custom_private): the integers of its keys, aggregates, sets, FILTER columns, bins and partitions, then its quals, its FILTER
 * clauses, its quals on jsonb keys and its Ext (v6_ext.h).
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

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
	return lappend_int(lappend_int(lappend_int(lappend_int(l, c->jleaf), (int) c->src), c->dim), c->slot);
}

static SiftV6Col
take_col(const List *l, int *at)
{
	SiftV6Col	c = {.attno = list_nth_int(l, *at),.typid = (Oid) list_nth_int(l, *at + 1),.scale = list_nth_int(l, *at + 2),.jleaf = list_nth_int(l, *at + 3),
	.src = (SiftV6Src) list_nth_int(l, *at + 4),.dim = list_nth_int(l, *at + 5),.slot = list_nth_int(l, *at + 6)};

	*at += 7;
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

List *
sift_v6_spec_pack(const SiftV6Spec *s)
{
	List	   *ints = list_make4_int((int) s->relid, s->nkeys, s->naggs, s->is_table);

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
	ints = list_concat(ints, list_make3_int(s->par_strategy, s->par_workers, (int) s->text_indexoid));
	ints = sift_v6_derive_pack(ints, s);
	return lappend(list_make5(ints, s->indexqual, s->filter, s->jsonqual, sift_v6_ext_pack(s->ext)), s->text_query ? list_make1(s->text_query) : NIL);
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
	s->par_strategy = list_nth_int(ints, at++);
	s->par_workers = list_nth_int(ints, at++);
	s->text_indexoid = (Oid) list_nth_int(ints, at++);
	sift_v6_derive_unpack(s, ints, at);
	s->text_query = list_nth(l, 5) ? linitial_node(Const, (List *) list_nth(l, 5)) : NULL;
	s->indexqual = lsecond(l);
	s->filter = lthird(l);
	s->nfilters = list_length(s->filter);
	s->jsonqual = lfourth(l);
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
			ints = lappend_int(lappend_int(lappend_int(lappend_int(ints, xd->fatt[k]), (int) xd->ftype[k]), (int) xd->ktype[k]), xd->kcol[k]);
		ints = lappend_int(ints, xd->npay);
		for (int c = 0; c < xd->npay; c++)
			ints = lappend_int(lappend_int(ints, xd->pcol[c]), (int) xd->ptype[c]);
	}
	ints = lappend_int(ints, x->unnest);
	ints = list_concat(ints, list_make4_int((int) x->elem.fn, x->elem.attno, (int) x->elem.type, x->elem.group_leaf));
	ints = list_concat(ints, list_make3_int(x->elem.left, x->elem.semi, x->elem.ncols));
	for (int c = 0; c < x->elem.ncols; c++)
		ints = lappend_int(lappend_int(ints, x->elem.col_leaf[c]), (int) x->elem.col_type[c]);
	return list_make5(ints, x->where, x->elem_where, x->elem_names, x->heap_where);
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
		for (int k = 0; k < xd->nkeys; k++, at += 4)
		{
			xd->fatt[k] = list_nth_int(ints, at);
			xd->ftype[k] = (Oid) list_nth_int(ints, at + 1);
			xd->ktype[k] = (Oid) list_nth_int(ints, at + 2);
			xd->kcol[k] = (AttrNumber) list_nth_int(ints, at + 3);
		}
		xd->npay = list_nth_int(ints, at++);
		for (int c = 0; c < xd->npay; c++, at += 2)
		{
			xd->pcol[c] = (AttrNumber) list_nth_int(ints, at);
			xd->ptype[c] = (Oid) list_nth_int(ints, at + 1);
		}
	}
	x->unnest = list_nth_int(ints, at++);
	x->elem = (SiftV6XElem) {.fn = (SiftV6ElemFn) list_nth_int(ints, at),.attno = list_nth_int(ints, at + 1),.type = (Oid) list_nth_int(ints, at + 2),
	.group_leaf = list_nth_int(ints, at + 3),.left = list_nth_int(ints, at + 4),.semi = list_nth_int(ints, at + 5),.ncols = list_nth_int(ints, at + 6)};
	at += 7;
	for (int c = 0; c < x->elem.ncols; c++, at += 2)
	{
		x->elem.col_leaf[c] = list_nth_int(ints, at);
		x->elem.col_type[c] = (Oid) list_nth_int(ints, at + 1);
	}
	x->where = lsecond(l);
	x->elem_where = lthird(l);
	x->elem_names = lfourth(l);
	x->heap_where = list_nth(l, 4);
	return x;
}

static AttrNumber
column_position(const Plan *plan, AttrNumber attno)
{
	foreach_node(TargetEntry, te, plan->targetlist)
		if (IsA(te->expr, Var) && ((Var *) te->expr)->varattno == attno)
			return te->resno;
	SIFT_INVARIANT(false, "a column of a dimension of the star join is in the output of its plan");
	return 0;
}

void
sift_v6_ext_resolve(SiftV6Ext *x, const List *custom_plans)
{
	for (int d = 0; d < x->ndims; d++)
	{
		SiftV6XDim *xd = &x->dim[d];
		const Plan *plan = list_nth(custom_plans, d);

		for (int k = 0; k < xd->nkeys; k++)
			xd->kcol[k] = column_position(plan, xd->kcol[k]);
		for (int c = 0; c < xd->npay; c++)
			xd->pcol[c] = column_position(plan, xd->pcol[c]);
	}
}
