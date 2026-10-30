/*
 * agg/spec.c - the SiftAggSpec as a flat List: Integer, String and Node items in the order the unpacker reads them.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
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

/* a scan with no join or aggregate, whatever its conditions: the rows of one table, as a top-N of them reads */
bool
sift_aggspec_plain(const SiftAggSpec *s)
{
	return s->njoin == 0 && s->ncombo == 0 && s->npost == 0 && s->narms == 0 && s->spine == NULL && s->nsets == 0 && s->group < 0 && s->nvleaf == 0 &&
		!s->left_lateral;
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

/* the comparison a text function is, or 0; the range operators only compare bytes in the C collation */
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
	}
	return 0;
}

static bool
str_pattern_func(Oid funcid)
{
	return funcid == F_TEXTLIKE || funcid == F_TEXTNLIKE || funcid == F_TEXTICLIKE || funcid == F_STARTS_WITH || funcid == F_TEXTREGEXEQ ||
		funcid == F_TEXTREGEXNE || funcid == F_TEXTICREGEXEQ;
}

/*
 * LIKE 'abc%' is a prefix test, '%abc%' a substring test and '%abc' a suffix test, all bytewise for every deterministic collation; a match of bytes is one of
 * characters in UTF-8 and the single byte encodings only (a trail byte of SJIS or GBK is also an ASCII one). On true the literal is in *s, *len.
 */
static bool
like_test(const char **s, uint32 *len, SiftSrcStrOp *op)
{
	bool		head = *len > 0 && **s == '%',
				tail = *len > 1 && (*s)[*len - 1] == '%';
	const char *lit = *s + head;
	uint32		n = *len - head - tail;

	if ((!head && !tail) || n == 0 || memchr(lit, '%', n) || memchr(lit, '_', n) || memchr(lit, '\\', n) ||
		(head && GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1))
		return false;
	*s = lit;
	*len = n;
	*op = head ? (tail ? SSTR_CONTAINS : SSTR_SUFFIX) : SSTR_PREFIX;
	return true;
}

/*
 * A regular expression that is a literal, '^' and '$' at its ends, a punctuation mark after a backslash a character, is the test LIKE makes of it; the
 * literal is unescaped in a new string. ~* of such is a substring test with the letters folded (the regex compares the cases of the pattern's letters
 * only, in a collation that lowers A-Z to a-z).
 */
static bool
regex_literal(const char **s, uint32 *len, bool icase, SiftSrcStrOp *op)
{
	const char *re = *s;
	uint32		n = *len,
				k = 0;
	bool		head = n > 0 && re[0] == '^',
				tail = false;
	char	   *lit;

	if (n - head > 0 && re[n - 1] == '$')
	{
		uint32		bs = 0;

		for (uint32 i = n - 1; i > head && re[i - 1] == '\\'; i--)
			bs++;
		tail = bs % 2 == 0;
	}
	if (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1)
		return false;
	lit = palloc(n);
	for (uint32 i = head; i < n - tail; i++)
	{
		char		c = re[i];

		if (c == '\\')
		{
			if (i + 1 >= n - tail || ((re[i + 1] >= '0' && re[i + 1] <= '9') || (re[i + 1] >= 'a' && re[i + 1] <= 'z') || (re[i + 1] >= 'A' && re[i + 1] <= 'Z')))
				return false;
			c = re[++i];
		}
		else if (strchr(".*+?()[]{}|^$", c) != NULL)
			return false;
		lit[k++] = c;
	}
	if (k == 0 || (icase && (head || tail)))
		return false;
	*s = lit;
	*len = k;
	*op = head ? (tail ? SSTR_EQ : SSTR_PREFIX) : tail ? SSTR_SUFFIX : SSTR_CONTAINS;
	return true;
}

/* lower() of the text s under the collation, as a palloc'ed string */
static char *
lower_of(Oid collid, const char *s, uint32 *len)
{
	text	   *t = DatumGetTextPP(DirectFunctionCall1Coll(lower, collid, PointerGetDatum(cstring_to_text_with_len(s, *len))));

	*len = VARSIZE_ANY_EXHDR(t);
	return pnstrdup(VARDATA_ANY(t), *len);
}

bool
sift_aggspec_ascii_fold(Oid collid)
{
	static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
				lowers[] = "abcdefghijklmnopqrstuvwxyz";
	uint32		n = sizeof(upper) - 1;
	char	   *az;

	if (GetDatabaseEncoding() != PG_UTF8)
		return false;
	az = lower_of(collid, upper, &n);
	return n == sizeof(lowers) - 1 && memcmp(az, lowers, n) == 0;
}

/*
 * ILIKE '%part%' is lower(value) LIKE lower(part), and for a part of ASCII bytes the strings are compared as bytes with their ASCII letters in lower case,
 * if the collation lowers A-Z to a-z (not tr_TR's I) in a UTF-8 database: the lower case of any other character is not ASCII, but for U+0130 and U+212A,
 * which a part with an i or a k meets as what lower() makes of them (*vals[1], *vals[2], when that is not themselves; subst false for ~*, which does not
 * equate them). On true vals/lens hold the lower case part and perhaps those two.
 */
static bool
ilike_test(Oid collid, const char *s, uint32 len, bool subst, const char ***vals, uint32 **lens, int *nvals)
{
	char	   *low = palloc(len);
	bool		ik = false;

	if (!sift_aggspec_ascii_fold(collid))
		return false;
	for (uint32 i = 0; i < len; i++)
	{
		if ((uint8) s[i] >= 0x80)
			return false;
		low[i] = (char) (s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 0x20 : s[i]);
		ik |= low[i] == 'i' || low[i] == 'k';
	}
	*nvals = 1;
	*vals = palloc(3 * sizeof(char *));
	*lens = palloc(3 * sizeof(uint32));
	(*vals)[0] = low;
	(*lens)[0] = len;
	if (ik && subst)
	{
		uint32		l1 = 2,
					l2 = 3;
		char	   *i1 = lower_of(collid, "\xC4\xB0", &l1),
				   *i2 = lower_of(collid, "\xE2\x84\xAA", &l2);

		if (!(l1 == 2 && memcmp(i1, "\xC4\xB0", 2) == 0 && l2 == 3 && memcmp(i2, "\xE2\x84\xAA", 3) == 0))
		{
			(*vals)[1] = i1;
			(*lens)[1] = l1;
			(*vals)[2] = i2;
			(*lens)[2] = l2;
			*nvals = 3;
		}
	}
	return true;
}

/*
 * The test a pattern function is on its constant pattern: LIKE, NOT LIKE and ILIKE of a prefix, a part or a suffix, starts_with, and a regular expression that
 * is a literal. On true *op, and *s, *len the literal (ICONTAINS: spelled by ilike_test, where *subst says whether U+0130 and U+212A count).
 */
static bool
pattern_test(Oid funcid, const char **s, uint32 *len, SiftSrcStrOp *op, bool *subst)
{
	SiftSrcStrOp lop;

	*subst = true;
	switch (funcid)
	{
		case F_TEXTLIKE:
			if (!like_test(s, len, &lop))
				return false;
			*op = lop;
			return true;
		case F_TEXTNLIKE:
		case F_TEXTICLIKE:
			if (!like_test(s, len, &lop) || lop != SSTR_CONTAINS)
				return false;
			*op = funcid == F_TEXTNLIKE ? SSTR_NOTCONTAINS : SSTR_ICONTAINS;
			return true;
		case F_STARTS_WITH:
			*op = SSTR_PREFIX;
			return *len > 0;
		case F_TEXTREGEXEQ:
			return regex_literal(s, len, false, op);
		case F_TEXTREGEXNE:
			if (!regex_literal(s, len, false, op) || *op != SSTR_CONTAINS)
				return false;
			*op = SSTR_NOTCONTAINS;
			return true;
		case F_TEXTICREGEXEQ:
			if (!regex_literal(s, len, true, op))
				return false;
			*op = SSTR_ICONTAINS;
			*subst = false;
			return true;
	}
	return false;
}

/* a test of the leaf and a constant by a comparison or pattern function: leaf <op> constant, constant <op> leaf, leaf LIKE 'prefix%', '%part%' or '%suffix', ... */
static bool
str_call_test(Oid funcid, Oid collid, List *args, SiftSrcStrTest *out, bool ranges)
{
	Node	   *a,
			   *b;
	SiftSrcStrOp sop = 0;
	const char *s;
	uint32		len;
	bool		pattern = str_pattern_func(funcid),
				subst = true;

	if (list_length(args) != 2 || (!pattern && (sop = str_func_op(funcid)) == 0))
		return false;
	a = strip_relabel(linitial(args));
	b = strip_relabel(lsecond(args));
	if (IsA(b, Param) && !pattern)
	{
		Node	   *t = a;

		a = b;
		b = t;
		sop = str_commute(sop);
	}
	if (!IsA(a, Param) || !str_const(b, &s, &len))
		return false;
	if (pattern && !pattern_test(funcid, &s, &len, &sop, &subst))
		return false;
	if (sop >= SSTR_LT && sop <= SSTR_GE && !ranges ? collid != C_COLLATION_OID : !get_collation_isdeterministic(collid))
		return false;
	out->op = sop;
	out->collid = collid;
	if (sop == SSTR_ICONTAINS)
	{
		const char **vals;
		uint32	   *lens;

		if (!ilike_test(collid, s, len, subst, &vals, &lens, &out->nvals))
			return false;
		out->vals = vals;
		out->lens = lens;
		return true;
	}
	out->nvals = 1;
	out->vals = palloc(sizeof(char *));
	out->lens = palloc(sizeof(uint32));
	((const char **) out->vals)[0] = s;
	((uint32 *) out->lens)[0] = len;
	return true;
}

/* l <cmp> r for the int4 comparison function, or -1 when funcid is none */
static int
int4_holds(Oid funcid, int32 l, int32 r)
{
	switch (funcid)
	{
		case F_INT4EQ:
			return l == r;
		case F_INT4NE:
			return l != r;
		case F_INT4LT:
			return l < r;
		case F_INT4LE:
			return l <= r;
		case F_INT4GT:
			return l > r;
		case F_INT4GE:
			return l >= r;
	}
	return -1;
}

/*
 * strpos(leaf, part) (position(part IN leaf)) compared with an integer is a substring test when the comparison holds for every position of a part
 * found (1, 2, ...) and not for 0, or the other way round: > 0, >= 1, <> 0 hold it, = 0, < 1, <= 0 do not hold it.
 */
static bool
strpos_test(OpExpr *op, SiftSrcStrTest *out)
{
	Node	   *a = strip_relabel(linitial(op->args)),
			   *b = strip_relabel(lsecond(op->args));
	FuncExpr   *f;
	const char *s;
	uint32		len;
	int32		c;
	bool		flip = IsA(b, FuncExpr);
	int			absent,
				found;

	if (flip)
	{
		Node	   *t = a;

		a = b;
		b = t;
	}
	set_opfuncid(op);
	if (!IsA(a, FuncExpr) || !IsA(b, Const) || ((FuncExpr *) a)->funcid != F_STRPOS || ((Const *) b)->constisnull || ((Const *) b)->consttype != INT4OID ||
		int4_holds(op->opfuncid, 0, 0) < 0)
		return false;
	f = (FuncExpr *) a;
	c = DatumGetInt32(((Const *) b)->constvalue);
	absent = flip ? int4_holds(op->opfuncid, c, 0) : int4_holds(op->opfuncid, 0, c);
	found = flip ? int4_holds(op->opfuncid, c, 1) : int4_holds(op->opfuncid, 1, c);
	for (int32 pos = 2; pos <= 4; pos *= 2)
		if ((flip ? int4_holds(op->opfuncid, c, pos) : int4_holds(op->opfuncid, pos, c)) != found)
			return false;
	if (absent == found || list_length(f->args) != 2 || !IsA(strip_relabel(linitial(f->args)), Param) || !str_const(strip_relabel(lsecond(f->args)), &s, &len) ||
		len == 0 || !get_collation_isdeterministic(f->inputcollid) || (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1))
		return false;
	out->op = found ? SSTR_CONTAINS : SSTR_NOTCONTAINS;
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

static bool
str_expr_test(const SiftAggPred *p, SiftSrcStrTest *out, bool ranges)
{
	Node	   *e = strip_relabel(p->expr);

	if (IsA(e, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) e;

		set_opfuncid(op);
		return int4_holds(op->opfuncid, 0, 0) >= 0 ? strpos_test(op, out) : str_call_test(op->opfuncid, op->inputcollid, op->args, out, ranges);
	}
	if (IsA(e, FuncExpr))
		return str_call_test(((FuncExpr *) e)->funcid, ((FuncExpr *) e)->inputcollid, ((FuncExpr *) e)->args, out, ranges);
	return IsA(e, ScalarArrayOpExpr) && str_in_test((ScalarArrayOpExpr *) e, out);
}

static bool
str_leaf_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out, bool ranges)
{
	const SiftSrcLeaf *lf;

	if (p->kind != SAP_LUT || p->level != 0 || p->leaf >= nbase)	/* a virtual leaf (a member by key, a count) is not stored */
		return false;
	lf = &lay->leaf[p->leaf];
	if (lf->kind != SCL_SCALAR || lf->vtype != SCV_CODE || lf->parent >= 0 || lf->lossy || !text_type(lf->typid))
		return false;
	out->leaf = p->leaf;
	out->ints = NULL;
	return str_expr_test(p, out, ranges);
}

bool
sift_aggspec_str_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out)
{
	return str_leaf_test(p, lay, nbase, out, false);
}

/* ... and a test that bounds the strings of the leaf in the order of its collation (a comparison under any deterministic collation, a prefix where the bytes are that order): the ranks of an order array */
bool
sift_aggspec_text_bound(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out)
{
	return str_leaf_test(p, lay, nbase, out, true) && (out->op == SSTR_PREFIX ? sift_collate_is_c(out->collid) : (out->op == SSTR_EQ || (out->op >= SSTR_LT && out->op <= SSTR_GE)));
}

/* EXISTS element = a string, or in a set, of a list of text: the element leaf's test, for the units and rows that own such an element (a source prunes by it; the executor tests the elements) */
bool
sift_aggspec_elem_test(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, SiftSrcStrTest *out)
{
	const SiftAggPred *t;
	const SiftSrcLeaf *lf;

	if (p->kind != SAP_ANY_ELEM || p->nchild != 1 || p->op)
		return false;
	t = p->child[0];
	if (t->leaf < 0 || t->leaf >= nbase)	/* an AND of element tests has no leaf of its own */
		return false;
	lf = &lay->leaf[t->leaf];
	if (lf->kind != SCL_SCALAR || lf->parent < 0 || lf->lossy)
		return false;
	out->leaf = t->leaf;
	out->ints = NULL;
	if (t->kind == SAP_CMP && t->op == SCMP_EQ && (lf->typid == INT2OID || lf->typid == INT4OID || lf->typid == INT8OID) && IsA(t->expr, Const) &&
		!((Const *) t->expr)->constisnull && ((Const *) t->expr)->consttype == INT8OID)
	{
		int64	   *v = palloc(sizeof(int64));

		*v = DatumGetInt64(((Const *) t->expr)->constvalue);
		*out = (SiftSrcStrTest) {.leaf = t->leaf,.op = SSTR_EQ,.nvals = 1,.ints = v};
		return true;
	}
	if (t->kind != SAP_LUT || lf->vtype != SCV_CODE || !text_type(lf->typid))
		return false;
	return str_expr_test(t, out, false) && (out->op == SSTR_EQ || out->op == SSTR_IN);
}

static bool
plain_key(const SiftAggKey *k)
{
	return k->kind == SAK_LEAF || k->kind == SAK_LUT || k->kind == SAK_NUM || k->kind == SAK_SET || k->kind == SAK_COMBO || k->kind == SAK_SPINE;
}

/*
 * What the planner and the executor need to know of each kind of aggregate, in one table: a new kind fails the StaticAssert until it has a row.
 * merge: how the partial states of the workers reach the leader (a text is the reason when they cannot); order_exact: the result does not
 * depend on the order the rows are read in (sift_aggspec_dstream).
 */
typedef enum AggMerge
{
	AGG_MERGE_OK,				/* the leader adds them up */
	AGG_MERGE_SUM_TYPE,			/* a sum or average of a float8 or a numeric is added up; others hold the worker's own ids */
	AGG_MERGE_GEN,				/* the generic state says (SiftAggGen.serial) */
	AGG_MERGE_NEVER				/* the state holds ids of values that are the worker's own */
} AggMerge;

typedef struct AggKindInfo
{
	bool		set;
	AggMerge	merge;
	bool		order_exact;
} AggKindInfo;

#define KIND(k, ...) [k] = {.set = true, __VA_ARGS__}

static const AggKindInfo kind_info[] = {
	KIND(SAA_COUNT_STAR, .order_exact = true),
	KIND(SAA_COUNT, .order_exact = true),
	KIND(SAA_COUNT_ROWS, .order_exact = true),
	KIND(SAA_COUNT_DISTINCT),
	KIND(SAA_SUM_INT, .order_exact = true),
	KIND(SAA_SUM_F64),
	KIND(SAA_AVG_INT, .order_exact = true),
	KIND(SAA_AVG_F64),
	KIND(SAA_MIN, .order_exact = true),
	KIND(SAA_MAX, .order_exact = true),
	KIND(SAA_SUM_NUM),
	KIND(SAA_AVG_NUM),
	KIND(SAA_PCT_CONT),
	KIND(SAA_PCT_DISC),
	KIND(SAA_M_COUNT),
	KIND(SAA_M_DISTINCT),
	KIND(SAA_M_MIN, .merge = AGG_MERGE_NEVER),
	KIND(SAA_M_MAX, .merge = AGG_MERGE_NEVER),
	KIND(SAA_M_SUM, .merge = AGG_MERGE_NEVER),
	KIND(SAA_M_AVG, .merge = AGG_MERGE_NEVER),
	KIND(SAA_GROUPING),
	/* the outer aggregates of the arms: a row is in one item, so the cells it makes and the groups they reach add up */
	KIND(SAA_O_COUNT_STAR),
	KIND(SAA_O_COUNT_ROWS),
	KIND(SAA_O_AVG_F64),
	KIND(SAA_O_SUM_F64),
	KIND(SAA_O_SUM_I64),
	KIND(SAA_M_PG, .merge = AGG_MERGE_NEVER),
	KIND(SAA_GEN, .merge = AGG_MERGE_GEN),
	/* the aggregates of the columns of a nullable input (a preserved join): counts and float sums add up (the lanes keep the order of the sums) */
	KIND(SAA_X_COUNT),
	KIND(SAA_X_MIN, .merge = AGG_MERGE_NEVER),
	KIND(SAA_X_MAX, .merge = AGG_MERGE_NEVER),
	KIND(SAA_X_SUM, .merge = AGG_MERGE_SUM_TYPE),
	KIND(SAA_X_AVG, .merge = AGG_MERGE_SUM_TYPE),
	KIND(SAA_O_COUNT_KEY),
};

StaticAssertDecl(lengthof(kind_info) == SAA_NKINDS, "every kind of aggregate has a row in kind_info");

static const AggKindInfo *
kind_of(SiftAggKind kind)
{
	Assert(kind >= 0 && kind < SAA_NKINDS && kind_info[kind].set);
	return &kind_info[kind];
}

/* why the workers' partial states of the aggregate cannot be merged by the leader (NULL: they can) */
static const char *
agg_serial(const SiftAggAgg *a)
{
	switch (kind_of(a->kind)->merge)
	{
		case AGG_MERGE_OK:
			return NULL;
		case AGG_MERGE_SUM_TYPE:
			return a->restype == FLOAT8OID || a->restype == NUMERICOID ? NULL : "its sum is neither a float8 nor a numeric";
		case AGG_MERGE_GEN:
			return a->gen->serial;
		case AGG_MERGE_NEVER:
			return "its partial state holds ids of values that are the worker's own";
	}
	return "its partial state is not one the leader can merge";
}

/* does the group table go to partitions on disk when it outgrows hash memory (agg/spill.c)? It must travel as a blob, one pass, one table */
bool
sift_aggspec_spillable(const SiftAggSpec *s)
{
	/* an ordered scan holds its groups to the end: the partitions of a spilled scan come one after the other */
	if (s->topn || s->nord > 0 || s->nsets > 0 || s->narms > 0 || s->nkeys == 0 || !sift_aggspec_parallel_ok(s))
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
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
		{
			if (found >= 0 || s->agg[i].num.n || s->agg[i].leaf < 0)
				return -1;
			found = i;
		}
		else if (!kind_of(s->agg[i].kind)->order_exact)
			return -1;
	return found;
}

/*
 * Why the scan runs in one process whatever the cost (NULL: it can have workers), for EXPLAIN VERBOSE; *agg: the aggregate that decides it,
 * -1 when it is not an aggregate
 */
const char *
sift_aggspec_serial_reason(const SiftAggSpec *s, int *agg)
{
	const char *why;

	*agg = -1;
	/* the rows nothing matched are known when every participant told which it matched (hit); the cells of arms are flushed by the one that read the row */
	for (int i = 0; i < s->njoin; i++)
		if (sift_aggjoin_preserves(s->join[i].kind) && s->narms > 0)
			return "an outer join keeps the rows nothing matched";
	for (int j = 0; j < s->nkeys; j++)
		if (!plain_key(&s->key[j]))
			return "a group key whose groups the workers cannot merge";
	for (int i = 0; i < s->naggs; i++)
		if ((why = agg_serial(&s->agg[i])) != NULL)
		{
			*agg = i;
			return why;
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
		only |= s->join[i].params != NIL && !(s->join[i].first.ok && s->join[i].first.use && !s->join[i].first.hybrid);
	return only;
}

bool
sift_aggspec_parallel_ok(const SiftAggSpec *s)
{
	int			agg;

	return sift_aggspec_serial_reason(s, &agg) == NULL;
}

