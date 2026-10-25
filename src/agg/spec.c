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

bool
sift_aggspec_span_pred(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	if (p->kind != SAP_AND && p->kind != SAP_OR)
		return sift_aggspec_zone_cmp(p, lay, nbase);
	for (int i = 0; i < p->nchild; i++)
		if (!sift_aggspec_span_pred(p->child[i], lay, nbase))
			return false;
	return true;
}

/* x IS [NOT] NULL on a scalar leaf of the rows: the NULL count of a unit's zone refuses the unit (no NULL, or only NULLs) or holds the test for all its rows */
bool
sift_aggspec_null_zone(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	return p->kind == SAP_NULL && p->op <= 1 && p->level == 0 && !p->late && p->leaf < nbase && lay->leaf[p->leaf].parent < 0 && lay->leaf[p->leaf].kind == SCL_SCALAR;
}

/* the conjuncts a unit's zone can refuse */
static List *
zone_cmps(List *out, const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, bool nulls)
{
	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			out = zone_cmps(out, p->child[i], lay, nbase, nulls);
	return (nulls ? sift_aggspec_null_zone(p, lay, nbase) : sift_aggspec_zone_cmp(p, lay, nbase)) ? lappend(out, (void *) p) : out;
}

List *
sift_aggspec_zone_cmps(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, bool nulls)
{
	List	   *out = NIL;

	for (int i = 0; i < s->npred; i++)
		out = zone_cmps(out, s->pred[i], lay, nbase, nulls);
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
	return k->kind == SAK_LEAF || k->kind == SAK_LUT || k->kind == SAK_NUM || k->kind == SAK_SET || k->kind == SAK_COMBO || k->kind == SAK_SPINE;
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
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
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
		/* the aggregates of the columns of a nullable input (a preserved join): counts and float sums add up (the lanes keep the order of the sums) */
		case SAA_X_COUNT:
			return true;
		case SAA_X_SUM:
		case SAA_X_AVG:
			return a->restype == FLOAT8OID;
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
		if (is_ordered_agg(&s->agg[i]) || s->agg[i].kind == SAA_GEN || s->agg[i].kind == SAA_M_DISTINCT)
			return false;
	return true;
}

/*
 * The count(DISTINCT x) of a scan that may visit its superblocks in the order of the zone minimum of x (dist_stream_scan), -1 when there is no
 * such aggregate: the only DISTINCT aggregate of the spec, over a leaf of the source, and every other aggregate exact in any order (counts,
 * integer sums and averages, min and max), over plain keys, with no join, arm, grouping set, unnest, partition, match set or top-N, which read
 * the superblocks in an order of their own.
 */
int
sift_aggspec_dstream(const SiftAggSpec *s)
{
	int			found = -1;

	if (s->topn || s->search || s->nsets > 0 || s->narms > 0 || s->spine || s->njoin > 0 || s->ncombo > 0 || s->nparts > 0 || s->group >= 0 ||
		s->nvleaf > 0)
		return -1;
	for (int j = 0; j < s->nkeys; j++)
		if (!plain_key(&s->key[j]) || s->key[j].kind == SAK_SET || s->key[j].kind == SAK_COMBO)
			return -1;
	for (int i = 0; i < s->naggs; i++)
		switch (s->agg[i].kind)
		{
			case SAA_COUNT_DISTINCT:
				if (found >= 0 || s->agg[i].num.n || s->agg[i].leaf < 0)
					return -1;
				found = i;
				break;
			case SAA_COUNT_STAR:
			case SAA_COUNT:
			case SAA_COUNT_ROWS:
			case SAA_SUM_INT:
			case SAA_AVG_INT:
			case SAA_MIN:
			case SAA_MAX:
				break;
			default:
				return -1;
		}
	return found;
}

/*
 * Why the scan runs in one process whatever the cost (NULL: it can have workers), for EXPLAIN VERBOSE; *agg: the aggregate that decides it,
 * -1 when it is not an aggregate
 */
const char *
sift_aggspec_serial_reason(const SiftAggSpec *s, int *agg)
{
	*agg = -1;
	/* the rows nothing matched are known when every participant told which it matched (hit); the cells of arms are flushed by the one that read the row */
	for (int i = 0; i < s->njoin; i++)
		if (sift_aggjoin_preserves(s->join[i].kind) && s->narms > 0)
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
sift_aggspec_workers_only(const SiftAggSpec *s)
{
	bool		only = s->spine;

	for (int i = 0; i < s->njoin; i++)
		only |= s->join[i].params != NIL && !(s->join[i].first.ok && s->join[i].first.use);
	return only;
}

bool
sift_aggspec_parallel_ok(const SiftAggSpec *s)
{
	int			agg;

	return sift_aggspec_serial_reason(s, &agg) == NULL;
}

