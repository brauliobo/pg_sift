/*
 * sift_aggspec.c - the SiftAggSpec as a flat List: Integer, String and Node items in the order the unpacker reads them.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "catalog/pg_type_d.h"
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

static bool
plain_key(const SiftAggKey *k)
{
	return (k->kind == SAK_LEAF && k->typid != FLOAT4OID && k->typid != FLOAT8OID) || k->kind == SAK_LUT;
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
			return true;
		case SAA_COUNT_DISTINCT:
		case SAA_MIN:
		case SAA_MAX:
			return a->num.n == 0;
		default:
			return false;
	}
}

bool
sift_aggspec_parallel_ok(const SiftAggSpec *s)
{
	if (s->ncombo || s->njoin || s->nvleaf || s->narms || s->nsets)
		return false;
	for (int j = 0; j < s->nkeys; j++)
		if (!plain_key(&s->key[j]))
			return false;
	for (int i = 0; i < s->naggs; i++)
		if (!plain_agg(&s->agg[i]))
			return false;
	return true;
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
}

static void
put_arms(Pack *p, const SiftAggSpec *s)
{
	put_int(p, s->narms);
	for (int i = 0; i < s->narms; i++)
	{
		const SiftAggArm *a = &s->arm[i];

		put_int(p, (int) a->keymask);
		put_int(p, a->nfilter);
		for (int j = 0; j < a->nfilter; j++)
			put_pred(p, a->filter[j]);
		put_int(p, a->ninner);
		for (int j = 0; j < a->ninner; j++)
			put_agg(p, &a->inner[j]);
		put_int(p, a->nhaving);
		for (int j = 0; j < a->nhaving; j++)
			put_cond(p, &a->having[j]);
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
		put_int(p, j->left);
		put_int(p, j->nkey);
		for (int k = 0; k < j->nkey; k++)
		{
			put_int(p, j->keyatt[k]);
			put_int(p, (int) j->keytype[k]);
			put_int(p, j->keypos[k]);
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
	put_int(p, s->npreds_total);
	put_dbl(p, s->est_groups);
	put_int(p, s->nworkers);
	put_str(p, s->desc_quals);
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
		j->left = get_int(u);
		j->nkey = get_int(u);
		for (int k = 0; k < j->nkey; k++)
		{
			j->keyatt[k] = get_int(u);
			j->keytype[k] = (Oid) get_int(u);
			j->keypos[k] = get_int(u);
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
	s->npreds_total = get_int(u);
	s->est_groups = strtod(get_str(u), NULL);
	s->nworkers = get_int(u);
	s->desc_quals = get_str(u);
	return s;
}
