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
			elog(ERROR, "sift: unexpected operand type %u", t);
	}
	return 0;
}

/* the types sift_aggspec_operand_int reads */
static bool
integer_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID || t == BOOLOID;
}

/* the conjuncts a unit's zone can refuse: a comparison (not <>) of an integer-valued leaf with a constant, a numeric leaf as its scaled integer, an IN list */
static List *
zone_cmps(List *out, const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	const SiftSrcLeaf *lf;
	bool		num;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			out = zone_cmps(out, p->child[i], lay, nbase);
	if ((p->kind != SAP_CMP && p->kind != SAP_IN) || p->leaf >= nbase || (p->kind == SAP_CMP && p->op == SCMP_NE) || (p->kind == SAP_IN && p->op))
		return out;
	lf = &lay->leaf[p->leaf];
	num = lf->typid == NUMERICOID && lf->vtype == SCV_I64 && p->argtype == NUMERICOID;
	if (lf->parent >= 0 || lf->lossy || !(num || (integer_type(p->argtype) && integer_type(lf->typid))))
		return out;
	return lappend(out, (void *) p);
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

/* ---------- text tests on the encoded form ---------- */

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
			return !a->distinct;
		case SAA_COUNT_DISTINCT:
			return a->num.n == 0;
		case SAA_MIN:
		case SAA_MAX:
			return true;
		case SAA_GEN:
			return a->gen->parallel;
		default:
			return false;
	}
}

bool
sift_aggspec_parallel_ok(const SiftAggSpec *s)
{
	if (s->narms)
		return false;
	/* a worker has no child plan to run: the rows of a joined sub-select, VALUES list or function are the leader's */
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].child >= 0 || s->join[i].params != NIL)
			return false;
	/* the rows nothing matched are known when every row of the table was seen: by one scan */
	for (int i = 0; i < s->njoin; i++)
		if (sift_aggjoin_preserves(s->join[i].kind))
			return false;
	for (int j = 0; j < s->nkeys; j++)
		if (!plain_key(&s->key[j]))
			return false;
	for (int i = 0; i < s->naggs; i++)
		if (!plain_agg(&s->agg[i]))
			return false;
	/* the float sums of the scan are merged in lane order, the generic states as the workers finish: not in one scan */
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN)
			for (int j = 0; j < s->naggs; j++)
				if (s->agg[j].kind == SAA_SUM_F64 || s->agg[j].kind == SAA_AVG_F64)
					return false;
	return true;
}

/*
 * The arms of one query that scan the same rows for the same aggregates, joins and combos and differ in their group keys (a time series
 * at four granularities) run in one pass as the grouping sets of one spec: the distinct keys of the arms, then the number of the set.
 */
static bool
fuse_candidate(const SiftAggSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN)
			return false;
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].child >= 0 || s->join[i].params != NIL || sift_aggjoin_preserves(s->join[i].kind))
			return false;
	return s->nkeys > 0 && s->narms == 0 && s->nsets == 0;
}

/* the leaves the arms read together, and how: false when two arms read one leaf in different ways */
static bool
fused_leaves(SiftAggSpec *const *m, int n, SiftAggSpec *out)
{
	int			total = 0,
				k = 0;
	int		   *leaves;
	uint8	   *recheck;

	for (int i = 0; i < n; i++)
		total += m[i]->nleaves;
	leaves = palloc(Max(total, 1) * sizeof(int));
	recheck = palloc0(Max(total, 1));
	for (int i = 0; i < n; i++)
		for (int j = 0; j < m[i]->nleaves; j++)
		{
			int			at = 0;

			while (at < k && leaves[at] < m[i]->leaves[j])
				at++;
			if (at < k && leaves[at] == m[i]->leaves[j])
			{
				if (recheck[at] != m[i]->recheck[j])
					return false;
				continue;
			}
			memmove(&leaves[at + 1], &leaves[at], (k - at) * sizeof(int));
			memmove(&recheck[at + 1], &recheck[at], k - at);
			leaves[at] = m[i]->leaves[j];
			recheck[at] = m[i]->recheck[j];
			k++;
		}
	out->nleaves = k;
	out->leaves = leaves;
	out->recheck = recheck;
	return true;
}

/* what the arms must share to run in one pass: everything but their keys, and the leaves are those of all of them */
static List *
fuse_shape(const SiftAggSpec *s, const SiftAggSpec *all)
{
	SiftAggSpec t = *s;

	t.nkeys = 0;
	t.key = NULL;
	t.nworkers = 0;
	t.est_groups = 0;
	t.nleaves = all->nleaves;
	t.leaves = all->leaves;
	t.recheck = all->recheck;
	return sift_aggspec_pack(&t);
}

/* a key as a spec of its own: the keys of arms with the same combos are the same when these are equal */
static List *
key_pack(const SiftAggSpec *s, int j)
{
	SiftAggSpec t = {.nkeys = 1, .key = &s->key[j]};

	return sift_aggspec_pack(&t);
}

/* the distinct keys of the arms m[0..n) in the order of their first arm; per arm the position of each key in them (keymap, when asked for) */
static List *
fused_keys(SiftAggSpec *const *m, int n, int **keymap)
{
	List	   *packs = NIL,
			   *first = NIL;

	for (int i = 0; i < n; i++)
	{
		if (keymap)
			keymap[i] = palloc(m[i]->nkeys * sizeof(int));
		for (int j = 0; j < m[i]->nkeys; j++)
		{
			List	   *k = key_pack(m[i], j);
			int			at = 0;

			foreach_ptr(List, p, packs)
			{
				if (equal(p, k))
					break;
				at++;
			}
			if (at == list_length(packs))
			{
				packs = lappend(packs, k);
				first = lappend(first, &m[i]->key[j]);
			}
			if (keymap)
				keymap[i][j] = at;
		}
	}
	return first;
}

/* can one scan serve the arms m[0..n): the same rows, aggregates, joins and combos, and keys that fit the masks with the set's */
bool
sift_aggspec_fusable(SiftAggSpec *const *m, int n)
{
	SiftAggSpec all;
	List	   *shape;

	for (int i = 0; i < n; i++)
		if (!fuse_candidate(m[i]))
			return false;
	if (list_length(fused_keys(m, n, NULL)) >= SIFT_AGG_MAX_KEYS || !fused_leaves(m, n, &all))
		return false;
	shape = fuse_shape(m[0], &all);
	for (int i = 1; i < n; i++)
		if (!equal(shape, fuse_shape(m[i], &all)))
			return false;
	return true;
}

SiftAggSpec *
sift_aggspec_fuse(SiftAggSpec *const *m, int n, int **keymap)
{
	SiftAggSpec *f = palloc(sizeof(SiftAggSpec));
	List	   *keys = fused_keys(m, n, keymap);
	int			nk = list_length(keys);

	*f = *m[0];
	fused_leaves(m, n, f);
	Assert(nk < SIFT_AGG_MAX_KEYS && n <= SIFT_AGG_MAX_KEYS);
	f->key = palloc0((nk + 1) * sizeof(SiftAggKey));
	f->setmask = palloc0(n * sizeof(uint32));
	f->nsets = n;
	f->est_groups = 0;
	f->nworkers = 0;
	for (int k = 0; k < nk; k++)
		f->key[k] = *(SiftAggKey *) list_nth(keys, k);
	for (int i = 0; i < n; i++)
	{
		for (int j = 0; j < m[i]->nkeys; j++)
			f->setmask[i] |= (uint32) 1 << keymap[i][j];
		f->setmask[i] |= (uint32) 1 << nk;
		f->nworkers = Max(f->nworkers, m[i]->nworkers);
		f->est_groups += m[i]->est_groups;
	}
	f->key[nk] = (SiftAggKey) {.kind = SAK_SET, .leaf = -1, .typid = INT4OID, .typmod = -1, .eqop = Int4EqualOperator, .desc = "grouping set"};
	f->nkeys = nk + 1;
	return f;
}
