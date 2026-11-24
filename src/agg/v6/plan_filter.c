/*
 * agg/v6/plan_filter.c - the FILTER (WHERE ..) clause of an aggregate of SiftAggV6: the scan decides it on the stored values, or PostgreSQL's own functions decide it over the datums of its columns.
 */
/* check-memory: allow-allocs every list here follows the clauses of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_qual.h"
#include "agg/v6/v6_unnest_jsonb.h"

/* a conjunct of a FILTER: over the columns of the element, or the members the statement reads by key, as a qual of the rows of the scan; any other over the columns of the index */
static Expr *
filter_qual(V6Plan *p, Expr *c)
{
	const bool	on_elem = p->elem_rel != NULL && bms_is_member(p->elem_rel->relid, pull_varnos(p->root, (Node *) c));
	Expr	   *q = on_elem || sift_v6_mapkey_on(p) ? sift_v6_unnest_elem_qual(p, (Node *) c) : NULL;

	return q != NULL || on_elem ? q : sift_v6_index_qual(p, sift_v6_plan_type_vars(p, c));
}

/* the conjuncts of the clause the scan decides on the stored values (NIL: one is not of those) */
static List *
native_filter_quals(V6Plan *p, Expr *filter)
{
	List	   *quals = NIL;

	p->in_filter = true;
	foreach_ptr(Expr, c, make_ands_implicit(filter))
	{
		Expr	   *q = filter_qual(p, c);

		if (q == NULL)
		{
			quals = NIL;
			break;
		}
		quals = lappend(quals, q);
	}
	p->in_filter = false;
	return quals;
}

/* the clause as a boolean PostgreSQL's own functions compute over the datums of its columns: the test that the derived column is true */
static List *
derived_filter_quals(V6Plan *p, Expr *filter)
{
	SiftV6Col	col;
	BooleanTest *bt;
	int			i = 0;

	if (exprType((Node *) filter) != BOOLOID || !sift_v6_pg_derived_col(p, filter, false, &col))
		return NIL;
	while (i < p->spec.nfcols && p->spec.fcol[i].derive != col.derive)
		i++;
	if (i == p->spec.nfcols && p->spec.nfcols == SIFT_V6_MAX_FCOLS)
		return NIL;
	p->spec.fcol[i] = col;
	p->spec.nfcols = Max(p->spec.nfcols, i + 1);
	bt = makeNode(BooleanTest);
	bt->arg = (Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(col.derive), false, true);
	bt->booltesttype = IS_TRUE;
	return list_make1(bt);
}

/* FILTER (WHERE ..): a conjunction of the quals of a scan, over the columns of the index; the same clause of two aggregates is one clause (at most 64 of them: a bit of the mask of a row each) */
bool
sift_v6_compile_filter(V6Plan *p, Expr *filter, int *fclause)
{
	List	   *quals = native_filter_quals(p, filter);
	SiftClause *clause = palloc(sizeof(SiftClause));
	SiftBytes	canon;
	int			id;

	sift_bytes_init(&canon, CurrentMemoryContext);
	if (quals == NIL || !sift_v6_clause_make(CurrentMemoryContext, quals, &p->spec, clause, &canon))
	{
		sift_bytes_init(&canon, CurrentMemoryContext);
		quals = derived_filter_quals(p, filter);
		if (quals == NIL || !sift_v6_clause_make(CurrentMemoryContext, quals, &p->spec, clause, &canon))
			return false;
	}
	if ((id = sift_freg_add(p->freg, canon.buf, canon.len)) < 0)
		return false;
	if (id == p->spec.nfilters)
	{
		p->spec.filter = lappend(p->spec.filter, quals);
		p->spec.nfilters++;
	}
	*fclause = id + 1;
	return true;
}
