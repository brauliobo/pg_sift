/*
 * agg/spec.c - the SiftAggSpec as a flat List: Integer, String and Node items in the order the unpacker reads them.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/date.h"
#include "nodes/value.h"
#include "storage/itemptr.h"

#include "agg/spec.h"

sift_i128
sift_pow10(int e)
{
	sift_i128	r = 1;

	while (e-- > 0)
		r *= 10;
	return r;
}

bool
sift_parse_decimal(const char *s, sift_i128 *v, int *scale)
{
	bool		neg = *s == '-';
	int			nd = 0,
				frac = -1;
	sift_i128	r = 0;

	s += neg;
	for (; *s; s++)
	{
		if (*s == '.' && frac < 0)
			frac = 0;
		else if (*s >= '0' && *s <= '9')
		{
			if (r != 0 || *s != '0')
				nd++;
			r = r * 10 + (*s - '0');
			frac += frac >= 0;
		}
		else
			return false;
		if (nd > SIFT_NUM_MAX_DIGITS)
			return false;
	}
	*v = neg ? -r : r;
	*scale = Max(frac, 0);
	return true;
}

/* an integer-like operand as the int64 the kernels compare */
int64
sift_aggspec_operand_int(Oid t, Datum d)
{
	switch (t)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return DatumGetInt64(d);
		case DATEOID:
			return DatumGetDateADT(d);
		case BOOLOID:
			return DatumGetBool(d);
		default:
			SIFT_INVARIANT(false, "sift: unexpected operand type %u", t);
	}
	return 0;
}

/* the types sift_aggspec_operand_int reads */
bool
sift_aggspec_integer_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID || t == BOOLOID;
}

/* can a unit's zone refuse p: a comparison (not <>) of an integer-valued leaf with a constant, a numeric leaf as its scaled integer, an IN list */
bool
sift_aggspec_zone_cmp(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	const SiftSrcLeaf *lf;
	bool		num;

	if ((p->kind != SAP_CMP && p->kind != SAP_IN) || p->leaf >= nbase || (p->kind == SAP_CMP && p->op == SCMP_NE) || (p->kind == SAP_IN && p->op))
		return false;
	lf = &lay->leaf[p->leaf];
	num = lf->typid == NUMERICOID && lf->vtype == SCV_I64 && p->argtype == NUMERICOID;
	return lf->parent < 0 && !lf->lossy && (num || (sift_aggspec_integer_type(p->argtype) && sift_aggspec_integer_type(lf->typid)));
}

/* the conjuncts a unit's zone can refuse */
static List *
zone_cmps(List *out, const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			out = zone_cmps(out, p->child[i], lay, nbase);
	return sift_aggspec_zone_cmp(p, lay, nbase) ? lappend(out, (void *) p) : out;
}

List *
sift_aggspec_zone_cmps(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase)
{
	List	   *out = NIL;

	for (int i = 0; i < s->npred; i++)
		out = zone_cmps(out, s->pred[i], lay, nbase);
	return out;
}

static uint64
tid_key(const ItemPointerData *t)
{
	return (uint64) ItemPointerGetBlockNumberNoCheck(t) << 16 | ItemPointerGetOffsetNumberNoCheck(t);
}

bool
sift_aggspec_tid_range(const SiftAggSpec *s, uint64 *lo, uint64 *hi)
{
	bool		any = false;

	*lo = 0;
	*hi = SIFT_TID_KEY_END;
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		uint64		k;

		if (p->kind != SAP_TID)
			continue;
		any = true;
		k = tid_key(DatumGetItemPointer(((Const *) p->expr)->constvalue));
		if (p->op == SCMP_EQ || p->op == SCMP_GE || p->op == SCMP_GT)
			*lo = Max(*lo, p->op == SCMP_GT ? k + 1 : k);
		if (p->op == SCMP_EQ || p->op == SCMP_LE || p->op == SCMP_LT)
			*hi = Min(*hi, p->op == SCMP_LT ? k : k + 1);
	}
	return any;
}

static Node *
strip_relabel(Node *n)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static bool
text_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
}

/* a text constant that is not NULL */
static bool
str_const(Node *n, const char **s, uint32 *len)
{
	Const	   *k = (Const *) strip_relabel(n);
	text	   *t;

	if (!IsA(k, Const) || k->constisnull || !text_type(k->consttype))
		return false;
	t = DatumGetTextPP(k->constvalue);
	*s = VARDATA_ANY(t);
	*len = VARSIZE_ANY_EXHDR(t);
	return true;
}

static SiftSrcStrOp
str_commute(SiftSrcStrOp op)
{
	return op == SSTR_LT ? SSTR_GT : op == SSTR_LE ? SSTR_GE : op == SSTR_GT ? SSTR_LT : op == SSTR_GE ? SSTR_LE : op;
}

/* the comparison a text function is, or 0; the range operators and LIKE only compare bytes in the C collation or for a prefix */
static SiftSrcStrOp
str_func_op(Oid funcid)
{
	switch (funcid)
	{
		case F_TEXTEQ:
			return SSTR_EQ;
		case F_TEXTNE:
			return SSTR_NE;
		case F_TEXT_LT:
			return SSTR_LT;
		case F_TEXT_LE:
			return SSTR_LE;
		case F_TEXT_GT:
			return SSTR_GT;
		case F_TEXT_GE:
			return SSTR_GE;
		case F_TEXTLIKE:
			return SSTR_PREFIX;
	}
	return 0;
}

/* leaf <op> constant, constant <op> leaf, leaf LIKE 'prefix%' */
static bool
str_cmp_test(OpExpr *op, SiftSrcStrTest *out)
{
	Node	   *a,
			   *b;
	SiftSrcStrOp sop;
	const char *s;
	uint32		len;

	set_opfuncid(op);
	if (list_length(op->args) != 2 || (sop = str_func_op(op->opfuncid)) == 0)
		return false;
	a = strip_relabel(linitial(op->args));
	b = strip_relabel(lsecond(op->args));
	if (IsA(b, Param) && sop != SSTR_PREFIX)
	{
		Node	   *t = a;

		a = b;
		b = t;
		sop = str_commute(sop);
	}
	if (!IsA(a, Param) || !str_const(b, &s, &len))
		return false;
	if (sop == SSTR_PREFIX)
	{
		/* 'abc%': a LIKE with no other wildcard is bytewise for every deterministic collation */
		if (len < 2 || s[len - 1] != '%' || memchr(s, '%', len - 1) || memchr(s, '_', len - 1) || memchr(s, '\\', len - 1))
			return false;
		len--;
	}
	if (sop >= SSTR_LT && sop <= SSTR_GE ? op->inputcollid != C_COLLATION_OID : !get_collation_isdeterministic(op->inputcollid))
		return false;
	out->op = sop;
	out->nvals = 1;
	out->vals = palloc(sizeof(char *));
	out->lens = palloc(sizeof(uint32));
	((const char **) out->vals)[0] = s;
	((uint32 *) out->lens)[0] = len;
	return true;
}

/* leaf = ANY (constants), leaf <> ALL (constants); a NULL among the constants never matches, and makes NOT IN false for every row */
static bool
str_in_test(ScalarArrayOpExpr *sa, SiftSrcStrTest *out)
{
	Const	   *k;
	Datum	   *el;
	bool	   *nl;
	int			ne,
				n = 0;
	const char **vals;
	uint32	   *lens;

	set_sa_opfuncid(sa);
	if (list_length(sa->args) != 2 || !IsA(strip_relabel(linitial(sa->args)), Param) ||
		!((sa->useOr && sa->opfuncid == F_TEXTEQ) || (!sa->useOr && sa->opfuncid == F_TEXTNE)) ||
		!get_collation_isdeterministic(sa->inputcollid))
		return false;
	k = (Const *) lsecond(sa->args);
	if (!IsA(k, Const) || k->constisnull || !text_type(get_element_type(k->consttype)))
		return false;
	deconstruct_array(DatumGetArrayTypeP(k->constvalue), get_element_type(k->consttype), -1, false, TYPALIGN_INT, &el, &nl, &ne);
	vals = palloc(Max(ne, 1) * sizeof(char *));
	lens = palloc(Max(ne, 1) * sizeof(uint32));
	for (int i = 0; i < ne; i++)
	{
		if (nl[i])
		{
			if (sa->useOr)
				continue;
			return false;
		}
		vals[n] = VARDATA_ANY(DatumGetPointer(el[i]));
		lens[n++] = VARSIZE_ANY_EXHDR(DatumGetPointer(el[i]));
	}
	if (n == 0)
		return false;
	out->op = sa->useOr ? SSTR_IN : SSTR_NOTIN;
	out->nvals = n;
	out->vals = vals;
	out->lens = lens;
	return true;
}

bool
sift_aggspec_str_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out)
{
	const SiftSrcLeaf *lf;
	Node	   *e;

	if (p->kind != SAP_LUT || p->level != 0 || p->leaf >= nbase)	/* a virtual leaf (a member by key, a count) is not stored */
		return false;
	lf = &lay->leaf[p->leaf];
	if (lf->kind != SCL_SCALAR || lf->vtype != SCV_CODE || lf->parent >= 0 || lf->lossy || !text_type(lf->typid))
		return false;
	out->leaf = p->leaf;
	e = strip_relabel(p->expr);
	return IsA(e, OpExpr) ? str_cmp_test((OpExpr *) e, out) : IsA(e, ScalarArrayOpExpr) && str_in_test((ScalarArrayOpExpr *) e, out);
}

static bool
plain_key(const SiftAggKey *k)
{
	return k->kind == SAK_LEAF || k->kind == SAK_LUT || k->kind == SAK_NUM || k->kind == SAK_SET || k->kind == SAK_COMBO;
}

static bool
plain_agg(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_COUNT_ROWS:
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
		case SAA_GROUPING:
			return true;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
		case SAA_COUNT_DISTINCT:
			return true;
		/* the outer aggregates of the arms: a row is in one item, so the cells it makes and the groups they reach add up */
		case SAA_O_COUNT_STAR:
		case SAA_O_COUNT_ROWS:
		case SAA_O_COUNT_KEY:
		case SAA_O_SUM_I64:
		case SAA_O_SUM_F64:
		case SAA_O_AVG_F64:
			return true;
		case SAA_MIN:
		case SAA_MAX:
			return true;
		case SAA_GEN:
			return a->gen->parallel;
		default:
			return false;
	}
}

/* does the group table go to partitions on disk when it outgrows hash memory (agg/spill.c)? It must travel as a blob, one pass, one table */
bool
sift_aggspec_spillable(const SiftAggSpec *s)
{
	if (s->topn || s->nsets > 0 || s->narms > 0 || s->nkeys == 0 || !sift_aggspec_parallel_ok(s))
		return false;
	for (int j = 0; j < s->nkeys; j++)
		if (s->key[j].kind == SAK_SET)
			return false;
	/* the values of an ordered-set aggregate are one sort for all the groups, which a blob of some of them cannot take */
	for (int i = 0; i < s->naggs; i++)
		if (is_ordered_agg(&s->agg[i]) || s->agg[i].kind == SAA_GEN)
			return false;
	return true;
}

/*
 * Why the scan runs in one process whatever the cost (NULL: it can have workers), for EXPLAIN VERBOSE; *agg: the aggregate that decides it,
 * -1 when it is not an aggregate
 */
const char *
sift_aggspec_serial_reason(const SiftAggSpec *s, int *agg)
{
	*agg = -1;
	/* the rows of the spine are those of a plan the leader runs, and the rows no cell joined are a group of the end of the scan */
	if (s->spine)
		return "the rows of a spine are those of a plan the leader runs";
	/*
	 * a worker has no child plan to run: the rows of a joined sub-select, VALUES list or function are the leader's, who shares them; a child plan
	 * run per combination of leaves (params) is not, unless the leader reads it as a table of the unique key it takes the first row of
	 * (join_first_rows; a leader that cannot runs the scan alone)
	 */
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].params != NIL && !(s->join[i].first.ok && s->join[i].first.use))
			return "a joined child plan runs per combination of values";
	/* the rows nothing matched are known when every row of the table was seen: by one scan */
	for (int i = 0; i < s->njoin; i++)
		if (sift_aggjoin_preserves(s->join[i].kind))
			return "an outer join keeps the rows nothing matched";
	for (int j = 0; j < s->nkeys; j++)
		if (!plain_key(&s->key[j]))
			return "a group key whose groups the workers cannot merge";
	for (int i = 0; i < s->naggs; i++)
		if (!plain_agg(&s->agg[i]))
		{
			*agg = i;
			return "its partial state is not one the leader can merge";
		}
	/* the float sums of the scan are merged in lane order, the generic states as the workers finish: not in one scan */
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN)
			for (int j = 0; j < s->naggs; j++)
				if (s->agg[j].kind == SAA_SUM_F64 || s->agg[j].kind == SAA_AVG_F64)
				{
					*agg = i;
					return "a float sum is merged in lane order, a generic state as the workers finish";
				}
	return NULL;
}

bool
sift_aggspec_parallel_ok(const SiftAggSpec *s)
{
	int			agg;

	return sift_aggspec_serial_reason(s, &agg) == NULL;
}

