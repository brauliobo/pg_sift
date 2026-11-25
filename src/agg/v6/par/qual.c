/*
 * agg/v6/par/qual.c - the quals of a statement that no scan key says, on the compute threads (agg/v6/par/qual.h): which of them a thread can run (par_quals_of, in the leader) and the rows of a granule
 * they leave (par_quals_apply, in a thread). A text held by a dictionary is judged once for each entry.
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"

#include "agg/v6/par/run_int.h"
#include "agg/v6/qual/bqual.h"

static bool
op_of(const char *name, ParQualOp *op)
{
	static const struct
	{
		const char *name;
		ParQualOp	op;
	}			ops[] = {{"=", PQO_EQ}, {"<>", PQO_NE}, {"<", PQO_LT}, {"<=", PQO_LE}, {">", PQO_GT}, {">=", PQO_GE}};

	for (int i = 0; i < lengthof(ops); i++)
		if (strcmp(name, ops[i].name) == 0)
		{
			*op = ops[i].op;
			return true;
		}
	return false;
}

/* the operator with its operands swapped: a constant on the left */
static ParQualOp
commuted(ParQualOp op)
{
	return op == PQO_LT ? PQO_GT : op == PQO_LE ? PQO_GE : op == PQO_GT ? PQO_LT : op == PQO_GE ? PQO_LE : op;
}

static Node *
unrelabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static bool
is_int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool
is_time_type(Oid t)
{
	return t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static int64
int_of(const Const *c)
{
	switch (c->consttype)
	{
		case INT2OID:
			return DatumGetInt16(c->constvalue);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(c->constvalue);
		default:
			return DatumGetInt64(c->constvalue);
	}
}

static bool
int_qual(const Var *v, const Const *c, ParQualOp op, ParQual *q)
{
	if (!(is_int_type(v->vartype) ? is_int_type(c->consttype) : is_time_type(v->vartype) && c->consttype == v->vartype))
		return false;
	q->kind = PQK_INT;
	q->op = op;
	q->ival = int_of(c);
	return true;
}

static bool
int_clause(Node *clause, const int *first, ParShape *sh)
{
	const OpExpr *op;
	Node	   *l,
			   *r;
	const char *name;
	ParQualOp	cmp = PQO_EQ;
	ParQual	   *q = &sh->q[sh->nq];
	bool		swapped = false;

	if (!IsA(clause, OpExpr) || list_length((op = (const OpExpr *) clause)->args) != 2 || get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE ||
		(name = get_opname(op->opno)) == NULL)
		return false;
	l = unrelabel(linitial(op->args));
	r = unrelabel(lsecond(op->args));
	if (l != NULL && IsA(l, Const))
	{
		Node	   *t = l;

		l = r;
		r = t;
		swapped = true;
	}
	if (l == NULL || r == NULL || !IsA(l, Var) || !IsA(r, Const) || ((const Var *) l)->varno != SIFT_V6_BQ_RELID || ((const Var *) l)->varattno <= 0 || ((const Const *) r)->constisnull)
		return false;
	if (!(is_int_type(((const Var *) l)->vartype) || is_time_type(((const Var *) l)->vartype)) || !op_of(name, &cmp) ||
		!int_qual((const Var *) l, (const Const *) r, swapped ? commuted(cmp) : cmp, q))
		return false;
	q->at = par_add_leaf(sh, first != NULL ? first[((const Var *) l)->varattno] : 0, false);
	sh->nq++;
	return true;
}

static bool
one_qual(Node *clause, const int *first, ParShape *sh)
{
	ParQual    *q = &sh->q[sh->nq];
	const Var  *v;

	if (!sift_text_qual_of(clause, SIFT_V6_BQ_RELID, &q->text, &v))
		return int_clause(clause, first, sh);
	q->kind = PQK_TEXT;
	q->at = par_add_leaf(sh, first != NULL ? first[v->varattno] : 0, true);
	sh->nq++;
	return true;
}

bool
par_quals_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (sp->vqual == NIL)
		return true;
	if (!sp->is_table || list_length(sp->vqual) > PAR_QUALS_MAX)
		return false;
	foreach_ptr(Node, clause, sp->vqual)
		if (sh->nleaves >= PAR_LEAVES_MAX || !one_qual(clause, first, sh))
			return false;
	return true;
}

static bool
int_holds(const ParQual *q, int64 v)
{
	switch (q->op)
	{
		case PQO_EQ:
			return v == q->ival;
		case PQO_NE:
			return v != q->ival;
		case PQO_LT:
			return v < q->ival;
		case PQO_LE:
			return v <= q->ival;
		case PQO_GT:
			return v > q->ival;
		default:
			return v >= q->ival;
	}
}

static bool
str_holds(const ParQual *q, const ParLoad *ld, const ColView *cv, uint32 r)
{
	const SiftStrRef s = par_str_at(ld, cv, r);

	return sift_text_qual_holds(&q->text, s.p, s.len);
}

/* a dictionary entry is judged the first time a row holds it */
static bool
code_holds(const ParQual *q, const ParLoad *ld, const ColView *cv, uint32 r, uint8 *memo)
{
	const uint32 code = cv->dcode[r];

	if (memo[code] == 0)
		memo[code] = 1 + str_holds(q, ld, cv, r);
	return memo[code] == 2;
}

static bool
row_holds(const ParQual *q, const ParLoad *ld, const ColView *cv, uint32 r, uint8 *memo)
{
	if (cv->isnull[r])
		return false;
	if (q->kind == PQK_INT)
		return int_holds(q, cv->i[r]);
	return memo != NULL ? code_holds(q, ld, cv, r, memo) : str_holds(q, ld, cv, r);
}

bool
par_quals_apply(const ParShape *sh, ParLoad *ld, SiftArena *scratch, SiftErr *err)
{
	for (int k = 0; k < sh->nq && ld->nact > 0; k++)
	{
		const ParQual *q = &sh->q[k];
		const ColView *cv = &ld->cv[q->at];
		uint8	   *memo = NULL;

		if (q->kind == PQK_TEXT && cv->dcode != NULL)
		{
			if ((memo = sift_arena_alloc(scratch, cv->dict->n, 1)) == NULL)
				return par_arena_full(scratch, err);
			memset(memo, 0, cv->dict->n);
		}
		ld->nact = 0;
		for (uint32 w = 0; w < (ld->n + 63) / 64; w++)
		{
			for (uint64 bits = ld->active[w]; bits != 0; bits &= bits - 1)
			{
				const uint32 r = w * 64 + __builtin_ctzll(bits);

				if (cv->isnull[r] && cv->tag != NULL && cv->tag[r] != SIFT_COLTAG_UNKNOWN)
					return par_serial(err, "a value the leaf holds out of line in a qual");
				if (!row_holds(q, ld, cv, r, memo))
					ld->active[w] &= ~(UINT64CONST(1) << (r % 64));
			}
			ld->nact += pg_popcount64(ld->active[w]);
		}
	}
	return true;
}
