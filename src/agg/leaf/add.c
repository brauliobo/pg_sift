/*
 * agg/leaf/add.c - adding a leaf to the column source: scalar, list, key and child leaves
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "utils/fmgroids.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/leaf/int.h"

int
scalar_leaf(AggCtx *c, int att, List *path, Oid typid)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' && l->arrlen == 0 && paths_equal(l->path, path) &&
			l->typid == typid)
			return i;
	}
	return -1;
}

/* The leaf that is NULL where the jsonb document e is SQL NULL (option document_present), -1 for another expression or an index without it */
int
present_leaf(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, strip_relabel(e));

	if (att > 0 && exprType(e) == JSONBOID)
		for (int i = 0; i < c->lay->nleaves; i++)
		{
			SiftSrcLeaf *l = leaf_of(c, i);

			if (l->attno == att && l->parent < 0 && strcmp(l->name, "#present") == 0)
				return i;
		}
	return -1;
}

/*
 * `cardinality(x)` and `array_length(x, 1)` of an array attribute that keeps its lengths (option array_lengths): the integer lane after its image, NULL
 * where PostgreSQL's function is. Any other dimension keeps the image.
 */
int
array_lane_leaf(AggCtx *c, Node *e, Oid fid)
{
	List	   *args = func_args(e);
	Node	   *arr = strip_relabel(linitial(args));
	int			att = attr_of_expr(c, arr);

	if (att == 0)
		return -1;
	if (fid == F_ARRAY_LENGTH)
	{
		Node	   *dim = strip_relabel(lsecond(args));

		if (!IsA(dim, Const) || ((Const *) dim)->constisnull || DatumGetInt32(((Const *) dim)->constvalue) != 1)
			return -1;
	}
	for (int i = 0; i < c->lay->nleaves - c->nvleaf; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->arrhead && l->parent < 0)
			return i + (fid == F_CARDINALITY ? 1 : 2);
	}
	return -1;
}

int
child_leaf(AggCtx *c, int group, const char *name, Oid typid)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && strcmp(l->name, name) == 0 && l->typid == typid)
			return i;
	}
	return -1;
}

/*
 * `(x->>'k')::float8` of an object leaf with free keys (`metrics.*`) at the row level: a scalar leaf after the source's, which the
 * executor builds from the "value" of the element of each row whose "key" is k.
 */
static SiftSrcLeaf *
vleaf_add(AggCtx *c, int att, List *path, SiftSrcVecType vtype, Oid typid)
{
	SiftSrcLayout *lay = palloc(sizeof(SiftSrcLayout));
	SiftSrcLeaf *v;

	*lay = *c->lay;
	lay->leaf = palloc((lay->nleaves + 1) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, c->lay->leaf, lay->nleaves * sizeof(SiftSrcLeaf));
	v = &lay->leaf[lay->nleaves];
	memset(v, 0, sizeof(SiftSrcLeaf));
	v->id = lay->nleaves++;
	v->attno = att;
	v->parent = -1;
	v->kind = SCL_SCALAR;
	v->vtype = vtype;
	v->typid = typid;
	v->path = path;
	c->lay = lay;
	return v;
}

int
key_leaf(AggCtx *c, int att, List *path, Oid typid)
{
	int			n = list_length(path),
				obj = -1,
				val;
	SiftSrcLeaf *v;

	if (n == 0 || agg_width_hit(c->nvleaf, virtual_leaves, sift_limit(virtual_leaves, SIFT_AGG_MAX_VLEAVES), "virtual leaves") || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->kind == SCL_OBJECT && l->attno == att && l->parent < 0 && !l->lossy && list_length(l->path) == n - 1 &&
			paths_equal(l->path, list_copy_head(path, n - 1)))
			obj = i;
	}
	if (obj < 0 || (val = child_leaf(c, obj, "value", typid)) < 0)
		return -1;
	/* an expression is matched more than once (when its inputs are collected and when they are replaced): the same member is one leaf */
	for (int j = 0; j < c->nvleaf; j++)
		if (!c->vleaf[j].count && !c->vleaf[j].types && c->vleaf[j].obj == obj && strcmp(c->vleaf[j].key, strVal(llast(path))) == 0 &&
			leaf_of(c, c->lay->nleaves - c->nvleaf + j)->typid == typid)
			return c->lay->nleaves - c->nvleaf + j;
	v = vleaf_add(c, att, path, leaf_of(c, val)->vtype, typid);
	c->vleaf[c->nvleaf].obj = obj;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf].bound = 0;
	c->vleaf[c->nvleaf++].key = pstrdup(strVal(llast(path)));
	return v->id;
}

/*
 * `jsonb_typeof(x->'k')` of a typed scalar leaf: a scalar leaf after the source's, text, whose values the executor takes from the JSON types of the
 * values of the leaf (SiftAggVLeaf.types): the name of the type, NULL where the path has no value.
 */
int
types_leaf(AggCtx *c, Node *call)
{
	int			base = typeof_leaf(c, linitial(func_args(call)));
	SiftSrcLeaf *v;

	if (base < 0 || !source_can_types(c) || c->stats.nokinds[base])
		return -1;
	/* the types are those the chunks keep of the stored values: a row read again from the heap as text has the type of its text */
	c->txt_bad = bms_add_member(c->txt_bad, base);
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].types && c->vleaf[j].obj == base)
			return c->lay->nleaves - c->nvleaf + j;
	if (agg_width_hit(c->nvleaf, virtual_leaves, sift_limit(virtual_leaves, SIFT_AGG_MAX_VLEAVES), "virtual leaves") || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	v = vleaf_add(c, leaf_of(c, base)->attno, leaf_of(c, base)->path, SCV_CODE, TEXTOID);
	strlcpy(v->name, "#typeof", NAMEDATALEN);	/* not the scalar of the path: scalar_leaf skips it */
	c->vleaf[c->nvleaf].obj = base;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = true;
	c->vleaf[c->nvleaf].bound = 0;
	c->vleaf[c->nvleaf++].key = pstrdup("");
	return v->id;
}

/* `jsonb_array_length(x->'k')` of an array leaf: a scalar leaf after the source's, whose values the executor takes from the element counts of the array */
int
count_leaf(AggCtx *c, int grp)
{
	SiftSrcLeaf *v;
	int			nbase = c->lay->nleaves - c->nvleaf;

	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].count && c->vleaf[j].obj == grp)
			return nbase + j;
	if (agg_width_hit(c->nvleaf, virtual_leaves, sift_limit(virtual_leaves, SIFT_AGG_MAX_VLEAVES), "virtual leaves") || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	v = vleaf_add(c, leaf_of(c, grp)->attno, leaf_of(c, grp)->path, SCV_I32, INT4OID);
	strlcpy(v->name, "#count", NAMEDATALEN);	/* not the scalar of the path: scalar_leaf skips it */
	c->vleaf[c->nvleaf].obj = grp;
	c->vleaf[c->nvleaf].count = true;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf].bound = 0;
	c->vleaf[c->nvleaf++].key = pstrdup("");
	return v->id;
}

/*
 * lower(r) or upper(r) of a range column the index keeps lanes for: a scalar leaf after the source's, of the subtype, whose values the executor takes from the lane of that bound
 * (SiftAggVLeaf.bound; the lanes are the leaves after the head of the range). -1 for another call or column.
 */
int
bound_leaf(AggCtx *c, Node *e, int bound)
{
	Node	   *arg = strip_relabel(linitial(func_args(e)));
	int			att = attr_of_expr(c, arg),
				nbase = c->lay->nleaves - c->nvleaf,
				head = -1;
	Oid			sub;
	SiftSrcLeaf *v;

	for (int i = 0; att > 0 && i < nbase; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && l->shape == 1 && !l->lossy && l->typid == exprType(arg))
			head = i;
	}
	if (head < 0 || head + 2 >= nbase || !OidIsValid(sub = get_range_subtype(exprType(arg))))
		return -1;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].bound == bound && c->vleaf[j].obj == head)
			return nbase + j;
	if (agg_width_hit(c->nvleaf, virtual_leaves, sift_limit(virtual_leaves, SIFT_AGG_MAX_VLEAVES), "virtual leaves") || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	v = vleaf_add(c, att, leaf_of(c, head)->path, sift_aggbound_i32(sub) ? SCV_I32 : SCV_I64, sub);
	strlcpy(v->name, bound == 1 ? "#lower" : "#upper", NAMEDATALEN);
	c->vleaf[c->nvleaf].obj = head;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf].bound = bound;
	c->vleaf[c->nvleaf++].key = pstrdup("");
	return v->id;
}

/* the member of the row's object that `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` picks (NULL where the row has none), as a row level leaf */
int
member_leaf(AggCtx *c, Oid typid)
{
	const SiftSrcLeaf *obj = leaf_of(c, c->func_obj);

	if (child_leaf(c, c->func_obj, "value", typid) < 0)
		return -1;
	c->func_ovf = bms_add_member(c->func_ovf, c->func_obj);
	return key_leaf(c, obj->attno, lappend(list_copy(obj->path), makeString(pstrdup(c->func_key))), typid);
}

/* a column of the unnested function, as a leaf */
int
func_var_leaf(AggCtx *c, Var *v)
{
	RangeTblFunction *f = c->func;

	if (v->varlevelsup != 0 || v->varno != c->func_rti || c->group < 0)
		return -1;
	switch (c->func_kind)
	{
		case FK_RECORDSET:
			if (v->varattno < 1 || v->varattno > list_length(f->funccolnames) ||
				v->vartype != list_nth_oid(f->funccoltypes, v->varattno - 1))
				return -1;
			{
				int			leaf = child_leaf(c, c->group, strVal(list_nth(f->funccolnames, v->varattno - 1)), v->vartype);

				c->rec_pending = leaf + 1;
				return leaf;
			}
		case FK_EACH:
		case FK_EACH_TEXT:
			if (v->varattno == 1)
				return child_leaf(c, c->group, "key", TEXTOID);
			/* jsonb_each_text: the text of the value is the text of the member; a number has none in a text leaf, only in an astext one */
			if (c->func_kind == FK_EACH_TEXT && v->varattno == 2 && v->vartype == TEXTOID)
				return child_leaf(c, c->group, "value", TEXTOID);
			return -1;
		case FK_ELEMENTS_TEXT:		/* the element of an array of strings is the one unnamed child of the array */
			return v->varattno == 1 && v->vartype == TEXTOID ? child_leaf(c, c->group, "", TEXTOID) : -1;
		case FK_UNNEST:
			return v->varattno == 1 ? child_leaf(c, c->group, "", v->vartype) : -1;
		default:
			return -1;
	}
}
