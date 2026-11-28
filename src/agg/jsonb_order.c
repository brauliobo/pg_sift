/*
 * agg/jsonb_order.c - where the scan may raise the error of `(doc ->> 'key')::float8` (or `::bool`, `::int4`) for a row whose key holds a text that is no number (no boolean) or a container (the leaf of the key cannot
 * hold it: the scan reads it from the exception of its chunk and converts it as PostgreSQL does). PostgreSQL raises only for a row that reaches the cast: one that the conditions of the WHERE it tests
 * before, which its own plan orders by cost and then by position, keep. The scan decides its scan keys on every row first and converts the others on the rows they keep, so it raises for the same rows
 * only when no other condition of the statement can drop a row after the cast has run; a format that raises only for the rows its batch quals keep (SiftJsonbOps.raise_after_quals) lets those run too, and
 * converts the value of a WHERE clause on the rows its other batch quals keep, which PostgreSQL tests before it, when those raise no error of their own.
 */
/* check-memory: allow-allocs every list here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "agg/plan/int.h"
#include "agg/expr/pg.h"
#include "agg/ext.h"
#include "agg/star/star.h"
#include "agg/unnest/jsonb.h"

static bool
raising_attno(int attno)
{
	return sift_v6_vatt_raises(attno);
}

/* the read of a member of a document by a path (SiftV6Col.jmember), its virtual attribute past the leaves */
static int
member_read(int attno)
{
	return SIFT_V6_JVIRT_SPAN + attno - SIFT_V6_JMEMBER_VATT;
}

/*
 * The read of a column whose conversion raises for some value, as the member of the bitmaps of reads (-1: none): the float8 or bool cast of a key of a document (its leaf), or the cast of a member
 * that the scan reads by a path, which raises for a value that is no number
 */
static int
raising_read(const SiftV6Col *c)
{
	if (c->jmember > 0)
		return member_read(sift_v6_col_input_attno(c));
	if (c->jleaf > 0 && !c->jtype && (c->typid == FLOAT8OID || c->typid == BOOLOID || c->jint))
		return c->jleaf - 1;
	return -1;
}

/* the FILTER of the aggregate is the presence of the one key it reads a value of that raises: a row the FILTER drops has not the key, so no value to convert */
static bool
filter_is_presence(const SiftV6Spec *s, int fclause, const Bitmapset *own)
{
	const List *quals = list_nth(s->filter, fclause - 1);
	const OpExpr *op = list_length(quals) == 1 ? linitial(quals) : NULL;
	const Var  *v = op != NULL && IsA(op, OpExpr) && list_length(op->args) == 2 ? linitial(op->args) : NULL;
	const Const *c = v != NULL ? lsecond(op->args) : NULL;

	return v != NULL && IsA(v, Var) && sift_v6_vatt_is_presence(v->varattno) && IsA(c, Const) && DatumGetBool(c->constvalue) && bms_num_members(own) == 1 &&
		bms_is_member(sift_v6_vatt_leaf(v->varattno), own);
}

/* an expression that evaluates its operands in an order, or not at all, so that a cast under it runs for fewer rows than the scan converts */
static bool
short_circuits(Node *n, void *context)
{
	if (n == NULL)
		return false;
	return IsA(n, CaseExpr) || IsA(n, CoalesceExpr) || IsA(n, BoolExpr) || IsA(n, MinMaxExpr) || expression_tree_walker(n, short_circuits, context);
}

typedef struct Reads
{
	const SiftV6Spec *s;
	Bitmapset  *leaves;
	bool		inputs;			/* an expression of SiftV6Spec.pgexpr, whose Vars are its inputs: the inputs read, not the reads */
} Reads;

static void
note_col(const SiftV6Spec *s, Bitmapset **leaves, const SiftV6Col *c)
{
	const int	read = raising_read(c);

	if (read >= 0)
		*leaves = bms_add_member(*leaves, read);
}

/* a Var of the virtual attributes of the keys of a document or of the columns of the elements that reads a value that raises */
static void
note_var(Reads *r, const Var *v)
{
	if (r->inputs)
		r->leaves = bms_add_member(r->leaves, v->varattno);
	else if (sift_v6_vatt_is_member(v->varattno))
		r->leaves = bms_add_member(r->leaves, member_read(v->varattno));
	else if (raising_attno(v->varattno))
		r->leaves = bms_add_member(r->leaves, sift_v6_vatt_leaf(v->varattno));
	else if (v->varattno >= SIFT_V6_ELEM_VATT && v->varattno < SIFT_V6_ELEM_VATT + SIFT_V6_XELEM_COLS)
		note_col(r->s, &r->leaves, &(SiftV6Col) {.typid = v->vartype,.src = SV6SRC_ELEM,.slot = v->varattno - SIFT_V6_ELEM_VATT});
}

/* the reads that raise of an expression */
static bool
reads_raising_var(Node *n, Reads *r)
{
	if (n != NULL && IsA(n, Var))
		note_var(r, (const Var *) n);
	return n != NULL && !IsA(n, Var) && expression_tree_walker(n, reads_raising_var, r);
}

/* the reads that raise that PostgreSQL evaluates on every row an expression meets: the first operand of an AND, OR or NOT, nothing under an expression that may skip its operands */
static bool
always_reads(Node *n, Reads *r)
{
	if (n == NULL || IsA(n, CaseExpr) || IsA(n, CoalesceExpr) || IsA(n, MinMaxExpr))
		return false;
	if (IsA(n, Var))
		note_var(r, (const Var *) n);
	if (IsA(n, BoolExpr))
		return always_reads(linitial(((const BoolExpr *) n)->args), r);
	return !IsA(n, Var) && expression_tree_walker(n, always_reads, r);
}

static Bitmapset *
expr_leaves(const SiftV6Spec *s, const Expr *e)
{
	Reads		r = {s};

	(void) reads_raising_var((Node *) e, &r);
	return r.leaves;
}

/*
 * The reads that raise of the FILTER clauses: PostgreSQL evaluates the conjuncts of a FILTER in their order and stops at the first that is false, so the scan, which converts a value on every row it
 * keeps, raises as PostgreSQL does only for a read the first conjunct makes on every row (filtered: a clause reads another)
 */
static Bitmapset *
filter_leaves(const SiftV6Spec *s, bool *filtered)
{
	Bitmapset  *leaves = NULL;

	foreach_ptr(List, quals, s->filter)
	{
		Reads		always = {s};
		Bitmapset  *reads = expr_leaves(s, (const Expr *) quals);

		if (quals != NIL)
			(void) always_reads(linitial(quals), &always);
		*filtered |= !bms_is_subset(reads, always.leaves);
		leaves = bms_union(leaves, reads);
	}
	return leaves;
}

/* the read that raises of an input of an SiftV6Spec.pgexpr item (attno, c), -1: none */
static int
input_read(int attno, const SiftV6Col *c)
{
	return sift_v6_vatt_is_member(attno) ? member_read(attno) : raising_attno(attno) ? sift_v6_vatt_leaf(attno) : raising_read(c);
}

/* the reads that raise of a column an aggregate takes: its own, or of a derived one those of what the derivation computes it from */
static void
note_arg(const SiftV6Spec *s, Bitmapset **own, Bitmapset **derived, const SiftV6Col *c)
{
	const SiftV6Derive *d = c->derive > 0 ? &s->derive[c->derive - 1] : NULL;
	SiftV6PgItem it;

	if (d == NULL)
		note_col(s, own, c);
	else if (d->kind != SVD_PG_EXPR)
	{
		note_col(s, derived, &d->src);
		for (int i = 0; i < d->ncond; i++)
			note_col(s, derived, &d->cond[i]);
	}
	else
	{
		sift_v6_pgexpr_item(s, d, &it);
		for (int i = 0; i < it.nin; i++)
			if (input_read(it.col[i].attno, &it.col[i]) >= 0)
				*derived = bms_add_member(*derived, input_read(it.col[i].attno, &it.col[i]));
	}
}

/* the reads that raise the first conjunct of FILTER clause f makes on every row: PostgreSQL meets their error there before the aggregate casts anything */
static Bitmapset *
filter_always(const SiftV6Spec *s, int f)
{
	const List *quals = list_nth(s->filter, f - 1);
	Reads		always = {s};

	if (quals != NIL)
		(void) always_reads(linitial(quals), &always);
	return always.leaves;
}

/* FILTER clause f raises no error of its own: neither the clause nor an expression the scan computes for it (SiftV6Spec.pgexpr) applies a function that may raise */
static bool
filter_quiet(const SiftV6Spec *s, int f)
{
	if (contain_leaked_vars(list_nth(s->filter, f - 1)))
		return false;
	foreach_ptr(List, item, s->pgexpr)
		if (contain_leaked_vars(linitial(item)))
			return false;
	return true;
}

/*
 * The FILTER of aggregate a decides for each row whether PostgreSQL casts its argument, and the scan casts it for every row: it raises where PostgreSQL does not unless the FILTER raises the same
 * error first or keeps only the rows that hold the key, or the format raises the error of a column only for the rows the FILTER of an aggregate that reads it keeps (SiftJsonbOps.raise_after_filter)
 * and the FILTER raises nothing of its own, which the scan would meet before the cast of an earlier row. A derivation computes its value from the column for every row
 */
static bool
filter_misses_raise(const V6Plan *p, const SiftV6Agg *a, const Bitmapset *own, const Bitmapset *derived)
{
	const Bitmapset *always = filter_always(&p->spec, a->fclause);

	if (bms_is_subset(own, always) && bms_is_subset(derived, always))
		return false;
	return derived != NULL || !(filter_is_presence(&p->spec, a->fclause, own) || (p->ops->jsonb->raise_after_filter && p->xseed == NULL && filter_quiet(&p->spec, a->fclause)));
}

/* the leaves the scan converts outside its quals, for the rows it keeps: the keys, the arguments and what is computed from them */
static Bitmapset *
read_leaves(const V6Plan *p, bool *filtered, bool *computed)
{
	const SiftV6Spec *s = &p->spec;
	Bitmapset  *leaves = filter_leaves(s, filtered);

	for (int i = 0; i < s->nkeys; i++)
		note_col(s, &leaves, &s->key[i]);
	for (int a = 0; a < s->naggs; a++)
	{
		Bitmapset  *own = NULL,
				   *derived = NULL;

		note_arg(s, &own, &derived, &s->agg[a].arg);
		note_arg(s, &own, &derived, &s->agg[a].arg2);
		for (int c = 0; c < s->agg[a].nxcols; c++)
			note_arg(s, &own, &derived, &s->agg[a].xcol[c]);
		*filtered |= (own != NULL || derived != NULL) && s->agg[a].fclause != 0 && filter_misses_raise(p, &s->agg[a], own, derived);
		leaves = bms_union(leaves, bms_union(own, derived));
	}
	for (int d = 0; d < s->nderive; d++)
		note_col(s, &leaves, &s->derive[d].src);
	foreach_ptr(List, item, s->pgexpr)
	{
		Reads		always = {s,.inputs = true};

		(void) always_reads(linitial(item), &always);
		for (int i = 1; i < list_length(lsecond(item)); i += SIFT_V6_PGX_INTS)
		{
			const List *ints = lsecond(item);
			const SiftV6Col in = {.typid = (Oid) list_nth_int(ints, i + 1),.src = (SiftV6Src) list_nth_int(ints, i + 3),.slot = list_nth_int(ints, i + 4)};
			const int	read = input_read(list_nth_int(ints, i), &in);

			if (read < 0)
				continue;
			leaves = bms_add_member(leaves, read);
			*computed |= !bms_is_member(1 + (i - 1) / SIFT_V6_PGX_INTS, always.leaves) && short_circuits(linitial(item), NULL);
		}
	}
	return leaves;
}

/* the batch quals other than the one that casts raise no error (their functions are leakproof): the cast's error is the first PostgreSQL raises, whichever row it is */
static bool
others_quiet(const SiftV6Spec *s)
{
	foreach_ptr(Node, q, s->vqual)
		if (expr_leaves(s, (const Expr *) q) == NULL && contain_leaked_vars(q))
			return false;
	return true;
}

/* a join of dimensions that drops fact rows after the scan converted them: an inner, semi, anti or right one, or a condition of the join no key says (a format may raise the cast of a WHERE clause at once, as PostgreSQL tests the quals of the fact table before the join, and hold the errors of the reads of its rows until they reach the groups, SiftJsonbOps.raise_in_groups) */
static bool
join_drops_rows(const SiftV6Ext *x)
{
	bool		drops = x != NULL && x->resid != NIL;

	for (int d = 0; x != NULL && d < x->ndims; d++)
		drops |= x->dim[d].mode != SV6J_LEFT_EQUI && x->dim[d].mode != SV6J_FULL_EQUI;
	return drops;
}

static double
clause_cost(PlannerInfo *root, const RestrictInfo *ri)
{
	QualCost	cost;

	cost_qual_eval_node(&cost, (Node *) ri->clause, root);
	return cost.startup + cost.per_tuple;
}

/* PostgreSQL's own plan of the statement tests every other condition of the WHERE before the clause that casts: it costs less, or as much and is written first */
static bool
tested_before(PlannerInfo *root, const List *clauses, const RestrictInfo *cast)
{
	const double limit = clause_cost(root, cast);
	bool		before = true,
				written = false;

	foreach_node(RestrictInfo, ri, clauses)
	{
		if (ri == cast)
		{
			written = true;
			continue;
		}
		if (!ri->pseudoconstant)
			before &= clause_cost(root, ri) < limit || (clause_cost(root, ri) == limit && !written);
	}
	return before;
}

/*
 * The errors the reads raise: of a key, its leaf; of members, the conversion (the text cast to float8, the cast of the jsonb to the type), which raises the one error for the members alike, as it
 * does for the rows of one key, whichever of them comes first
 */
static Bitmapset *
conversions(const SiftV6Spec *s, const Bitmapset *reads)
{
	Bitmapset  *out = NULL;
	int			r = -1;

	while ((r = bms_next_member(reads, r)) >= 0)
	{
		const SiftV6Col c = r < SIFT_V6_JVIRT_SPAN ? (SiftV6Col) {0} : sift_v6_member_col(s, SIFT_V6_JMEMBER_VATT + r - SIFT_V6_JVIRT_SPAN);

		out = bms_add_member(out, r < SIFT_V6_JVIRT_SPAN ? r : SIFT_V6_JVIRT_SPAN + (c.jsonb_cast ? (int) c.typid : 0));
	}
	return out;
}

/*
 * The scan of the rows raises, for the first row whose casts raise, the error PostgreSQL meets first in it: the cast of the WHERE clause, then a read of the row the clause keeps
 * (sift_f2_v6_raise_kept, SiftJsonbOps.raise_after_quals). A read the clause makes too raises for no row it keeps, so the error is PostgreSQL's when the other reads convert alike; the unnest of a row converts a column at a
 * time, and its reads convert as the clause does
 */
static bool
one_error(const V6Plan *p, const Bitmapset *other, const Bitmapset *cast_reads)
{
	const SiftV6Spec *s = &p->spec;
	const bool	rows_first = p->ops->jsonb->raise_after_quals && (p->xseed == NULL || !p->xseed->unnest);

	return (rows_first && bms_num_members(conversions(s, bms_difference(other, cast_reads))) <= 1) || bms_is_subset(conversions(s, other), conversions(s, cast_reads));
}

bool
sift_v6_json_cast_order_ok(V6Plan *p)
{
	const SiftV6Spec *s = &p->spec;
	bool		filtered = false,
				computed = false;
	Bitmapset  *other = read_leaves(p, &filtered, &computed);
	const RestrictInfo *cast = NULL;
	Bitmapset  *cast_leaves = NULL;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		Bitmapset  *leaves = ri->pseudoconstant ? NULL : expr_leaves(s, sift_v6_plan_json_vars(p, ri->clause, true));

		if (leaves == NULL)
			continue;
		if (cast != NULL)
			return false;
		cast = ri;
		cast_leaves = leaves;
	}
	if (join_drops_rows(p->xseed) && (cast != NULL || other != NULL) && (p->xseed->unnest || !p->ops->jsonb->raise_in_groups))
		return false;
	if (cast == NULL)
		return other == NULL || (!filtered && !computed && (s->vqual == NIL || p->ops->jsonb->raise_after_quals));
	return (list_length(s->vqual) == 1 || (p->ops->jsonb->raise_after_quals && others_quiet(s))) && !short_circuits((Node *) cast->clause, NULL) && !filtered && !computed && p->text == NULL &&
		one_error(p, other, cast_leaves) && tested_before(p->root, p->rel->baserestrictinfo, cast);
}
