/*
 * agg/spec_pack.c - pack and unpack of the spec as a flat List with per-node writers and readers
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
	put_int(p, pr->leaf2);
	put_int(p, pr->op);
	put_int(p, (int) pr->argtype);
	put_int(p, pr->level);
	put_int(p, pr->late);
	put_int(p, pr->pass_poison);
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
	put_int(p, g->first);
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
	put_int(p, s->spine != NULL);
	if (s->spine == NULL)
		return;
	put_int(p, s->spine->left);
	put_int(p, s->spine->child);
	put_int(p, s->spine->ncol);
	put_int(p, s->spine->npair);
	for (int i = 0; i < s->spine->npair; i++)
	{
		const struct SiftAggSpinePair *q = &s->spine->pair[i];

		put_int(p, q->col);
		put_node(p, q->konst);
		put_int(p, q->nullsafe);
		put_int(p, (int) q->typid);
		put_int(p, (int) q->collid);
		put_int(p, (int) q->eqop);
		for (int k = 0; k < s->narms; k++)
			put_int(p, q->key[k]);
	}
	put_int(p, s->spine->nout);
	for (int i = 0; i < s->spine->nout; i++)
	{
		put_int(p, s->spine->outcol[i]);
		put_int(p, s->spine->outkey[i]);
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
		put_int(p, j->child);
		put_int(p, j->kind);
		put_int(p, j->subplan);
		put_node(p, (Node *) j->params);
		put_node(p, (Node *) j->args);
		put_node(p, j->sig);
		put_int(p, j->scoped);
		put_int(p, j->fslot);
		put_int(p, j->nidx);
		for (int k = 0; k < j->nidx; k++)
		{
			put_int(p, j->idxatt[k]);
			put_int(p, (int) j->idxop[k]);
			put_int(p, (int) j->idxcoll[k]);
			put_int(p, j->idxany[k]);
			put_node(p, j->idxval[k]);
		}
		put_int(p, j->nkey);
		for (int k = 0; k < j->nkey; k++)
		{
			put_int(p, j->keyatt[k]);
			put_int(p, (int) j->keytype[k]);
			put_int(p, j->keypos[k]);
			put_node(p, j->keyexpr[k]);
			put_node(p, j->tkey[k]);
			put_int(p, j->keynull[k]);
		}
		put_int(p, j->ncol);
		for (int k = 0; k < j->ncol; k++)
		{
			put_int(p, j->colatt[k]);
			put_int(p, (int) j->coltype[k]);
			put_int(p, j->coltypmod[k]);
			put_int(p, (int) j->colcoll[k]);
		}
		put_int(p, j->first.ok);
		if (j->first.ok)
		{
			put_int(p, j->first.use);
			put_int(p, (int) j->first.relid);
			put_int(p, j->first.nkey);
			for (int k = 0; k < j->first.nkey; k++)
			{
				put_int(p, j->first.keyatt[k]);
				put_int(p, (int) j->first.keytype[k]);
				put_int(p, j->first.keyarg[k]);
			}
			put_int(p, j->first.ncol);
			for (int k = 0; k < j->first.ncol; k++)
				put_int(p, j->first.col[k]);
			put_int(p, j->first.nord);
			for (int k = 0; k < j->first.nord; k++)
			{
				put_int(p, j->first.ordatt[k]);
				put_int(p, (int) j->first.ordtype[k]);
				put_int(p, j->first.orddesc[k]);
				put_int(p, j->first.ordnullsfirst[k]);
			}
			put_node(p, (Node *) j->first.qual);
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
			put_int(p, c->refcombo[k]);
			put_int(p, c->refcol[k]);
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
	put_int(p, s->nparts);
	for (int i = 0; i < s->nparts; i++)
	{
		put_int(p, (int) s->partheap[i]);
		put_int(p, (int) s->partindex[i]);
	}
	put_node(p, (Node *) s->partprune);
	put_int(p, s->group);
	put_int(p, s->nleaves);
	for (int i = 0; i < s->nleaves; i++)
	{
		put_int(p, s->leaves[i]);
		put_int(p, s->recheck[i]);
		put_int(p, (int) (s->gate[i] & PG_UINT32_MAX));
		put_int(p, (int) (s->gate[i] >> 32));
	}
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
		put_int(p, k->bucket);
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
	put_int(p, s->nprepost);
	for (int i = 0; i < s->npost; i++)
	{
		put_pred(p, s->post[i]);
		put_int(p, s->postsets ? (int) s->postsets[i] : 0);
	}
	put_int(p, s->nvleaf);
	for (int i = 0; i < s->nvleaf; i++)
	{
		put_int(p, s->vleaf[i].obj);
		put_str(p, s->vleaf[i].key);
		put_int(p, s->vleaf[i].count);
		put_int(p, s->vleaf[i].types);
		put_int(p, s->vleaf[i].recheck);
		put_int(p, (int) s->vleaf[i].cast);
	}
	put_arms(p, s);
	put_int(p, s->nsets);
	for (int i = 0; i < s->nsets; i++)
		put_int(p, (int) s->setmask[i]);
	put_int(p, s->setaggs != NULL);
	for (int i = 0; s->setaggs && i < s->naggs; i++)
		put_int(p, (int) s->setaggs[i]);
	put_int(p, s->setpred != NULL);
	for (int i = 0; s->setpred && i < s->nsets; i++)
		put_pred(p, s->setpred[i]);
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
	SIFT_INVARIANT(u->pos < list_length(u->items), "sift: truncated aggregate plan");
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
		SIFT_INVARIANT(sift_parse_decimal(get_str(u), &x->c, &scale), "sift: bad number in the aggregate plan");
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
	pr->leaf2 = get_int(u);
	pr->op = get_int(u);
	pr->argtype = (Oid) get_int(u);
	pr->level = get_int(u);
	pr->late = get_int(u);
	pr->pass_poison = get_int(u);
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
	g->first = get_int(u);
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
	if (!get_int(u))
		return;
	s->spine = palloc0(sizeof(SiftAggSpine));
	s->spine->left = get_int(u);
	s->spine->child = get_int(u);
	s->spine->ncol = get_int(u);
	s->spine->npair = get_int(u);
	for (int i = 0; i < s->spine->npair; i++)
	{
		struct SiftAggSpinePair *q = &s->spine->pair[i];

		q->col = get_int(u);
		q->konst = get_node(u);
		q->nullsafe = get_int(u);
		q->typid = (Oid) get_int(u);
		q->collid = (Oid) get_int(u);
		q->eqop = (Oid) get_int(u);
		for (int k = 0; k < s->narms; k++)
			q->key[k] = get_int(u);
	}
	s->spine->nout = get_int(u);
	for (int i = 0; i < s->spine->nout; i++)
	{
		s->spine->outcol[i] = get_int(u);
		s->spine->outkey[i] = get_int(u);
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
		j->child = get_int(u);
		j->kind = get_int(u);
		j->subplan = get_int(u);
		j->params = (List *) get_node(u);
		j->args = (List *) get_node(u);
		j->sig = get_node(u);
		j->scoped = get_int(u);
		j->fslot = get_int(u);
		j->nidx = get_int(u);
		for (int k = 0; k < j->nidx; k++)
		{
			j->idxatt[k] = get_int(u);
			j->idxop[k] = (Oid) get_int(u);
			j->idxcoll[k] = (Oid) get_int(u);
			j->idxany[k] = get_int(u);
			j->idxval[k] = get_node(u);
		}
		j->nkey = get_int(u);
		for (int k = 0; k < j->nkey; k++)
		{
			j->keyatt[k] = get_int(u);
			j->keytype[k] = (Oid) get_int(u);
			j->keypos[k] = get_int(u);
			j->keyexpr[k] = get_node(u);
			j->tkey[k] = get_node(u);
			j->keynull[k] = get_int(u);
		}
		j->ncol = get_int(u);
		for (int k = 0; k < j->ncol; k++)
		{
			j->colatt[k] = get_int(u);
			j->coltype[k] = (Oid) get_int(u);
			j->coltypmod[k] = get_int(u);
			j->colcoll[k] = (Oid) get_int(u);
		}
		if ((j->first.ok = get_int(u)))
		{
			j->first.use = get_int(u);
			j->first.relid = (Oid) get_int(u);
			j->first.nkey = get_int(u);
			for (int k = 0; k < j->first.nkey; k++)
			{
				j->first.keyatt[k] = get_int(u);
				j->first.keytype[k] = (Oid) get_int(u);
				j->first.keyarg[k] = get_int(u);
			}
			j->first.ncol = get_int(u);
			for (int k = 0; k < j->first.ncol; k++)
				j->first.col[k] = get_int(u);
			j->first.nord = get_int(u);
			for (int k = 0; k < j->first.nord; k++)
			{
				j->first.ordatt[k] = get_int(u);
				j->first.ordtype[k] = (Oid) get_int(u);
				j->first.orddesc[k] = get_int(u);
				j->first.ordnullsfirst[k] = get_int(u);
			}
			j->first.qual = (List *) get_node(u);
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
			c->refcombo[k] = get_int(u);
			c->refcol[k] = get_int(u);
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
	s->nparts = get_int(u);
	s->partheap = palloc(Max(s->nparts, 1) * sizeof(Oid));
	s->partindex = palloc(Max(s->nparts, 1) * sizeof(Oid));
	for (int i = 0; i < s->nparts; i++)
	{
		s->partheap[i] = (Oid) get_int(u);
		s->partindex[i] = (Oid) get_int(u);
	}
	s->partprune = (List *) get_node(u);
	s->group = get_int(u);
	s->nleaves = get_int(u);
	s->leaves = palloc(Max(s->nleaves, 1) * sizeof(int));
	s->recheck = palloc0(Max(s->nleaves, 1));
	s->gate = palloc0(Max(s->nleaves, 1) * sizeof(uint64));
	for (int i = 0; i < s->nleaves; i++)
	{
		s->leaves[i] = get_int(u);
		s->recheck[i] = get_int(u);
		s->gate[i] = (uint32) get_int(u);
		s->gate[i] |= (uint64) (uint32) get_int(u) << 32;
	}
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
		k->bucket = get_int(u);
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
	s->nprepost = get_int(u);
	s->post = palloc(Max(s->npost, 1) * sizeof(SiftAggPred *));
	s->postsets = palloc0(Max(s->npost, 1) * sizeof(uint32));
	for (int i = 0; i < s->npost; i++)
	{
		s->post[i] = get_pred(u);
		s->postsets[i] = (uint32) get_int(u);
	}
	s->nvleaf = get_int(u);
	for (int i = 0; i < s->nvleaf; i++)
	{
		s->vleaf[i].obj = get_int(u);
		s->vleaf[i].key = get_str(u);
		s->vleaf[i].count = get_int(u);
		s->vleaf[i].types = get_int(u);
		s->vleaf[i].recheck = get_int(u);
		s->vleaf[i].cast = (Oid) get_int(u);
	}
	get_arms(u, s);
	s->nsets = get_int(u);
	s->setmask = palloc(Max(s->nsets, 1) * sizeof(uint32));
	for (int i = 0; i < s->nsets; i++)
		s->setmask[i] = (uint32) get_int(u);
	if (get_int(u))
	{
		s->setaggs = palloc(Max(s->naggs, 1) * sizeof(uint32));
		for (int i = 0; i < s->naggs; i++)
			s->setaggs[i] = (uint32) get_int(u);
	}
	if (get_int(u))
	{
		s->setpred = palloc(s->nsets * sizeof(SiftAggPred *));
		for (int i = 0; i < s->nsets; i++)
			s->setpred[i] = get_pred(u);
	}
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
