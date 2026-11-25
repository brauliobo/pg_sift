/*
 * agg/v6/v6_jsonpath.c - the literals and the pieces of a jsonpath filter that the tests of the scan of an unnest are made of (agg/v6/v6_jsonpath.h)
 */
#include "postgres.h"

#include "nodes/primnodes.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"

#include "agg/kern6.h"
#include "agg/v6/v6_jsonpath.h"

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

int
jsonpath_cmp_op(JsonPathItemType t)
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
bool
jsonpath_lit(JsonPathItem *v, const Jsonb *vars, Lit *lit)
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
char *
jsonpath_key(JsonPathItem *v)
{
	JsonPathItem k;
	int32		len;
	char	   *name;

	if (v->type != jpiCurrent || !jspGetNext(v, &k) || k.type != jpiKey || jspHasNext(&k))
		return NULL;
	name = jspGetString(&k, &len);
	return pnstrdup(name, len);
}

/* the variables of jsonb_path_exists as an object constant (NULL with *ok: none; not ok: not a constant object) */
const Jsonb *
jsonpath_vars(Node *vars, bool *ok)
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
