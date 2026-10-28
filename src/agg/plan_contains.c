/*
 * agg/plan_contains.c - contains and key tests on jsonb arrays: col @> '[{...}]', ?|, ?& (agg/plan_exists.c).
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

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
	if (n > 1 && fid == F_JSONB_EXISTS_ANY)
	{
		Lit		   *lits = palloc(n * sizeof(Lit));

		for (int i = 0; i < n; i++)
			lits[i] = (Lit) {'s', strVal(list_nth(keys, i)), strlen(strVal(list_nth(keys, i)))};
		if ((p = any_elem_in(c, grp, lits, n)) != NULL)
			return p;
	}
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

/* ---- native arrays whose elements the index holds as a list (reloption paths names the column): @>, && and = ANY ---- */

/* the list leaf of the array column an expression is (-1: none) */
static int
array_group(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, strip_relabel(e));

	for (int i = 0; att > 0 && i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_ARRAY && !l->lossy)
			return i;
	}
	return -1;
}

/* a constant of the array's element type as the literal its element leaf is compared with; false: not proven (a NULL, a collation that is not bytewise) */
static bool
elem_lit(Oid elem, Oid collid, Datum d, Lit *lit)
{
	if (elem == TEXTOID || elem == VARCHAROID)
	{
		if (OidIsValid(collid) && !get_collation_isdeterministic(collid))
			return false;
		d = PointerGetDatum(pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d)));
		lit->kind = 's';
		lit->s = VARDATA_ANY(DatumGetPointer(d));
		lit->len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
		return true;
	}
	lit->kind = 'n';
	lit->num = DatumGetNumeric(DirectFunctionCall1(elem == INT2OID ? int2_numeric : elem == INT4OID ? int4_numeric : int8_numeric, d));
	return true;
}

/* the equality of an element type the list leaf holds */
static bool
array_eq_proc(Oid proc)
{
	return proc == F_TEXTEQ || proc == F_INT2EQ || proc == F_INT4EQ || proc == F_INT8EQ;
}

/* the test of the elements of a constant array against the list leaf: for all of them (@>) or for one (&&, = ANY) */
static SiftAggPred *
array_elems(AggCtx *c, int grp, Const *arr, Oid collid, bool all)
{
	ArrayType  *a;
	Oid			elem;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *vals;
	bool	   *nulls;
	int			n;
	SiftAggPred *p = NULL;

	if (arr->constisnull)
		return NULL;
	a = DatumGetArrayTypeP(arr->constvalue);
	elem = ARR_ELEMTYPE(a);
	get_typlenbyvalalign(elem, &len, &byval, &align);
	deconstruct_array(a, elem, len, byval, align, &vals, &nulls, &n);
	if (n == 0)
		return NULL;
	if (!all && n > 1 && (elem == TEXTOID || elem == VARCHAROID))
	{
		Lit		   *lits = palloc(n * sizeof(Lit));

		for (int i = 0; i < n; i++)
			if (nulls[i] || !elem_lit(elem, collid, vals[i], &lits[i]))
				return NULL;
		return any_elem_in(c, grp, lits, n);
	}
	if (n > 1)
		p = new_junction(c, all ? SAP_AND : SAP_OR, n);
	for (int i = 0; i < n; i++)
	{
		Lit			lit = {0};
		SiftAggPred *e;

		if (nulls[i] || !elem_lit(elem, collid, vals[i], &lit) || (e = any_elem(c, grp, build_elem_lit, &lit)) == NULL)
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

/* col <@ const of text elements: the array is not NULL and none of its elements is NULL or outside the set (an empty array is contained by any set) */
static SiftAggPred *
array_contained(AggCtx *c, int grp, Const *arr, Oid collid)
{
	ArrayType  *a;
	Oid			elem;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *vals;
	bool	   *nulls;
	int			n,
				m = 0;
	Lit		   *lits;
	SiftAggPred *out,
			   *both,
			   *present;

	if (arr->constisnull)
		return NULL;
	a = DatumGetArrayTypeP(arr->constvalue);
	elem = ARR_ELEMTYPE(a);
	if (!sift_shred_array_elem_ok(elem))
		return NULL;
	get_typlenbyvalalign(elem, &len, &byval, &align);
	deconstruct_array(a, elem, len, byval, align, &vals, &nulls, &n);
	lits = palloc(Max(n, 1) * sizeof(Lit));
	/* a NULL in the set equals no element: it is left out */
	for (int i = 0; i < n; i++)
		if (!nulls[i] && !elem_lit(elem, collid, vals[i], &lits[m++]))
			return NULL;
	if (m == 0 || (out = any_elem_outside(c, grp, lits, m)) == NULL)
		return NULL;
	out->op = 1;
	out->desc = psprintf("NOT %s", out->desc);
	present = make_leaf_pred(c, SAP_NULL, grp);
	present->op = 1;
	present->desc = psprintf("%s IS NOT NULL", leaf_name(c, grp));
	both = new_junction(c, SAP_AND, 2);
	both->child[0] = present;
	both->child[1] = out;
	junction_done(both);
	return both;
}

/* ---- col = const on an array column: the rows with its cardinality and elements, then the elements in order ---- */

/* x = const (either side) of an array_eq operator: the column's expression and the constant */
static bool
array_eq_args(OpExpr *op, Node **x, Const **k)
{
	Node	   *l,
			   *r;

	if (op->opfuncid != F_ARRAY_EQ || list_length(op->args) != 2)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(r, Const) && !IsA(l, Const))
	{
		*x = l;
		*k = (Const *) r;
	}
	else if (IsA(l, Const) && !IsA(r, Const))
	{
		*x = r;
		*k = (Const *) l;
	}
	else
		return false;
	return !(*k)->constisnull;
}

/* cardinality(x) = n: the lane after the image of an array that keeps its lengths; *lane is its leaf */
static SiftAggPred *
array_card_eq(AggCtx *c, Node *x, int n, int *lane)
{
	Node	   *call = (Node *) makeFuncExpr(F_CARDINALITY, INT4OID, list_make1(x), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	Lit			lit = {'n'};

	if ((*lane = match_leaf(c, call)) < 0)
		return NULL;
	lit.num = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(n)));
	return leaf_lit_cmp(c, *lane, SCMP_EQ, &lit);
}

/* the elements of the constant of x = const as literals, in order: NULL when the lists cannot decide it (a NULL element, another dimension or bound than one from 1, more than max elements) */
static Lit *
array_eq_lits(Const *k, Oid collid, int max, int *n)
{
	ArrayType  *a = DatumGetArrayTypeP(k->constvalue);
	Oid			elem = ARR_ELEMTYPE(a);
	int16		len;
	bool		byval;
	char		align;
	Datum	   *vals;
	bool	   *nulls;
	Lit		   *lits;

	if (ARR_NDIM(a) > 1 || (ARR_NDIM(a) == 1 && ARR_LBOUND(a)[0] != 1) || !sift_shred_array_elem_ok(elem))
		return NULL;
	get_typlenbyvalalign(elem, &len, &byval, &align);
	deconstruct_array(a, elem, len, byval, align, &vals, &nulls, n);
	if (*n > max)
		return NULL;
	lits = palloc(Max(*n, 1) * sizeof(Lit));
	for (int i = 0; i < *n; i++)
		if (nulls[i] || !elem_lit(elem, collid, vals[i], &lits[i]))
			return NULL;
	return lits;
}

#define ARRAY_EQ_ELEMS	4		/* the elements of the constant the point directory narrows the rows by: the rows own all of them */

/* the first ARRAY_EQ_ELEMS different elements of lits as tests of the list leaf grp, appended to *conj */
static void
array_eq_elems(AggCtx *c, int grp, const Lit *lits, int n, List **conj)
{
	int			done = 0;

	for (int i = 0; i < n && done < ARRAY_EQ_ELEMS; i++)
	{
		bool		seen = false;
		SiftAggPred *e;

		for (int j = 0; j < i && !seen; j++)
			seen = lits[j].kind == lits[i].kind && (lits[i].kind == 's' ? lits[j].len == lits[i].len && memcmp(lits[j].s, lits[i].s, lits[i].len) == 0 :
													DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(lits[j].num), NumericGetDatum(lits[i].num))));
		if (!seen && (e = any_elem(c, grp, build_elem_lit, (void *) &lits[i])) != NULL)
		{
			*conj = lappend(*conj, e);
			done++;
		}
	}
}

static SiftAggPred *
and_of(AggCtx *c, List *conj)
{
	SiftAggPred *p;
	int			i = 0;

	if (list_length(conj) == 1)
		return linitial(conj);
	p = new_junction(c, SAP_AND, list_length(conj));
	foreach_ptr(SiftAggPred, e, conj)
		p->child[i++] = e;
	junction_done(p);
	return p;
}

/*
 * The conjuncts that x = const (an array column, a constant) comes to: the rows it keeps have the cardinality and the elements of the constant, which
 * narrow the rows before anything else is read (the point directory of the lists lists the rows that own the elements). *complete: they decide it, for
 * an index that keeps the elements of its arrays and no image: a one-dimensional array from 1 has no head there, any other keeps it, so the rows
 * of a head that is NULL are the arrays the lists tell, and their elements are read in order. The rows of a NULL array have no cardinality and
 * no test passes them. Otherwise the caller adds the test of the image after them. NIL: none.
 */
List *
array_eq_conjuncts(AggCtx *c, Node *e, bool *complete)
{
	OpExpr	   *op = (OpExpr *) e;
	Node	   *x;
	Const	   *k;
	ArrayType  *a;
	List	   *conj = NIL;
	SiftAggPred *card;
	int			lane,
				n = 0,
				grp;
	Lit		   *lits;

	*complete = false;
	if (!IsA(e, OpExpr) || !array_eq_args(op, &x, &k))
		return NIL;
	a = DatumGetArrayTypeP(k->constvalue);
	if ((card = array_card_eq(c, x, ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a)), &lane)) == NULL)
		return NIL;
	conj = list_make1(card);
	lits = array_eq_lits(k, op->inputcollid, INT_MAX, &n);
	grp = lits != NULL ? array_group(c, x) : -1;
	if (grp >= 0 && get_element_type(exprType(x)) != ARR_ELEMTYPE(a))
		grp = -1;
	if (grp >= 0)
		array_eq_elems(c, grp, lits, n, &conj);
	if (leaf_of(c, lane - 1)->noimage && lits != NULL && n <= SIFT_AGG_SEQ_ELEMS && (n == 0 || grp >= 0))
	{
		SiftAggPred *head,
				   *seq;

		if (n > 0)
		{
			head = make_leaf_pred(c, SAP_NULL, lane - 1);
			head->desc = psprintf("%s IS NULL", leaf_name(c, lane - 1));
			if ((seq = any_elem_seq(c, grp, build_elem_lit, lits, n)) == NULL)
				return NIL;
			conj = lcons(card, lcons(head, list_delete_first(conj)));
			conj = lappend(conj, seq);
		}
		*complete = true;
	}
	return conj;
}

/*
 * col @> const, const <@ col, col && const and const = ANY (col) on an array column: some element passes the equality of the list leaf.
 * The result is NULL for a NULL array, as WHERE treats it (*target, for the negation); = ANY is also NULL for a NULL element, so its target is
 * the expression itself, never proven not NULL.
 */
SiftAggPred *
compile_array_test(AggCtx *c, Node *n, Node **target)
{
	Node	   *arr,
			   *other;
	Oid			fid,
				collid;
	bool		all,
				contained;
	int			grp;

	if (IsA(n, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;
		Node	   *x = strip_relabel(linitial(sa->args));
		Lit			lit = {0};
		Oid			xt = exprType(x);

		if (!sa->useOr || list_length(sa->args) != 2 || !IsA(x, Const) || ((Const *) x)->constisnull ||
			!array_eq_proc(get_opcode(sa->opno)) ||
			(grp = array_group(c, lsecond(sa->args))) < 0 || !sift_shred_array_elem_ok(get_element_type(exprType(lsecond(sa->args)))) ||
			get_element_type(exprType(lsecond(sa->args))) != (xt == VARCHAROID ? VARCHAROID : xt) ||
			!elem_lit(xt, sa->inputcollid, ((Const *) x)->constvalue, &lit))
			return NULL;
		*target = n;
		return any_elem(c, grp, build_elem_lit, &lit);
	}
	if (IsA(n, OpExpr) && ((OpExpr *) n)->opfuncid == F_ARRAY_EQ)
	{
		bool		complete;
		List	   *conj = array_eq_conjuncts(c, n, &complete);

		if (!complete)
			return NULL;
		return and_of(c, conj);
	}
	fid = func_of(n);
	if (fid != F_ARRAYCONTAINS && fid != F_ARRAYCONTAINED && fid != F_ARRAYOVERLAP)
		return NULL;
	arr = strip_relabel(linitial(func_args(n)));
	other = strip_relabel(lsecond(func_args(n)));
	if (fid == F_ARRAYOVERLAP && IsA(arr, Const))
	{
		Node	   *t = arr;

		arr = other;
		other = t;
	}
	/* const <@ col is col @> const; col <@ const is the one that needs every element in the set */
	contained = fid == F_ARRAYCONTAINED && !IsA(arr, Const);
	if (fid == F_ARRAYCONTAINED && !contained)
	{
		Node	   *t = arr;

		arr = other;
		other = t;
	}
	all = fid != F_ARRAYOVERLAP;
	collid = IsA(n, OpExpr) ? ((OpExpr *) n)->inputcollid : ((FuncExpr *) n)->inputcollid;
	if (!IsA(other, Const) || (grp = array_group(c, arr)) < 0 || !sift_shred_array_elem_ok(get_element_type(exprType(arr))) ||
		get_element_type(exprType(arr)) != get_element_type(((Const *) other)->consttype))
		return NULL;
	*target = arr;
	if (contained)
		return array_contained(c, grp, (Const *) other, collid);
	*target = arr;
	return array_elems(c, grp, (Const *) other, collid, all);
}
