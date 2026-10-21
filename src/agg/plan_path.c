/*
 * agg/plan_path.c - jsonb_path_exists / @? over the elements of an array leaf (agg/plan_exists.c).
 */
#include "postgres.h"

#include "utils/fmgroids.h"
#include "utils/jsonpath.h"

#include "agg/plan_exists_int.h"

/* a JSON string, number, boolean or null as a literal an element leaf is compared with */
bool
jsonb_lit(const JsonbValue *v, Lit *lit)
{
	if (v->type == jbvString)
		*lit = (Lit) {'s', v->val.string.val, v->val.string.len};
	else if (v->type == jbvNumeric)
		*lit = (Lit) {'n', NULL, 0, v->val.numeric};
	else if (v->type == jbvBool)
		*lit = (Lit) {'b', NULL, 0, NULL, v->val.boolean};
	else if (v->type == jbvNull)
		*lit = (Lit) {'z'};
	else
		return false;
	return true;
}

/* ---- jsonb_path_exists(col, '$[*] ? (<comparisons of @.key with literals, && ||>)') ---- */

static int
path_cmp_op(JsonPathItemType t)
{
	switch (t)
	{
		case jpiEqual:
			return SCMP_EQ;
		case jpiLess:
			return SCMP_LT;
		case jpiLessOrEqual:
			return SCMP_LE;
		case jpiGreater:
			return SCMP_GT;
		case jpiGreaterOrEqual:
			return SCMP_GE;
		case jpiNotEqual:
			return SCMP_NE;
		default:
			return -1;
	}
}

/* a literal of the path, or a variable of it with its value in the constant vars of the call (a variable it lacks is an error of the heap's plan) */
static bool
path_lit(JsonPathItem *v, const Jsonb *vars, Lit *lit)
{
	JsonbValue	jv;
	int32		len;
	const char *name;

	if (jspHasNext(v))
		return false;
	switch (v->type)
	{
		case jpiString:
			lit->kind = 's';
			lit->s = jspGetString(v, &lit->len);
			return true;
		case jpiNumeric:
			lit->kind = 'n';
			lit->num = jspGetNumeric(v);
			return true;
		case jpiBool:
			lit->kind = 'b';
			lit->b = jspGetBool(v);
			return true;
		case jpiNull:
			lit->kind = 'z';
			return true;
		case jpiVariable:
			name = jspGetString(v, &len);
			return vars != NULL && getKeyJsonValueFromContainer((JsonbContainer *) &vars->root, name, len, &jv) != NULL && jsonb_lit(&jv, lit);
		default:
			return false;
	}
}

/* @.key, as the name of the member */
static char *
path_key(JsonPathItem *v)
{
	JsonPathItem k;
	int32		len;
	char	   *name;

	if (v->type != jpiCurrent || !jspGetNext(v, &k) || k.type != jpiKey || jspHasNext(&k))
		return NULL;
	name = jspGetString(&k, &len);
	return pnstrdup(name, len);
}

typedef struct PathArg
{
	JsonPathItem *expr;
	const Jsonb *vars;			/* the constant variables of the call, NULL: none */
	bool		strict;			/* strict mode: a missing key is unknown, not false, which only a filter without ! cannot tell */
} PathArg;

/*
 * The filter expression as a predicate on the element leaves: comparisons of @.key with literals and variables, && || and ! (neg: under
 * an odd number of them, where the comparison is the negated one of lit_test and && and || swap). Every comparison of these is true or
 * false for a value the leaf holds (what it sets aside is an error of the scan), so ! is the complement and De Morgan holds.
 */
static SiftAggPred *
path_pred(AggCtx *c, int grp, JsonPathItem *v, const PathArg *a, bool neg)
{
	JsonPathItem l,
				r;
	SiftAggPred *p;
	int			op;
	char	   *key;
	Lit			lit;

	if (jspHasNext(v))
		return NULL;
	if (v->type == jpiNot)
	{
		jspGetArg(v, &l);
		return a->strict ? NULL : path_pred(c, grp, &l, a, !neg);
	}
	op = path_cmp_op(v->type);
	if (op < 0 && v->type != jpiAnd && v->type != jpiOr)
		return NULL;
	jspGetLeftArg(v, &l);
	jspGetRightArg(v, &r);
	if (op < 0)
	{
		p = new_junction(c, (v->type == jpiAnd) != neg ? SAP_AND : SAP_OR, 2);
		if ((p->child[0] = path_pred(c, grp, &l, a, neg)) == NULL || (p->child[1] = path_pred(c, grp, &r, a, neg)) == NULL)
			return NULL;
		junction_done(p);
		return p;
	}
	if ((key = path_key(&l)) != NULL && path_lit(&r, a->vars, &lit))
		;
	else if ((key = path_key(&r)) != NULL && path_lit(&l, a->vars, &lit))
		op = commute(op);
	else
		return NULL;
	return lit_test(c, grp, key, op, &lit, neg);
}

static SiftAggPred *
build_path(AggCtx *c, int grp, void *arg)
{
	PathArg    *a = arg;

	return path_pred(c, grp, a->expr, a, false);
}

/* the variables of jsonb_path_exists as an object constant (NULL with *ok: none; not ok: not a constant object) */
static const Jsonb *
path_vars(Node *vars, bool *ok)
{
	Jsonb	   *jb;

	vars = strip_relabel(vars);
	*ok = IsA(vars, Const) && !((Const *) vars)->constisnull;
	if (!*ok)
		return NULL;
	jb = DatumGetJsonbP(((Const *) vars)->constvalue);
	*ok = JB_ROOT_IS_OBJECT(jb);
	return *ok && JB_ROOT_COUNT(jb) > 0 ? jb : NULL;
}

/* `$[*] ? (filter)` on the array column, or `$.k.j[*] ? (filter)` on the array below keys of the object (lax only: a strict path raises for a missing key) */
SiftAggPred *
compile_path_exists(AggCtx *c, Node *n)
{
	List	   *args = func_args(n);
	Node	   *path = strip_relabel(lsecond(args));
	JsonPathItem root,
				any,
				filt,
				expr;
	JsonPath   *jp;
	PathArg		arg = {&expr, NULL, false};
	List	   *keys = NIL;
	bool		ok = true;
	int			grp;

	if (func_of(n) == F_JSONB_PATH_EXISTS)
	{
		if (list_length(args) != 4)
			return NULL;
		arg.vars = path_vars(lthird(args), &ok);
	}
	if (!ok || !IsA(path, Const) || ((Const *) path)->constisnull)
		return NULL;
	jp = DatumGetJsonPathP(((Const *) path)->constvalue);
	jspInit(&root, jp);
	arg.strict = !(jp->header & JSONPATH_LAX);
	if (root.type != jpiRoot || !jspGetNext(&root, &any))
		return NULL;
	for (; any.type == jpiKey && !arg.strict; jspGetNext(&any, &any))
	{
		int32		len;
		const char *key = jspGetString(&any, &len);

		keys = lappend(keys, makeString(pnstrdup(key, len)));
		if (!jspHasNext(&any))
			return NULL;
	}
	if (any.type != jpiAnyArray || !jspGetNext(&any, &filt) || filt.type != jpiFilter || jspHasNext(&filt) ||
		(grp = group_leaf_below(c, linitial(args), keys, SCL_ARRAY)) < 0)
		return NULL;
	jspGetArg(&filt, &expr);
	return any_elem(c, grp, build_path, &arg);
}
