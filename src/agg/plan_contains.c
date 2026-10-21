/*
 * agg/plan_contains.c - contains and key tests on jsonb arrays: col @> '[{...}]', ?|, ?& (agg/plan_exists.c).
 */
#include "postgres.h"

#include "utils/fmgroids.h"

#include "agg/plan_exists_int.h"

/* ---- col @> '[{"k": v, ...}, ...]': every object of the right side is contained by some element ---- */

/* an element of an array of scalars (the unnamed child of the array leaf) equals the literal */
static SiftAggPred *
build_elem_lit(AggCtx *c, int grp, void *arg)
{
	return lit_test(c, grp, "", SCMP_EQ, arg, false);
}

static SiftAggPred *
build_contains(AggCtx *c, int grp, void *arg)
{
	JsonbContainer *obj = arg;
	int			n = JsonContainerSize(obj);
	JsonbIterator *it = JsonbIteratorInit(obj);
	JsonbValue	k,
				v;
	SiftAggPred *p = n > 1 ? new_junction(c, SAP_AND, n) : NULL;

	if (n == 0)
		return NULL;
	(void) JsonbIteratorNext(&it, &k, true);	/* the start of the object */
	for (int i = 0; i < n; i++)
	{
		Lit			lit;
		SiftAggPred *e;

		(void) JsonbIteratorNext(&it, &k, true);
		(void) JsonbIteratorNext(&it, &v, true);
		if (!jsonb_lit(&v, &lit))
			return NULL;
		if ((e = lit_test(c, grp, pnstrdup(k.val.string.val, k.val.string.len), SCMP_EQ, &lit, false)) == NULL)
			return NULL;
		if (p)
			p->child[i] = e;
		else
			p = e;
	}
	if (n > 1)
		junction_done(p);
	return p;
}

SiftAggPred *
compile_contains(AggCtx *c, Node *target, Node *rhs)
{
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbValue	v;
	SiftAggPred *p = NULL;
	int			grp,
				n;

	rhs = strip_relabel(rhs);
	if (!IsA(rhs, Const) || ((Const *) rhs)->constisnull || (grp = group_leaf_of(c, target, SCL_ARRAY)) < 0)
		return NULL;
	jb = DatumGetJsonbP(((Const *) rhs)->constvalue);
	if (!JB_ROOT_IS_ARRAY(jb) || JB_ROOT_IS_SCALAR(jb) || (n = JB_ROOT_COUNT(jb)) == 0)
		return NULL;
	if (n > 1)
		p = new_junction(c, SAP_AND, n);
	it = JsonbIteratorInit(&jb->root);
	(void) JsonbIteratorNext(&it, &v, true);	/* the start of the array */
	for (int i = 0; i < n; i++)
	{
		SiftAggPred *e;
		Lit			lit;

		(void) JsonbIteratorNext(&it, &v, true);
		if (v.type == jbvBinary && JsonContainerIsObject(v.val.binary.data))
			e = any_elem(c, grp, build_contains, v.val.binary.data);
		else if (jsonb_lit(&v, &lit) && lit.kind != 'z')	/* the JSON null of a scalar element: not proven */
			e = any_elem(c, grp, build_elem_lit, &lit);
		else
			return NULL;
		if (e == NULL)
			return NULL;
		if (p)
			p->child[i] = e;
		else
			p = e;
	}
	if (n > 1)
		junction_done(p);
	return p;
}

/*
 * x ? 'k', x ?| array['k', ..], x ?& array['k', ..] on an array of strings: an element is the string; for any (?|) or each (?&) of the
 * strings. The same test on an object (its keys) or a string is the other shapes of the value, which the index holds as an overflow.
 */
SiftAggPred *
compile_has_keys(AggCtx *c, Oid fid, Node *target, Node *keys_arg)
{
	List	   *keys = NIL;
	SiftAggPred *p;
	int			grp,
				n;

	keys_arg = strip_relabel(keys_arg);
	if (fid == F_JSONB_EXISTS)
	{
		if (!IsA(keys_arg, Const) || ((Const *) keys_arg)->constisnull)
			return NULL;
		keys = list_make1(makeString(TextDatumGetCString(((Const *) keys_arg)->constvalue)));
	}
	else if (!path_keys(keys_arg, &keys) || keys == NIL)
		return NULL;
	if ((grp = group_leaf_of(c, target, SCL_ARRAY)) < 0)
		return NULL;
	n = list_length(keys);
	p = n > 1 ? new_junction(c, fid == F_JSONB_EXISTS_ALL ? SAP_AND : SAP_OR, n) : NULL;
	for (int i = 0; i < n; i++)
	{
		const char *key = strVal(list_nth(keys, i));
		Lit			lit = {'s', key, strlen(key)};
		SiftAggPred *e = any_elem(c, grp, build_elem_lit, &lit);

		if (e == NULL)
			return NULL;
		if (p)
			p->child[i] = e;
		else
			p = e;
	}
	if (n > 1)
		junction_done(p);
	return p;
}
