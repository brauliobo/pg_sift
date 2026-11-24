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

#include "agg/group/ht.h"
#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_bqual.h"

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

static bool
is_text_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
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
text_bytes(const Const *c, const uint8 **bytes, uint32 *len)
{
	const text *t = DatumGetTextPP(c->constvalue);

	*len = VARSIZE_ANY_EXHDR(t);
	*bytes = memcpy(palloc(Max(*len, 1)), VARDATA_ANY(t), *len);
	return true;
}

static bool
cmp_qual(const Const *c, ParQualOp op, Oid collid, ParQual *q)
{
	if (c->consttype != TEXTOID || (op != PQO_EQ && op != PQO_NE) || !sift_text_bytes_equal(collid))
		return false;
	q->kind = PQK_STR_CMP;
	q->op = op;
	return text_bytes(c, &q->bytes, &q->len);
}

static bool
like_qual(const Const *c, bool negate, Oid collid, ParQual *q)
{
	const uint8 *pat;
	uint32		plen;
	uint8	   *lit;

	if (c->consttype != TEXTOID || !sift_text_bytes_equal(collid) || !text_bytes(c, &pat, &plen))
		return false;
	lit = palloc(Max(plen, 1));
	q->like = sift_like_parse(pat, plen, lit, &q->len);
	q->kind = PQK_STR_LIKE;
	q->negate = negate;
	q->bytes = lit;
	if (q->like == SIFT_LIKE_CONTAINS)
		sift_like_needle_borrow(&q->needle, lit, q->len, false);
	return q->like != SIFT_LIKE_OTHER;
}

static bool
one_qual(Node *clause, const int *first, ParShape *sh)
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
	if (is_int_type(((const Var *) l)->vartype) || is_time_type(((const Var *) l)->vartype))
	{
		if (!op_of(name, &cmp) || !int_qual((const Var *) l, (const Const *) r, swapped ? commuted(cmp) : cmp, q))
			return false;
	}
	else if (is_text_type(((const Var *) l)->vartype) && !swapped)
	{
		if (op_of(name, &cmp) ? !cmp_qual((const Const *) r, cmp, op->inputcollid, q) : !((strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0) && like_qual((const Const *) r, name[0] == '!', op->inputcollid, q)))
			return false;
	}
	else
		return false;
	q->at = par_add_leaf(sh, first != NULL ? first[((const Var *) l)->varattno] : 0, is_text_type(((const Var *) l)->vartype));
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
like_holds(const ParQual *q, const SiftStrRef s)
{
	bool		hit;

	switch (q->like)
	{
		case SIFT_LIKE_CONTAINS:
			hit = sift_like_test(&q->needle, s.p, s.len) == SIFT_LIKE_YES;
			break;
		case SIFT_LIKE_PREFIX:
			hit = s.len >= q->len && memcmp(s.p, q->bytes, q->len) == 0;
			break;
		case SIFT_LIKE_SUFFIX:
			hit = s.len >= q->len && memcmp(s.p + s.len - q->len, q->bytes, q->len) == 0;
			break;
		default:
			hit = s.len == q->len && memcmp(s.p, q->bytes, q->len) == 0;
			break;
	}
	return hit != q->negate;
}

static bool
str_holds(const ParQual *q, const SiftStrRef s)
{
	if (q->kind == PQK_STR_LIKE)
		return like_holds(q, s);
	return (s.len == q->len && memcmp(s.p, q->bytes, q->len) == 0) == (q->op == PQO_EQ);
}

/* a dictionary entry is judged the first time a row holds it */
static bool
code_holds(const ParQual *q, const ParLoad *ld, const ColView *cv, uint32 r, uint8 *memo)
{
	const uint32 code = cv->dcode[r];

	if (memo[code] == 0)
		memo[code] = 1 + str_holds(q, par_str_at(ld, cv, r));
	return memo[code] == 2;
}

static bool
row_holds(const ParQual *q, const ParLoad *ld, const ColView *cv, uint32 r, uint8 *memo)
{
	if (cv->isnull[r])
		return false;
	if (q->kind == PQK_INT)
		return int_holds(q, cv->i[r]);
	return memo != NULL ? code_holds(q, ld, cv, r, memo) : str_holds(q, par_str_at(ld, cv, r));
}

bool
par_quals_apply(const ParShape *sh, ParLoad *ld, SiftArena *scratch, SiftErr *err)
{
	for (int k = 0; k < sh->nq && ld->nact > 0; k++)
	{
		const ParQual *q = &sh->q[k];
		const ColView *cv = &ld->cv[q->at];
		uint8	   *memo = NULL;

		if (q->kind != PQK_INT && cv->dcode != NULL)
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
