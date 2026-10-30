/*
 * agg/from/recheck.c - whether the index answers a test alone
 * Or the rows are checked against the heap: casts of members, text tests and scalar tests.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"

#include "agg/part.h"
#include "agg/from/int.h"

typedef struct MemberCasts
{
	AggCtx	   *c;
	Oid			to;				/* the type the value of the member is cast to */
	bool		mixed;			/* ... it is cast to two types */
	bool		bad;			/* ... or it is read bare, or where the walk does not see (a sub-select), or cast to two types */
} MemberCasts;

static bool
is_member_value(Node *n, AggCtx *c)
{
	return IsA(n, Var) && ((Var *) n)->varno == c->func_rti && ((Var *) n)->varattno == 2;
}

/* the casts of the value column of the jsonb_each of the FROM, in the query and nowhere else: a sub-select could read it where the walk cannot tell */
static bool
member_casts(Node *n, MemberCasts *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE)
				w->bad = true;
		return query_tree_walker(q, member_casts, w, QTW_IGNORE_RT_SUBQUERIES);
	}
	if (IsA(n, SubLink) || IsA(n, SubPlan))
	{
		w->bad = true;
		return false;
	}
	if (IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) != InvalidOid && is_member_value(strip_relabel(linitial(((FuncExpr *) n)->args)), w->c))
	{
		Oid			to = json_cast_target(((FuncExpr *) n)->funcid);

		if (w->to != InvalidOid && w->to != to)
			w->mixed = w->bad = true;
		w->to = to;
		return false;
	}
	if (is_member_value(n, w->c))
		w->bad = true;
	return expression_tree_walker(n, member_casts, w);
}

/*
 * Can leaf m, read only through the cast of a jsonb_each member's value to float8, be read again from the heap, so that a row with a value of another
 * type raises where PostgreSQL's cast does? The query casts the value to that one type: the error names the type of the cast that raises first, in
 * the order the aggregates evaluate them, which the row read again does not know (the message of another cast would be float8's).
 */
static bool
cast_recheck_ok(AggCtx *c, int m)
{
	Query	   *q = c->root->parse;
	MemberCasts w = {c, InvalidOid, false, false};
	Unconditional u = {c, -1};

	c->cast_want = FLOAT8OID;
	/* a leaf of the index, not the member of one key (a virtual leaf): the source reads the rows with a bad value in any member again, and the error is the row's, not the key's */
	return c->func_kind == FK_EACH && m < c->lay->nleaves - c->nvleaf && leaf_of(c, m)->parent >= 0 && bms_is_member(m, c->cast_ok) && !bms_is_member(m, c->cast_bad) &&
		leaf_of(c, m)->typid == FLOAT8OID && !c->search && OidIsValid(c->indexoid) && sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true) &&
		(unconditional((Node *) q->targetList, &u) || unconditional(q->havingQual, &u)) &&
		!member_casts((Node *) q, &w) && !w.mixed && w.to == FLOAT8OID;
}

/*
 * Can the member of one key (vleaf j: `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`) be read again from the heap, so that a row whose member of
 * that key holds a value that is no number raises where the cast of PostgreSQL does, and a dirty member of another key raises nothing? The
 * member is read only through casts to one type that every row reaching the aggregates evaluates, no conjunct reads it, and no other read of the
 * value column of the object (all the members) shares the rows read again.
 */
Oid
member_recheck(AggCtx *c, const SiftAggSpec *s, int j)
{
	int			v = c->lay->nleaves - c->nvleaf + j,
				src = vleaf_source(c, j);
	Query	   *q = c->root->parse;
	MemberCasts w = {c, InvalidOid, false, false};
	Unconditional u = {c, -1};

	if (!sift_aggvleaf_member(&c->vleaf[j]) || src < 0 || c->func_kind != FK_EACH || c->func_key == NULL || c->vleaf[j].obj != c->func_obj ||
		bms_is_member(src, c->used) || leaf_of(c, v)->typid != FLOAT8OID || c->search || !OidIsValid(c->indexoid) ||
		!sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return InvalidOid;
	/* the integer casts start from the float8 member (jsonb_int_mutator): the message of the error names the type the query casts to */
	if (member_casts((Node *) q, &w) || w.bad || (w.to != FLOAT8OID && w.to != INT4OID && w.to != INT8OID))
		return InvalidOid;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level != 0 || pred_reads_leaf(c, s->pred[i], v))
			return InvalidOid;
	c->cast_want = w.to;
	if (!unconditional((Node *) q->targetList, &u) && !unconditional(q->havingQual, &u))
		return InvalidOid;
	return w.to;
}

/* can leaf m, a text path read only as `doc ->> 'k'`, be read again from the heap? The text of a value cannot raise: a container where a scalar is, a document of another kind than an object (a key of an array or scalar is NULL) */
static bool
text_recheck_ok(AggCtx *c, int m)
{
	const SiftSrcLeaf *l = leaf_of(c, m);

	return m < c->lay->nleaves - c->nvleaf && bms_is_member(m, c->txt_ok) && !bms_is_member(m, c->txt_bad) && l->kind == SCL_SCALAR && l->parent < 0 &&
		l->path != NIL && l->typid == TEXTOID && !l->wide && !c->search && OidIsValid(c->indexoid) && sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true);
}

/* the list leaf m is, or is a column of; -1 when it is neither */
static int
recordset_group(AggCtx *c, int m)
{
	return bms_is_member(m, recordset_groups(c)) ? m : leaf_of(c, m)->parent;
}

/* How the query reads leaf m when the source holds a value it cannot store (SRK_NONE: it does not, such a value is an error for the scan) */
static SiftSrcRecheck
leaf_recheck(AggCtx *c, int m)
{
	int			grp;

	if (cast_recheck_ok(c, m))
		return SRK_CAST;
	if (text_recheck_ok(c, m))
		return SRK_TEXT;
	grp = recordset_group(c, m);
	if (grp < 0 || !recordset_recheck_ok(c, grp))
		return SRK_NONE;
	if (m == grp)
		return SRK_RECORDSET;
	return bms_is_member(m, c->used) || bms_is_member(m, recordset_columns(c, grp)) ? SRK_TEXT : SRK_NONE;
}

/* a comparison, IN list or NULL test of the members' leaf alone: PostgreSQL casts the value of every member that reaches it */
static bool
leaf_test(const SiftAggPred *p, int leaf)
{
	return p->level == 1 && !p->late && p->nchild == 0 && p->leaf == leaf && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NULL);
}

/* the types of a scalar path whose rows are read again through the input function of the type (the narrower float and integer types are not tested with the read) */
static bool
scalar_text_type(Oid typid)
{
	return typid == BOOLOID || typid == INT4OID || typid == INT8OID || typid == FLOAT8OID;
}

/* a comparison, IN list, NULL test or boolean expression of the leaf alone, over the rows, that cannot raise by itself: a row the leaf cannot read passes it (pass_poison) */
static bool
scalar_test(const SiftAggPred *p, int leaf)
{
	return p->level == 0 && !p->late && p->nchild == 0 && p->leaf == leaf && p->leaf2 < 0 && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NULL || p->kind == SAP_LUT);
}

/*
 * Can leaf m, a number or boolean path of the document that the query reads only as `(doc ->> 'k')::T` (SRK_IOCAST) or, for float8, as the
 * cast of `doc -> 'k'` (SRK_CAST), be read again from the heap? A row whose value the leaf cannot store is read as SQL reads it: the text of the
 * scalar through the input function of T, or the number of the jsonb value, and the error SQL raises for another value is raised where the row
 * reaches the aggregate (recheck_late), which is where PostgreSQL evaluates the argument of an aggregate or a grouping expression. So every
 * read of the leaf is that one cast, in a key or an aggregate that every row reaching it evaluates, and nothing the rows go through before the
 * aggregates may drop a row after PostgreSQL has evaluated the cast, which a function in the FROM or a join could.
 *
 * A condition of the WHERE may read it too, as a comparison, IN list or NULL test of the cast: a row of an unreadable value passes it and goes on
 * to the other conditions, and raises only if they keep it. That is what PostgreSQL does when it tests the others first (others_first); a plan
 * that tests the cast before a condition that drops the row raises for it. A condition that may raise itself (late) is left to the normal plan.
 * The rows the condition keeps have a readable value, so the aggregates may read the leaf in any way.
 */
static SiftSrcRecheck
scalar_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	const SiftSrcLeaf *l;
	Query	   *q = c->root->parse;
	Unconditional u = {c, m};
	Unconditional all;
	SiftSrcRecheck kind;
	Bitmapset  *read;
	bool		tested = false,
				filtered = false;
	uint64		gate = 0;

	if (m >= c->lay->nleaves - c->nvleaf || (c->group >= 0) != (c->func_kind == FK_ELEMENTS) || (c->func_rti != 0) != (c->group >= 0) || c->joins != NIL ||
		c->search || !OidIsValid(c->indexoid) || !sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return SRK_NONE;
	l = leaf_of(c, m);
	if (l->kind != SCL_SCALAR || l->parent != c->group || (c->group < 0 && l->path == NIL) || l->wide)
		return SRK_NONE;
	if (bms_is_member(m, c->cast_ok) && !bms_is_member(m, c->cast_bad) && l->typid == FLOAT8OID)
		kind = SRK_CAST;
	else if (bms_is_member(m, c->io_ok) && !bms_is_member(m, c->io_bad) && scalar_text_type(l->typid))
	{
		kind = SRK_IOCAST;
		u.as_text = true;
	}
	else
		return SRK_NONE;
	/* the error of an element is kept for its row, not for the element: a condition on the elements may drop the one that raises and keep another */
	for (int i = 0; c->group >= 0 && i < s->npred; i++)
		if (s->pred[i]->level != 0)
		{
			c->dirty_leaf = m;
			c->dirty_why = "a condition on the elements of the list may drop the element whose value raises: the error of an element is kept for its row";
			return SRK_NONE;
		}
	read = bms_make_singleton(m);
	for (int i = 0; i < s->npred; i++)
		if (pred_reads_set(c, s->pred[i], read))
		{
			if (!scalar_test(s->pred[i], m))
			{
				c->dirty_leaf = m;
				c->dirty_why = "a condition on the cast is none of a comparison with a constant, an IN list of integers and a NULL test (an OR, an expression of it, a boolean)";
				return SRK_NONE;
			}
			tested = true;
		}
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].filter != NULL && pred_reads_set(c, s->agg[j].filter, read))
		{
			if (!scalar_test(s->agg[j].filter, m))
			{
				c->dirty_leaf = m;
				c->dirty_why = "the FILTER of an aggregate tests the cast in a way that is none of a comparison with a constant, an IN list of integers and a NULL test";
				return SRK_NONE;
			}
			filtered = true;
		}
	if (tested)
	{
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->late)
				return order_refused(c, m);
		if (!others_first(c, &u))
			return order_refused(c, m);
	}
	else if (!unconditional((Node *) q->targetList, &u) && !unconditional(q->havingQual, &u))
	{
		/* not every row evaluates it: the rows that the FILTER of every aggregate that reads it keeps do */
		gate = gate_mask(c, s, m);
		if (gate == 0)
		{
			c->dirty_leaf = m;
			c->dirty_why = "the cast runs for some rows only (a CASE branch), or in an aggregate whose FILTER may raise, reads another path that holds a bad value or is missing";
			return SRK_NONE;
		}
	}
	/* every read is in the conditions and the targets (in the arguments of the aggregates with a FILTER when it is gated): the one the walk found is in the same tree as the others */
	all = u;
	all.nread = 0;
	count_leaf_reads((Node *) q, &all);
	u.nread = 0;
	if (gate)
	{
		filtered_reads((Node *) q->targetList, &u);
		filtered_reads(q->havingQual, &u);
	}
	else
	{
		count_leaf_reads((Node *) q->targetList, &u);
		count_leaf_reads(q->havingQual, &u);
		count_leaf_reads(q->jointree->quals, &u);
	}
	if (all.nread != u.nread)
		return SRK_NONE;
	for (int i = 0; gate && i < s->nleaves; i++)
		if (s->leaves[i] == m)
			s->gate[i] = gate;
	for (int i = 0; i < s->npred; i++)
		if (pred_reads_set(c, s->pred[i], read))
			s->pred[i]->pass_poison = true;
	for (int j = 0; filtered && j < s->naggs; j++)
		if (s->agg[j].filter != NULL && pred_reads_set(c, s->agg[j].filter, read))
			s->agg[j].filter->pass_poison = true;
	return kind;
}

/* SRK_NONE unless leaf m is read in a way the source can read again, and no conjunct of the rows reads the unnested list or its columns (it may drop a row that the error would reach first) */
SiftSrcRecheck
spec_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	Bitmapset  *list = NULL;
	int			grp;
	SiftSrcRecheck scalar = scalar_recheck(c, s, m);

	if (scalar != SRK_NONE)
		return scalar;
	if (leaf_recheck(c, m) == SRK_NONE)
		return SRK_NONE;
	/*
	 * the row raises where it reaches the aggregate, none of the members dropped before it. A conjunct that reads the member of a key taken from the
	 * leaf (the cast of the conjunct would raise before the aggregates, with the message of its own type, which the row read again does not know) or
	 * anything but the leaf's own value keeps the normal plan. A conjunct that tests the leaf alone casts every member of a row that reaches the
	 * members, with the cast of the aggregates, and no other test of the members may drop one first: the cast raises before the test (SRK_CAST_TESTED).
	 * Where it runs relative to a join or a conjunct that may raise is the planner's choice, so none of them is in the query then.
	 */
	if (leaf_recheck(c, m) == SRK_CAST)
	{
		Bitmapset  *read = bms_make_singleton(m);
		bool		tested = false;

		for (int j = 0; j < c->nvleaf; j++)
			if (vleaf_source(c, j) == m)
				read = bms_add_member(read, c->lay->nleaves - c->nvleaf + j);
		for (int i = 0; i < s->npred; i++)
			if (pred_reads_set(c, s->pred[i], read))
			{
				if (!leaf_test(s->pred[i], m))
					return SRK_NONE;
				tested = true;
			}
			else if (s->pred[i]->level != 0)
				return SRK_NONE;
		if (!tested)
			return SRK_CAST;
		if (c->joins != NIL)
			return SRK_NONE;
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->late)
				return SRK_NONE;
		return SRK_CAST_TESTED;
	}
	/* no error to raise, so no order to keep: the rows read again give the text any conjunct and key reads */
	if (leaf_of(c, m)->parent < 0 && leaf_recheck(c, m) == SRK_TEXT)
		return SRK_TEXT;
	grp = recordset_group(c, m);
	list = bms_add_member(list, grp);
	for (int i = 0; i < c->lay->nleaves; i++)
		if (leaf_of(c, i)->parent == grp)
			list = bms_add_member(list, i);
	/* an EXISTS over the list passes the row that raises: the other conjuncts decide whether it reaches the error */
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 0 && pred_reads_any(s->pred[i], list) && !(s->pred[i]->pass_poison && s->pred[i]->leaf == grp))
			return SRK_NONE;
	if (m == grp && grp != c->group && list_read_by_tuple_tests(s, grp))
		return SRK_RECORDSET_LAZY;
	return leaf_recheck(c, m);
}
