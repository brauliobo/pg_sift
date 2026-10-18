/*
 * sift_aggspec.c - the SiftAggSpec as a flat List: Integer, String and Node items in the order the unpacker reads them.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/value.h"

#include "sift_aggspec.h"

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
	put_str(p, pr->desc);
	put_int(p, pr->nchild);
	for (int i = 0; i < pr->nchild; i++)
		put_pred(p, pr->child[i]);
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
		put_node(p, k->expr);
		put_int(p, (int) k->typid);
		put_int(p, k->typmod);
		put_int(p, (int) k->collid);
		put_int(p, (int) k->eqop);
		put_str(p, k->desc);
	}
	put_int(p, s->naggs);
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];

		put_int(p, a->kind);
		put_int(p, a->leaf);
		put_int(p, a->nvx);
		for (int j = 0; j < a->nvx; j++)
		{
			put_int(p, a->vx[j].op);
			put_int(p, a->vx[j].leaf);
			put_dbl(p, a->vx[j].c);
		}
		put_pred(p, a->filter);
		put_int(p, (int) a->restype);
		put_int(p, (int) a->argtype);
		put_int(p, (int) a->collid);
		put_str(p, a->desc);
	}
	put_int(p, s->npreds_total);
	put_dbl(p, s->est_groups);
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
	pr->desc = get_str(u);
	pr->nchild = get_int(u);
	pr->child = pr->nchild ? palloc(pr->nchild * sizeof(SiftAggPred *)) : NULL;
	for (int i = 0; i < pr->nchild; i++)
		pr->child[i] = get_pred(u);
	return pr;
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
		k->expr = get_node(u);
		k->typid = (Oid) get_int(u);
		k->typmod = get_int(u);
		k->collid = (Oid) get_int(u);
		k->eqop = (Oid) get_int(u);
		k->desc = get_str(u);
	}
	s->naggs = get_int(u);
	s->agg = palloc0(Max(s->naggs, 1) * sizeof(SiftAggAgg));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];

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
		a->filter = get_pred(u);
		a->restype = (Oid) get_int(u);
		a->argtype = (Oid) get_int(u);
		a->collid = (Oid) get_int(u);
		a->desc = get_str(u);
	}
	s->npreds_total = get_int(u);
	s->est_groups = strtod(get_str(u), NULL);
	s->desc_quals = get_str(u);
	return s;
}
