/*
 * agg/spec_pack.c - pack and unpack of the spec as a flat List
 *
 * One function per struct does both: the Codec either appends the fields it walks to the list (pack) or sets them from it (unpack), so a field that is added
 * here is written and read in the same place and in the same order. The packed list is only read by the same build (the workers' copy of the plan). The integers between two items that are
 * not one (a string, a node) are one item, a bytea of their varints: a plan packs some five hundred of them, and an Integer node and a list cell for each took 34,000 instructions.
 */
/* check-memory: allow-allocs every array here follows the keys, predicates, slots or constants of one plan or query, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"

#include "agg/spec.h"

typedef struct Codec
{
	List	   *items;			/* written (pack) or read (unpack) */
	int			pos;
	bool		rd;
	StringInfoData wr;			/* pack: the integers since the last item that is not one */
	const uint8 *rp;			/* unpack: the rest of the bytea of integers being read */
	const uint8 *re;
} Codec;

static pg_attribute_always_inline Node *
pk_item(Codec *c)
{
	SIFT_INVARIANT(c->pos < list_length(c->items) && c->rp == c->re, "sift: truncated aggregate plan");
	return list_nth(c->items, c->pos++);
}

/* the integers written since the last item that is not one become one item */
static void
pk_flush(Codec *c)
{
	bytea	   *b;

	if (c->wr.len == 0)
		return;
	b = palloc(VARHDRSZ + c->wr.len);
	SET_VARSIZE(b, VARHDRSZ + c->wr.len);
	memcpy(VARDATA(b), c->wr.data, c->wr.len);
	c->items = lappend(c->items, makeConst(BYTEAOID, -1, InvalidOid, -1, PointerGetDatum(b), false, false));
	resetStringInfo(&c->wr);
}

static pg_attribute_always_inline void
pk_int(Codec *c, int *v)
{
	if (c->rd)
	{
		uint32		u = 0;
		int			shift = 0;
		uint8		b;

		if (c->rp == c->re)
		{
			bytea	   *k;

			SIFT_INVARIANT(c->pos < list_length(c->items), "sift: truncated aggregate plan");
			k = DatumGetByteaPP(((Const *) list_nth(c->items, c->pos++))->constvalue);

			c->rp = (const uint8 *) VARDATA_ANY(k);
			c->re = c->rp + VARSIZE_ANY_EXHDR(k);
		}
		do
		{
			b = *c->rp++;
			u |= (uint32) (b & 0x7F) << shift;
			shift += 7;
		} while (b & 0x80);
		*v = (int) (u >> 1) ^ -(int) (u & 1);
	}
	else
	{
		uint32		u = ((uint32) *v << 1) ^ (uint32) (*v >> 31);

		for (; u >= 0x80; u >>= 7)
			appendStringInfoChar(&c->wr, (char) (u | 0x80));
		appendStringInfoChar(&c->wr, (char) u);
	}
}

/* the field f of any integer, enum or Oid type, as an int */
#define PK(c, f) \
	do { int v_ = (int) (f); pk_int(c, &v_); if ((c)->rd) (f) = (__typeof__(f)) v_; } while (0)

/* the count of an array, and the array itself on the way in */
#define PK_COUNT(c, cnt, arr) \
	do { PK(c, cnt); if ((c)->rd) (arr) = palloc0(Max(cnt, 1) * sizeof(*(arr))); } while (0)

/* a string, "" for NULL */
static void
pk_str(Codec *c, const char **s)
{
	if (c->rd)
		*s = pstrdup(strVal(pk_item(c)));
	else
	{
		pk_flush(c);
		c->items = lappend(c->items, makeString(pstrdup(*s ? *s : "")));
	}
}

#define PKS(c, f) pk_str(c, (const char **) &(f))

/* a string where the empty one is NULL */
#define PKS_OPT(c, f) \
	do { PKS(c, f); if ((c)->rd && !*(f)) (f) = NULL; } while (0)

/* Node or NULL */
static pg_attribute_always_inline void
pk_node(Codec *c, Node **n)
{
	int			has = !c->rd && *n != NULL;

	pk_int(c, &has);
	if (c->rd)
		*n = has ? pk_item(c) : NULL;
	else if (has)
	{
		pk_flush(c);
		c->items = lappend(c->items, *n);
	}
}

#define PKN(c, f) pk_node(c, (Node **) &(f))

/* a double as the two integers of its bits: exact, with no formatting ("%.17g" took 2,000 instructions each and a plan of a scope read packs about fifty) */
static void
pk_dbl(Codec *c, double *d)
{
	uint64		u;
	int			hi,
				lo;

	memcpy(&u, d, sizeof(u));
	hi = (int) (u >> 32);
	lo = (int) (u & 0xFFFFFFFF);
	pk_int(c, &hi);
	pk_int(c, &lo);
	u = (uint64) (uint32) hi << 32 | (uint32) lo;
	memcpy(d, &u, sizeof(u));
}

static void
pk_i64(Codec *c, int64 *v)
{
	const char *s;

	if (c->rd)
	{
		*v = strtoll(strVal(pk_item(c)), NULL, 10);
		return;
	}
	s = psprintf(INT64_FORMAT, *v);
	pk_str(c, &s);
}

static void codec_spec(Codec *c, SiftAggSpec *s);

static void
codec_scope(Codec *c, SiftAggScope **sc)
{
	int			has = !c->rd && *sc != NULL;

	pk_int(c, &has);
	if (!has)
		return;
	if (c->rd)
		*sc = palloc0(sizeof(SiftAggScope));
	PKS((c), (*sc)->key_name);
	PK(c, (*sc)->key_type);
	PK(c, (*sc)->fk);
	PKN(c, (*sc)->param);
	PKS_OPT(c, (*sc)->param_desc);
	PK_COUNT(c, (*sc)->nkeys, (*sc)->keys);
	for (int i = 0; (*sc)->param == NULL && i < (*sc)->nkeys; i++)
		pk_i64(c, &(*sc)->keys[i]);
	PK(c, (*sc)->once);
	PK(c, (*sc)->all);
	has = !c->rd && (*sc)->alt != NULL;
	pk_int(c, &has);
	if (!has)
		return;
	if (c->rd)
		(*sc)->alt = palloc0(sizeof(SiftAggSpec));
	codec_spec(c, (*sc)->alt);
	PK(c, (*sc)->nknot);
	for (int i = 0; i < (*sc)->nknot; i++)
	{
		pk_dbl(c, &(*sc)->knot_k[i]);
		pk_dbl(c, &(*sc)->knot_instr[i]);
	}
	pk_dbl(c, &(*sc)->knot_rows);
	pk_dbl(c, &(*sc)->entry_instr);
	pk_dbl(c, &(*sc)->rowwise);
}

static void
codec_num(Codec *c, SiftAggNum *n)
{
	PK(c, n->n);
	PK(c, n->scale);
	PK(c, n->dig);
	if (c->rd)
		n->step = palloc(Max(n->n, 1) * sizeof(SiftAggNx));
	for (int i = 0; i < n->n; i++)
	{
		SiftAggNx  *x = &n->step[i];

		PK(c, x->op);
		PK(c, x->leaf);
		PK(c, x->a);
		PK(c, x->b);
		if (c->rd)
		{
			int			scale;

			SIFT_INVARIANT(sift_parse_decimal(strVal(pk_item(c)), &x->c, &scale), "sift: bad number in the aggregate plan");
		}
		else
		{
			sift_i128	m = x->c;
			char		buf[48],
					   *o = buf + sizeof(buf) - 1;
			const char *s;

			*o = '\0';
			do
				*--o = '0' + (int) (m < 0 ? -(m % 10) : m % 10);
			while ((m /= 10) != 0);
			if (x->c < 0)
				*--o = '-';
			s = o;
			pk_str(c, &s);
		}
	}
}

/* the predicate tree: -1 for NULL */
static void
codec_pred(Codec *c, SiftAggPred **pp)
{
	SiftAggPred *pr;
	int			kind = !c->rd && *pp ? (int) (*pp)->kind : -1;

	pk_int(c, &kind);
	if (kind < 0)
	{
		*pp = NULL;
		return;
	}
	if (c->rd)
	{
		*pp = palloc0(sizeof(SiftAggPred));
		(*pp)->kind = kind;
	}
	pr = *pp;
	PK(c, pr->id);
	PK(c, pr->leaf);
	PK(c, pr->leaf2);
	PK(c, pr->wslot);
	PK(c, pr->op);
	PK(c, pr->argtype);
	PK(c, pr->level);
	PK(c, pr->late);
	PK(c, pr->negated);
	PK(c, pr->pass_poison);
	PKN(c, pr->expr);
	PKN(c, pr->cal);
	PK(c, pr->calmode);
	PK(c, pr->step);
	PK(c, pr->zoned);
	codec_num(c, &pr->num);
	PKS(c, pr->desc);
	PK(c, pr->nchild);
	if (c->rd)
		pr->child = pr->nchild ? palloc0(pr->nchild * sizeof(SiftAggPred *)) : NULL;
	for (int i = 0; i < pr->nchild; i++)
		codec_pred(c, &pr->child[i]);
}

static void
codec_cond(Codec *c, SiftAggCond *d)
{
	PK(c, d->col);
	PK(c, d->op);
	PK(c, d->isint);
	pk_i64(c, &d->ci);
	pk_dbl(c, &d->cd);
}

static void
codec_gen(Codec *c, SiftAggGen **gp)
{
	SiftAggGen *g = c->rd ? NULL : *gp;
	int			has = g != NULL;

	pk_int(c, &has);
	if (!has)
	{
		*gp = NULL;
		return;
	}
	if (c->rd)
		g = *gp = palloc0(sizeof(SiftAggGen));
	PKN(c, g->aggref);
	PK(c, g->nargs);
	PK(c, g->ntrans);
	for (int k = 0; k < g->nargs; k++)
	{
		PK(c, g->arg[k].typid);
		PK(c, g->arg[k].nleaves);
		for (int l = 0; l < g->arg[k].nleaves; l++)
			PK(c, g->arg[k].leaf[l]);
		PKN(c, g->arg[k].expr);
		PK(c, g->arg[k].mapped);
		PK(c, g->arg[k].combo);
		PK(c, g->arg[k].slot);
	}
	PKN(c, g->direct);
	PK(c, g->distinct);
	PK(c, g->darg);
	PK(c, g->ordered);
	PK(c, g->sorted);
	PK(c, g->first);
	PK(c, g->topn);
	PK(c, g->ties);
	PK(c, g->hyp);
	PKS_OPT(c, g->serial);
	PK(c, g->hll);
}

static void
codec_agg(Codec *c, SiftAggAgg *a)
{
	PK(c, a->kind);
	PK(c, a->leaf);
	PK(c, a->wsum);
	PK_COUNT(c, a->nvx, a->vx);
	for (int j = 0; j < a->nvx; j++)
	{
		PK(c, a->vx[j].op);
		PK(c, a->vx[j].leaf);
		pk_dbl(c, &a->vx[j].c);
	}
	codec_num(c, &a->num);
	PK(c, a->distinct);
	codec_pred(c, &a->filter);
	PK(c, a->restype);
	PK(c, a->argtype);
	PK(c, a->collid);
	PK(c, a->col);
	PK_COUNT(c, a->ncond, a->cond);
	for (int j = 0; j < a->ncond; j++)
		codec_cond(c, &a->cond[j]);
	PK(c, a->combo);
	PK(c, a->slot);
	PK_COUNT(c, a->nref, a->ref);
	for (int j = 0; j < a->nref; j++)
		PK(c, a->ref[j]);
	PKS(c, a->desc);
	pk_dbl(c, &a->frac);
	PK(c, a->desc_order);
	PK(c, a->share);
	codec_gen(c, &a->gen);
	PK(c, a->aggfn);
}

static void
codec_spine(Codec *c, SiftAggSpec *s)
{
	SiftAggSpine *sp;
	int			has = !c->rd && s->spine != NULL;

	pk_int(c, &has);
	if (!has)
		return;
	if (c->rd)
		s->spine = palloc0(sizeof(SiftAggSpine));
	sp = s->spine;
	PK(c, sp->left);
	PK(c, sp->child);
	PK(c, sp->ncol);
	PK(c, sp->npair);
	for (int i = 0; i < sp->npair; i++)
	{
		struct SiftAggSpinePair *q = &sp->pair[i];

		PK(c, q->col);
		PKN(c, q->konst);
		PK(c, q->nullsafe);
		PK(c, q->typid);
		PK(c, q->collid);
		PK(c, q->eqop);
		for (int k = 0; k < s->narms; k++)
			PK(c, q->key[k]);
	}
	PK(c, sp->nout);
	for (int i = 0; i < sp->nout; i++)
	{
		PK(c, sp->outcol[i]);
		PK(c, sp->outkey[i]);
	}
}

static void
codec_arms(Codec *c, SiftAggSpec *s)
{
	PK_COUNT(c, s->narms, s->arm);
	for (int i = 0; i < s->narms; i++)
	{
		SiftAggArm *a = &s->arm[i];

		PK(c, a->keymask);
		PK(c, a->hmask);
		PK_COUNT(c, a->nfilter, a->filter);
		for (int j = 0; j < a->nfilter; j++)
			codec_pred(c, &a->filter[j]);
		PK_COUNT(c, a->ninner, a->inner);
		for (int j = 0; j < a->ninner; j++)
			codec_agg(c, &a->inner[j]);
		PK_COUNT(c, a->nhaving, a->having);
		for (int j = 0; j < a->nhaving; j++)
			codec_cond(c, &a->having[j]);
		PKN(c, a->hexpr);
		PK_COUNT(c, a->nhp, a->hsrc);
		for (int j = 0; j < a->nhp; j++)
			PK(c, a->hsrc[j]);
		PK_COUNT(c, a->ncol, a->colsrc);
		for (int j = 0; j < a->ncol; j++)
			PK(c, a->colsrc[j]);
	}
	codec_spine(c, s);
}

static void
codec_joins(Codec *c, SiftAggSpec *s)
{
	PK_COUNT(c, s->njoin, s->join);
	for (int i = 0; i < s->njoin; i++)
	{
		SiftAggJoin *j = &s->join[i];

		PK(c, j->relid);
		PK(c, j->child);
		PK(c, j->kind);
		PK(c, j->subplan);
		PKN(c, j->params);
		PKN(c, j->args);
		PKN(c, j->sig);
		PK(c, j->scoped);
		PK(c, j->fslot);
		PK(c, j->nidx);
		for (int k = 0; k < j->nidx; k++)
		{
			PK(c, j->idxatt[k]);
			PK(c, j->idxop[k]);
			PK(c, j->idxcoll[k]);
			PK(c, j->idxany[k]);
			PKN(c, j->idxval[k]);
		}
		PK(c, j->nkey);
		for (int k = 0; k < j->nkey; k++)
		{
			PK(c, j->keyatt[k]);
			PK(c, j->keytype[k]);
			PK(c, j->keypos[k]);
			PKN(c, j->keyexpr[k]);
			PKN(c, j->tkey[k]);
			PK(c, j->keynull[k]);
		}
		PK(c, j->ncol);
		for (int k = 0; k < j->ncol; k++)
		{
			PK(c, j->colatt[k]);
			PK(c, j->coltype[k]);
			PK(c, j->coltypmod[k]);
			PK(c, j->colcoll[k]);
			PKN(c, j->colexpr[k]);
		}
		PK(c, j->first.ok);
		if (j->first.ok)
		{
			PK(c, j->first.use);
			PK(c, j->first.hybrid);
			PKN(c, j->first.pqual);
			PK(c, j->first.relid);
			PK(c, j->first.rows);
			PK(c, j->first.nkey);
			for (int k = 0; k < j->first.nkey; k++)
			{
				PK(c, j->first.keyatt[k]);
				PK(c, j->first.keytype[k]);
				PK(c, j->first.keyarg[k]);
			}
			PK(c, j->first.ncol);
			for (int k = 0; k < j->first.ncol; k++)
				PK(c, j->first.col[k]);
			PK(c, j->first.nord);
			for (int k = 0; k < j->first.nord; k++)
			{
				PK(c, j->first.ordatt[k]);
				PK(c, j->first.ordtype[k]);
				PK(c, j->first.orddesc[k]);
				PK(c, j->first.ordnullsfirst[k]);
			}
			PKN(c, j->first.qual);
		}
		PKS(c, j->desc);
	}
	PK_COUNT(c, s->ncombo, s->combo);
	for (int i = 0; i < s->ncombo; i++)
	{
		SiftAggCombo *cb = &s->combo[i];

		PK(c, cb->nleaves);
		for (int k = 0; k < cb->nleaves; k++)
		{
			PK(c, cb->leaf[k]);
			PK(c, cb->refcombo[k]);
			PK(c, cb->refcol[k]);
			PK(c, cb->ptype[k]);
			PK(c, cb->ptypmod[k]);
			PK(c, cb->pcoll[k]);
		}
		PK(c, cb->join);
		PK(c, cb->level);
		PK_COUNT(c, cb->nslots, cb->slot);
		for (int k = 0; k < cb->nslots; k++)
		{
			PK(c, cb->slot[k].is_key);
			PK(c, cb->slot[k].key);
			PK(c, cb->slot[k].agg);
			PKN(c, cb->slot[k].expr);
			PK(c, cb->slot[k].direct);
		}
	}
}

static void
codec_spec(Codec *c, SiftAggSpec *s)
{
	PK(c, s->indexoid);
	PK(c, s->heapoid);
	PK_COUNT(c, s->nparts, s->partheap);
	if (c->rd)
		s->partindex = palloc0(Max(s->nparts, 1) * sizeof(Oid));
	for (int i = 0; i < s->nparts; i++)
	{
		PK(c, s->partheap[i]);
		PK(c, s->partindex[i]);
	}
	PKN(c, s->partprune);
	PK(c, s->group);
	PK(c, s->left_lateral);
	PK(c, s->part_final);
	PK_COUNT(c, s->nleaves, s->leaves);
	if (c->rd)
	{
		s->recheck = palloc0(Max(s->nleaves, 1));
		s->gate = palloc0(Max(s->nleaves, 1) * sizeof(uint64));
	}
	for (int i = 0; i < s->nleaves; i++)
	{
		int			lo = c->rd ? 0 : (int) (s->gate[i] & PG_UINT32_MAX),
					hi = c->rd ? 0 : (int) (s->gate[i] >> 32);

		PK(c, s->leaves[i]);
		PK(c, s->recheck[i]);
		pk_int(c, &lo);
		pk_int(c, &hi);
		if (c->rd)
			s->gate[i] = (uint32) lo | (uint64) (uint32) hi << 32;
	}
	PK_COUNT(c, s->npred, s->pred);
	for (int i = 0; i < s->npred; i++)
		codec_pred(c, &s->pred[i]);
	PK_COUNT(c, s->nwide, s->wide);
	for (int i = 0; i < s->nwide; i++)
		codec_pred(c, &s->wide[i]);
	PK_COUNT(c, s->ntype, s->type_leaf);
	for (int i = 0; i < s->ntype; i++)
		PK(c, s->type_leaf[i]);
	PK_COUNT(c, s->nkeys, s->key);
	for (int i = 0; i < s->nkeys; i++)
	{
		SiftAggKey *k = &s->key[i];

		PK(c, k->kind);
		PK(c, k->leaf);
		PK(c, k->slot);
		PKN(c, k->expr);
		PK(c, k->bucket);
		PK(c, k->zoned);
		codec_num(c, &k->num);
		PK(c, k->typid);
		PK(c, k->typmod);
		PK(c, k->collid);
		PK(c, k->eqop);
		PK(c, k->coded);
		PKS(c, k->desc);
	}
	PK_COUNT(c, s->naggs, s->agg);
	for (int i = 0; i < s->naggs; i++)
		codec_agg(c, &s->agg[i]);
	codec_joins(c, s);
	PK(c, s->npost);
	PK(c, s->nprepost);
	if (c->rd)
	{
		s->post = palloc0(Max(s->npost, 1) * sizeof(SiftAggPred *));
		s->postsets = palloc0(Max(s->npost, 1) * sizeof(uint32));
	}
	for (int i = 0; i < s->npost; i++)
	{
		int			sets = !c->rd && s->postsets ? (int) s->postsets[i] : 0;

		codec_pred(c, &s->post[i]);
		pk_int(c, &sets);
		if (c->rd)
			s->postsets[i] = (uint32) sets;
	}
	PK(c, s->nvleaf);
	for (int i = 0; i < s->nvleaf; i++)
	{
		PK(c, s->vleaf[i].obj);
		PKS(c, s->vleaf[i].key);
		PK(c, s->vleaf[i].count);
		PK(c, s->vleaf[i].types);
		PK(c, s->vleaf[i].bound);
		PK(c, s->vleaf[i].recheck);
		PK(c, s->vleaf[i].cast);
	}
	codec_arms(c, s);
	PK_COUNT(c, s->nsets, s->setmask);
	for (int i = 0; i < s->nsets; i++)
		PK(c, s->setmask[i]);
	{
		int			has = !c->rd && s->setaggs != NULL;

		pk_int(c, &has);
		if (has && c->rd)
			s->setaggs = palloc0(Max(s->naggs, 1) * sizeof(uint32));
		for (int i = 0; has && i < s->naggs; i++)
			PK(c, s->setaggs[i]);
		has = !c->rd && s->setpred != NULL;
		pk_int(c, &has);
		if (has && c->rd)
			s->setpred = palloc0(s->nsets * sizeof(SiftAggPred *));
		for (int i = 0; has && i < s->nsets; i++)
			codec_pred(c, &s->setpred[i]);
	}
	PK(c, s->nhidden);
	PK(c, s->npreds_total);
	pk_dbl(c, &s->est_groups);
	pk_dbl(c, &s->est_rows);
	PK(c, s->nworkers);
	PK(c, s->gpart);
	PK(c, s->jpart);
	pk_dbl(c, &s->wide_unit_rows);
	PKS_OPT(c, s->serial);
	PKS(c, s->desc_quals);
	PKN(c, s->search);
	codec_scope(c, &s->scope);
	PKS_OPT(c, s->scope_why);
	PKN(c, s->topn);
	PKS(c, s->desc_topn);
	PK(c, s->gtopn_k);
	PK(c, s->gtopn_agg);
	PK(c, s->gtopn_desc);
	PK(c, s->gtopn_nulls_first);
	PK_COUNT(c, s->nord, s->ord);
	for (int i = 0; i < s->nord; i++)
	{
		PK(c, s->ord[i].key);
		PK(c, s->ord[i].sortop);
		PK(c, s->ord[i].collation);
		PK(c, s->ord[i].nulls_first);
		PKN(c, s->ord[i].subst);
		PKN(c, s->ord[i].eq);
		PK(c, s->ord[i].eqop);
		PK(c, s->ord[i].eqcoll);
	}
}

List *
sift_aggspec_pack(const SiftAggSpec *s)
{
	Codec		c = {NIL, 0, false};

	initStringInfo(&c.wr);
	/* a pack only reads the spec */
	codec_spec(&c, (SiftAggSpec *) s);
	pk_flush(&c);
	pfree(c.wr.data);
	return c.items;
}

SiftAggSpec *
sift_aggspec_unpack(const List *items)
{
	Codec		c = {(List *) items, 0, true};
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	codec_spec(&c, s);
	return s;
}
