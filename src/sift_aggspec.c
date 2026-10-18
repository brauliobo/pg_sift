/*
 * sift_aggspec.c - the SiftAggSpec as a flat List: Integer, String and Node items in the order the unpacker reads them.
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

#include "sift_aggspec.h"

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

static List *
zone_cmps(List *out, const SiftAggPred *p, const SiftSrcLayout *lay, int nbase)
{
	const SiftSrcLeaf *lf;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			out = zone_cmps(out, p->child[i], lay, nbase);
	if (p->kind != SAP_CMP || p->op == SCMP_NE || !integer_type(p->argtype) || p->leaf >= nbase)
		return out;
	lf = &lay->leaf[p->leaf];
	return lf->parent < 0 && !lf->lossy && integer_type(lf->typid) ? lappend(out, (void *) p) : out;
}

List *
sift_aggspec_zone_cmps(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase)
{
	List	   *out = NIL;

	for (int i = 0; i < s->npred; i++)
		out = zone_cmps(out, s->pred[i], lay, nbase);
	return out;
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
sift_aggspec_str_test(const SiftAggPred *p, const SiftSrcLayout *lay, SiftSrcStrTest *out)
{
	const SiftSrcLeaf *lf;
	Node	   *e;

	if (p->kind != SAP_LUT || p->level != 0)
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
 * The arms of one query that scan the same rows for the same aggregates and differ in their group keys (a time series at four
 * granularities) run in one pass as the grouping sets of one spec: the keys of the arms one after the other, then the number of the set.
 */
static bool
fuse_candidate(const SiftAggSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN)
			return false;
	return s->nkeys > 0 && s->narms == 0 && s->nsets == 0 && s->ncombo == 0 && s->njoin == 0 && s->npost == 0 && s->nvleaf == 0;
}

static List *
pack_without_keys(const SiftAggSpec *s)
{
	SiftAggSpec t = *s;

	t.nkeys = 0;
	t.key = NULL;
	t.nworkers = 0;
	t.est_groups = 0;
	return sift_aggspec_pack(&t);
}

bool
sift_aggspec_fusable(const SiftAggSpec *a, const SiftAggSpec *b)
{
	return fuse_candidate(a) && fuse_candidate(b) && equal(pack_without_keys(a), pack_without_keys(b));
}

SiftAggSpec *
sift_aggspec_fuse(SiftAggSpec *const *m, int n, int *keyoff)
{
	SiftAggSpec *f = palloc(sizeof(SiftAggSpec));
	int			nk = 0;

	*f = *m[0];
	for (int i = 0; i < n; i++)
		nk += m[i]->nkeys;
	Assert(nk < SIFT_AGG_MAX_KEYS && n <= 32);
	f->key = palloc0((nk + 1) * sizeof(SiftAggKey));
	f->setmask = palloc0(n * sizeof(uint32));
	f->nsets = n;
	f->est_groups = 0;
	nk = 0;
	for (int i = 0; i < n; i++)
	{
		keyoff[i] = nk;
		memcpy(&f->key[nk], m[i]->key, m[i]->nkeys * sizeof(SiftAggKey));
		for (int j = 0; j < m[i]->nkeys; j++)
			f->setmask[i] |= (uint32) 1 << (nk + j);
		nk += m[i]->nkeys;
		f->nworkers = Max(f->nworkers, m[i]->nworkers);
		f->est_groups += m[i]->est_groups;
	}
	f->key[nk] = (SiftAggKey) {.kind = SAK_SET, .leaf = -1, .typid = INT4OID, .typmod = -1, .eqop = Int4EqualOperator, .desc = "grouping set"};
	for (int i = 0; i < n; i++)
		f->setmask[i] |= (uint32) 1 << nk;
	f->nkeys = nk + 1;
	return f;
}

typedef struct Pack
{
	List	   *items;
} Pack;

static void
put_int(Pack *p, int v)
{
	p->items = lappend(p->items, makeInteger(v));
}

static void
put_str(Pack *p, const char *s)
{
	p->items = lappend(p->items, makeString(pstrdup(s ? s : "")));
}

/* Node or NULL */
static void
put_node(Pack *p, Node *n)
{
	put_int(p, n != NULL);
	if (n)
		p->items = lappend(p->items, n);
}

static void
put_dbl(Pack *p, double d)
{
	char		buf[64];

	snprintf(buf, sizeof(buf), "%.17g", d);
	put_str(p, buf);
}

static void
put_num(Pack *p, const SiftAggNum *n)
{
	put_int(p, n->n);
	put_int(p, n->scale);
	for (int i = 0; i < n->n; i++)
	{
		const SiftAggNx *x = &n->step[i];
		sift_i128	m = x->c;
		char		buf[48],
				   *o = buf + sizeof(buf) - 1;

		*o = '\0';
		do
			*--o = '0' + (int) (m < 0 ? -(m % 10) : m % 10);
		while ((m /= 10) != 0);
		if (x->c < 0)
			*--o = '-';
		put_int(p, x->op);
		put_int(p, x->leaf);
		put_int(p, x->a);
		put_int(p, x->b);
		put_str(p, o);
	}
}

static void
put_pred(Pack *p, const SiftAggPred *pr)
{
	if (pr == NULL)
	{
		put_int(p, -1);
		return;
	}
	put_int(p, pr->kind);
	put_int(p, pr->id);
	put_int(p, pr->leaf);
	put_int(p, pr->op);
	put_int(p, (int) pr->argtype);
	put_int(p, pr->level);
	put_int(p, pr->late);
	put_node(p, pr->expr);
	put_num(p, &pr->num);
	put_str(p, pr->desc);
	put_int(p, pr->nchild);
	for (int i = 0; i < pr->nchild; i++)
		put_pred(p, pr->child[i]);
}

static void
put_cond(Pack *p, const SiftAggCond *c)
{
	put_int(p, c->col);
	put_int(p, c->op);
	put_int(p, c->isint);
	put_str(p, psprintf(INT64_FORMAT, c->ci));
	put_dbl(p, c->cd);
}

static void
put_gen(Pack *p, const SiftAggGen *g)
{
	put_int(p, g != NULL);
	if (g == NULL)
		return;
	put_node(p, g->aggref);
	put_int(p, g->nargs);
	put_int(p, g->ntrans);
	for (int k = 0; k < g->nargs; k++)
	{
		put_int(p, (int) g->arg[k].typid);
		put_int(p, g->arg[k].nleaves);
		for (int l = 0; l < g->arg[k].nleaves; l++)
			put_int(p, g->arg[k].leaf[l]);
		put_node(p, g->arg[k].expr);
		put_int(p, g->arg[k].mapped);
		put_int(p, g->arg[k].combo);
		put_int(p, g->arg[k].slot);
	}
	put_node(p, g->direct);
	put_int(p, g->distinct);
	put_int(p, g->darg);
	put_int(p, g->ordered);
	put_int(p, g->sorted);
	put_int(p, g->hyp);
	put_int(p, g->parallel);
}

static void
put_agg(Pack *p, const SiftAggAgg *a)
{
	put_int(p, a->kind);
	put_int(p, a->leaf);
	put_int(p, a->nvx);
	for (int j = 0; j < a->nvx; j++)
	{
		put_int(p, a->vx[j].op);
		put_int(p, a->vx[j].leaf);
		put_dbl(p, a->vx[j].c);
	}
	put_num(p, &a->num);
	put_int(p, a->distinct);
	put_pred(p, a->filter);
	put_int(p, (int) a->restype);
	put_int(p, (int) a->argtype);
	put_int(p, (int) a->collid);
	put_int(p, a->col);
	put_int(p, a->ncond);
	for (int j = 0; j < a->ncond; j++)
		put_cond(p, &a->cond[j]);
	put_int(p, a->combo);
	put_int(p, a->slot);
	put_int(p, a->nref);
	for (int j = 0; j < a->nref; j++)
		put_int(p, a->ref[j]);
	put_str(p, a->desc);
	put_dbl(p, a->frac);
	put_int(p, a->desc_order);
	put_int(p, a->share);
	put_gen(p, a->gen);
	put_int(p, (int) a->aggfn);
}

static void
put_arms(Pack *p, const SiftAggSpec *s)
{
	put_int(p, s->narms);
	for (int i = 0; i < s->narms; i++)
	{
		const SiftAggArm *a = &s->arm[i];

		put_int(p, (int) a->keymask);
		put_int(p, (int) a->hmask);
		put_int(p, a->nfilter);
		for (int j = 0; j < a->nfilter; j++)
			put_pred(p, a->filter[j]);
		put_int(p, a->ninner);
		for (int j = 0; j < a->ninner; j++)
			put_agg(p, &a->inner[j]);
		put_int(p, a->nhaving);
		for (int j = 0; j < a->nhaving; j++)
			put_cond(p, &a->having[j]);
		put_node(p, a->hexpr);
		put_int(p, a->nhp);
		for (int j = 0; j < a->nhp; j++)
			put_int(p, a->hsrc[j]);
		put_int(p, a->ncol);
		for (int j = 0; j < a->ncol; j++)
			put_int(p, a->colsrc[j]);
	}
}

static void
put_joins(Pack *p, const SiftAggSpec *s)
{
	put_int(p, s->njoin);
	for (int i = 0; i < s->njoin; i++)
	{
		const SiftAggJoin *j = &s->join[i];

		put_int(p, (int) j->relid);
		put_int(p, j->kind);
		put_int(p, j->fslot);
		put_int(p, j->nkey);
		for (int k = 0; k < j->nkey; k++)
		{
			put_int(p, j->keyatt[k]);
			put_int(p, (int) j->keytype[k]);
			put_int(p, j->keypos[k]);
			put_node(p, j->keyexpr[k]);
		}
		put_int(p, j->ncol);
		for (int k = 0; k < j->ncol; k++)
		{
			put_int(p, j->colatt[k]);
			put_int(p, (int) j->coltype[k]);
			put_int(p, j->coltypmod[k]);
			put_int(p, (int) j->colcoll[k]);
		}
		put_str(p, j->desc);
	}
	put_int(p, s->ncombo);
	for (int i = 0; i < s->ncombo; i++)
	{
		const SiftAggCombo *c = &s->combo[i];

		put_int(p, c->nleaves);
		for (int k = 0; k < c->nleaves; k++)
		{
			put_int(p, c->leaf[k]);
			put_int(p, (int) c->ptype[k]);
			put_int(p, c->ptypmod[k]);
			put_int(p, (int) c->pcoll[k]);
		}
		put_int(p, c->join);
		put_int(p, c->level);
		put_int(p, c->nslots);
		for (int k = 0; k < c->nslots; k++)
		{
			put_int(p, c->slot[k].is_key);
			put_int(p, c->slot[k].key);
			put_int(p, c->slot[k].agg);
			put_node(p, c->slot[k].expr);
			put_int(p, c->slot[k].direct);
		}
	}
}

List *
sift_aggspec_pack(const SiftAggSpec *s)
{
	Pack		pk = {NIL};
	Pack	   *p = &pk;

	put_int(p, (int) s->indexoid);
	put_int(p, (int) s->heapoid);
	put_int(p, s->group);
	put_int(p, s->nleaves);
	for (int i = 0; i < s->nleaves; i++)
		put_int(p, s->leaves[i]);
	put_int(p, s->npred);
	for (int i = 0; i < s->npred; i++)
		put_pred(p, s->pred[i]);
	put_int(p, s->nkeys);
	for (int i = 0; i < s->nkeys; i++)
	{
		const SiftAggKey *k = &s->key[i];

		put_int(p, k->kind);
		put_int(p, k->leaf);
		put_int(p, k->slot);
		put_node(p, k->expr);
		put_num(p, &k->num);
		put_int(p, (int) k->typid);
		put_int(p, k->typmod);
		put_int(p, (int) k->collid);
		put_int(p, (int) k->eqop);
		put_str(p, k->desc);
	}
	put_int(p, s->naggs);
	for (int i = 0; i < s->naggs; i++)
		put_agg(p, &s->agg[i]);
	put_joins(p, s);
	put_int(p, s->npost);
	for (int i = 0; i < s->npost; i++)
		put_pred(p, s->post[i]);
	put_int(p, s->nvleaf);
	for (int i = 0; i < s->nvleaf; i++)
	{
		put_int(p, s->vleaf[i].obj);
		put_str(p, s->vleaf[i].key);
	}
	put_arms(p, s);
	put_int(p, s->nsets);
	for (int i = 0; i < s->nsets; i++)
		put_int(p, (int) s->setmask[i]);
	put_int(p, s->nhidden);
	put_int(p, s->npreds_total);
	put_dbl(p, s->est_groups);
	put_int(p, s->nworkers);
	put_str(p, s->desc_quals);
	put_node(p, s->search);
	put_node(p, s->topn);
	put_str(p, s->desc_topn ? s->desc_topn : "");
	return pk.items;
}

typedef struct Unpack
{
	const List *items;
	int			pos;
} Unpack;

static Node *
get_item(Unpack *u)
{
	if (u->pos >= list_length(u->items))
		elog(ERROR, "sift: truncated aggregate plan");
	return list_nth(u->items, u->pos++);
}

static int
get_int(Unpack *u)
{
	return intVal(get_item(u));
}

static char *
get_str(Unpack *u)
{
	return pstrdup(strVal(get_item(u)));
}

static Node *
get_node(Unpack *u)
{
	return get_int(u) ? get_item(u) : NULL;
}

static void
get_num(Unpack *u, SiftAggNum *n)
{
	n->n = get_int(u);
	n->scale = get_int(u);
	n->step = palloc(Max(n->n, 1) * sizeof(SiftAggNx));
	for (int i = 0; i < n->n; i++)
	{
		SiftAggNx  *x = &n->step[i];
		int			scale;

		x->op = get_int(u);
		x->leaf = get_int(u);
		x->a = get_int(u);
		x->b = get_int(u);
		if (!sift_parse_decimal(get_str(u), &x->c, &scale))
			elog(ERROR, "sift: bad number in the aggregate plan");
	}
}

static SiftAggPred *
get_pred(Unpack *u)
{
	SiftAggPred *pr;
	int			kind = get_int(u);

	if (kind < 0)
		return NULL;
	pr = palloc0(sizeof(SiftAggPred));
	pr->kind = kind;
	pr->id = get_int(u);
	pr->leaf = get_int(u);
	pr->op = get_int(u);
	pr->argtype = (Oid) get_int(u);
	pr->level = get_int(u);
	pr->late = get_int(u);
	pr->expr = get_node(u);
	get_num(u, &pr->num);
	pr->desc = get_str(u);
	pr->nchild = get_int(u);
	pr->child = pr->nchild ? palloc(pr->nchild * sizeof(SiftAggPred *)) : NULL;
	for (int i = 0; i < pr->nchild; i++)
		pr->child[i] = get_pred(u);
	return pr;
}

static void
get_cond(Unpack *u, SiftAggCond *c)
{
	c->col = get_int(u);
	c->op = get_int(u);
	c->isint = get_int(u);
	c->ci = strtoll(get_str(u), NULL, 10);
	c->cd = strtod(get_str(u), NULL);
}

static SiftAggGen *
get_gen(Unpack *u)
{
	SiftAggGen *g;

	if (!get_int(u))
		return NULL;
	g = palloc0(sizeof(SiftAggGen));
	g->aggref = get_node(u);
	g->nargs = get_int(u);
	g->ntrans = get_int(u);
	for (int k = 0; k < g->nargs; k++)
	{
		g->arg[k].typid = (Oid) get_int(u);
		g->arg[k].nleaves = get_int(u);
		for (int l = 0; l < g->arg[k].nleaves; l++)
			g->arg[k].leaf[l] = get_int(u);
		g->arg[k].expr = get_node(u);
		g->arg[k].mapped = get_int(u);
		g->arg[k].combo = get_int(u);
		g->arg[k].slot = get_int(u);
	}
	g->direct = get_node(u);
	g->distinct = get_int(u);
	g->darg = get_int(u);
	g->ordered = get_int(u);
	g->sorted = get_int(u);
	g->hyp = get_int(u);
	g->parallel = get_int(u);
	return g;
}

static void
get_agg(Unpack *u, SiftAggAgg *a)
{
	a->kind = get_int(u);
	a->leaf = get_int(u);
	a->nvx = get_int(u);
	a->vx = palloc(Max(a->nvx, 1) * sizeof(SiftAggVx));
	for (int j = 0; j < a->nvx; j++)
	{
		a->vx[j].op = get_int(u);
		a->vx[j].leaf = get_int(u);
		a->vx[j].c = strtod(get_str(u), NULL);
	}
	get_num(u, &a->num);
	a->distinct = get_int(u);
	a->filter = get_pred(u);
	a->restype = (Oid) get_int(u);
	a->argtype = (Oid) get_int(u);
	a->collid = (Oid) get_int(u);
	a->col = get_int(u);
	a->ncond = get_int(u);
	a->cond = palloc0(Max(a->ncond, 1) * sizeof(SiftAggCond));
	for (int j = 0; j < a->ncond; j++)
		get_cond(u, &a->cond[j]);
	a->combo = get_int(u);
	a->slot = get_int(u);
	a->nref = get_int(u);
	a->ref = palloc(Max(a->nref, 1) * sizeof(int));
	for (int j = 0; j < a->nref; j++)
		a->ref[j] = get_int(u);
	a->desc = get_str(u);
	a->frac = strtod(get_str(u), NULL);
	a->desc_order = get_int(u);
	a->share = get_int(u);
	a->gen = get_gen(u);
	a->aggfn = (Oid) get_int(u);
}

static void
get_arms(Unpack *u, SiftAggSpec *s)
{
	s->narms = get_int(u);
	s->arm = palloc0(Max(s->narms, 1) * sizeof(SiftAggArm));
	for (int i = 0; i < s->narms; i++)
	{
		SiftAggArm *a = &s->arm[i];

		a->keymask = (uint32) get_int(u);
		a->hmask = (uint32) get_int(u);
		a->nfilter = get_int(u);
		a->filter = palloc(Max(a->nfilter, 1) * sizeof(SiftAggPred *));
		for (int j = 0; j < a->nfilter; j++)
			a->filter[j] = get_pred(u);
		a->ninner = get_int(u);
		a->inner = palloc0(Max(a->ninner, 1) * sizeof(SiftAggAgg));
		for (int j = 0; j < a->ninner; j++)
			get_agg(u, &a->inner[j]);
		a->nhaving = get_int(u);
		a->having = palloc0(Max(a->nhaving, 1) * sizeof(SiftAggCond));
		for (int j = 0; j < a->nhaving; j++)
			get_cond(u, &a->having[j]);
		a->hexpr = get_node(u);
		a->nhp = get_int(u);
		a->hsrc = palloc(Max(a->nhp, 1) * sizeof(int));
		for (int j = 0; j < a->nhp; j++)
			a->hsrc[j] = get_int(u);
		a->ncol = get_int(u);
		a->colsrc = palloc(Max(a->ncol, 1) * sizeof(int));
		for (int j = 0; j < a->ncol; j++)
			a->colsrc[j] = get_int(u);
	}
}

static void
get_joins(Unpack *u, SiftAggSpec *s)
{
	s->njoin = get_int(u);
	s->join = palloc0(Max(s->njoin, 1) * sizeof(SiftAggJoin));
	for (int i = 0; i < s->njoin; i++)
	{
		SiftAggJoin *j = &s->join[i];

		j->relid = (Oid) get_int(u);
		j->kind = get_int(u);
		j->fslot = get_int(u);
		j->nkey = get_int(u);
		for (int k = 0; k < j->nkey; k++)
		{
			j->keyatt[k] = get_int(u);
			j->keytype[k] = (Oid) get_int(u);
			j->keypos[k] = get_int(u);
			j->keyexpr[k] = get_node(u);
		}
		j->ncol = get_int(u);
		for (int k = 0; k < j->ncol; k++)
		{
			j->colatt[k] = get_int(u);
			j->coltype[k] = (Oid) get_int(u);
			j->coltypmod[k] = get_int(u);
			j->colcoll[k] = (Oid) get_int(u);
		}
		j->desc = get_str(u);
	}
	s->ncombo = get_int(u);
	s->combo = palloc0(Max(s->ncombo, 1) * sizeof(SiftAggCombo));
	for (int i = 0; i < s->ncombo; i++)
	{
		SiftAggCombo *c = &s->combo[i];

		c->nleaves = get_int(u);
		for (int k = 0; k < c->nleaves; k++)
		{
			c->leaf[k] = get_int(u);
			c->ptype[k] = (Oid) get_int(u);
			c->ptypmod[k] = get_int(u);
			c->pcoll[k] = (Oid) get_int(u);
		}
		c->join = get_int(u);
		c->level = get_int(u);
		c->nslots = get_int(u);
		c->slot = palloc0(Max(c->nslots, 1) * sizeof(SiftAggSlot));
		for (int k = 0; k < c->nslots; k++)
		{
			c->slot[k].is_key = get_int(u);
			c->slot[k].key = get_int(u);
			c->slot[k].agg = get_int(u);
			c->slot[k].expr = get_node(u);
			c->slot[k].direct = get_int(u);
		}
	}
}

SiftAggSpec *
sift_aggspec_unpack(const List *items)
{
	Unpack		un = {items, 0};
	Unpack	   *u = &un;
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = (Oid) get_int(u);
	s->heapoid = (Oid) get_int(u);
	s->group = get_int(u);
	s->nleaves = get_int(u);
	s->leaves = palloc(Max(s->nleaves, 1) * sizeof(int));
	for (int i = 0; i < s->nleaves; i++)
		s->leaves[i] = get_int(u);
	s->npred = get_int(u);
	s->pred = palloc(Max(s->npred, 1) * sizeof(SiftAggPred *));
	for (int i = 0; i < s->npred; i++)
		s->pred[i] = get_pred(u);
	s->nkeys = get_int(u);
	s->key = palloc0(Max(s->nkeys, 1) * sizeof(SiftAggKey));
	for (int i = 0; i < s->nkeys; i++)
	{
		SiftAggKey *k = &s->key[i];

		k->kind = get_int(u);
		k->leaf = get_int(u);
		k->slot = get_int(u);
		k->expr = get_node(u);
		get_num(u, &k->num);
		k->typid = (Oid) get_int(u);
		k->typmod = get_int(u);
		k->collid = (Oid) get_int(u);
		k->eqop = (Oid) get_int(u);
		k->desc = get_str(u);
	}
	s->naggs = get_int(u);
	s->agg = palloc0(Max(s->naggs, 1) * sizeof(SiftAggAgg));
	for (int i = 0; i < s->naggs; i++)
		get_agg(u, &s->agg[i]);
	get_joins(u, s);
	s->npost = get_int(u);
	s->post = palloc(Max(s->npost, 1) * sizeof(SiftAggPred *));
	for (int i = 0; i < s->npost; i++)
		s->post[i] = get_pred(u);
	s->nvleaf = get_int(u);
	for (int i = 0; i < s->nvleaf; i++)
	{
		s->vleaf[i].obj = get_int(u);
		s->vleaf[i].key = get_str(u);
	}
	get_arms(u, s);
	s->nsets = get_int(u);
	s->setmask = palloc(Max(s->nsets, 1) * sizeof(uint32));
	for (int i = 0; i < s->nsets; i++)
		s->setmask[i] = (uint32) get_int(u);
	s->nhidden = get_int(u);
	s->npreds_total = get_int(u);
	s->est_groups = strtod(get_str(u), NULL);
	s->nworkers = get_int(u);
	s->desc_quals = get_str(u);
	s->search = get_node(u);
	s->topn = get_node(u);
	s->desc_topn = get_str(u);
	return s;
}
