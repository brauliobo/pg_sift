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

/* every object and scalar of the array pattern is contained by some element of the array leaf grp */
static SiftAggPred *
contains_elems(AggCtx *c, int grp, JsonbContainer *arr)
{
	JsonbIterator *it;
	JsonbValue	v;
	SiftAggPred *p = NULL;
	int			n = JsonContainerSize(arr);

	if (n == 0)
		return NULL;
	if (n > 1)
		p = new_junction(c, SAP_AND, n);
	it = JsonbIteratorInit(arr);
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

/* the stored leaf of kind `kind` at `path` below the jsonb attribute att; a text leaf is the strings only, or with astext the text of any scalar */
static int
root_leaf(AggCtx *c, int att, List *path, SiftSrcLeafKind kind, bool astext)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == kind && !l->lossy && l->name[0] != '#' && (kind != SCL_SCALAR || l->astext == astext) &&
			paths_equal(l->path, path))
			return i;
	}
	return -1;
}

/*
 * attribute @> '{"k": 1, "o": {"a": "x"}, "t": ["u"]}': the members are tests of the leaves of the paths below the attribute, as `x->'k'` is one: a scalar
 * equals the leaf, an object is the same of its members, an array is contained by the array leaf of its path. A JSON null, a number a leaf of its
 * type cannot hold exactly, an empty object or array and a path without a leaf are not proven.
 */
static SiftAggPred *
build_object(AggCtx *c, int att, List *path, JsonbContainer *obj)
{
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
		List	   *mp;
		SiftAggPred *e;
		Lit			lit;

		(void) JsonbIteratorNext(&it, &k, true);
		(void) JsonbIteratorNext(&it, &v, true);
		mp = lappend(list_copy(path), makeString(pnstrdup(k.val.string.val, k.val.string.len)));
		if (v.type == jbvBinary && JsonContainerIsObject(v.val.binary.data))
			e = build_object(c, att, mp, v.val.binary.data);
		else if (v.type == jbvBinary && JsonContainerIsArray(v.val.binary.data) && !JsonContainerIsScalar(v.val.binary.data))
		{
			int			grp = root_leaf(c, att, mp, SCL_ARRAY, false);

			e = grp < 0 ? NULL : contains_elems(c, grp, v.val.binary.data);
		}
		else if (jsonb_lit(&v, &lit) && lit.kind != 'z')
		{
			int			leaf = root_leaf(c, att, mp, SCL_SCALAR, false);

			if (leaf < 0 && lit_is_plain_text(&lit))
				leaf = root_leaf(c, att, mp, SCL_SCALAR, true);
			e = leaf_lit_cmp(c, leaf, SCMP_EQ, &lit);
		}
		else
			e = NULL;
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

SiftAggPred *
compile_contains(AggCtx *c, Node *target, Node *rhs)
{
	Jsonb	   *jb;
	JsonRef		r = {0};
	int			grp;

	rhs = strip_relabel(rhs);
	if (!IsA(rhs, Const) || ((Const *) rhs)->constisnull)
		return NULL;
	jb = DatumGetJsonbP(((Const *) rhs)->constvalue);
	if (JB_ROOT_IS_OBJECT(jb))
		return json_access(c, target, &r) && !r.as_text && r.att > 0 ? build_object(c, r.att, r.path, &jb->root) : NULL;
	if (!JB_ROOT_IS_ARRAY(jb) || JB_ROOT_IS_SCALAR(jb) || (grp = group_leaf_of(c, target, SCL_ARRAY)) < 0)
		return NULL;
	return contains_elems(c, grp, &jb->root);
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
