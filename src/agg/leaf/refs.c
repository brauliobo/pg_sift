/*
 * agg/leaf/refs.c - names of the leaves and the small helpers on expressions that the other files share
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/leaf/int.h"

SiftSrcLeaf *
leaf_of(AggCtx *c, int id)
{
	return &c->lay->leaf[id];
}

int
leaf_level(AggCtx *c, int id)
{
	return leaf_of(c, id)->parent >= 0;
}

/* the name of an index attribute: its heap column, or the index expression */
static const char *
att_name(AggCtx *c, int attno)
{
	Node	   *x = list_nth(c->attexpr, attno - 1);
	List	   *rtable = c->rtable,
			   *dpctx = c->dpctx;
	char	   *text;

	if (x == NULL || IsA(x, Var))
		return x == NULL ? "?" : get_attname(c->heapoid, ((Var *) x)->varattno, false);
	/* the Vars of an index expression are the table's of the query, not of the sub-query whose predicate is being compiled */
	c->rtable = NIL;
	c->dpctx = NIL;
	text = expr_text(c, x);
	c->rtable = rtable;
	c->dpctx = dpctx;
	return text;
}

const char *
leaf_name(AggCtx *c, int id)
{
	StringInfoData s;
	SiftSrcLeaf *l = leaf_of(c, id);
	const char *att = att_name(c, l->attno);

	initStringInfo(&s);
	if (l->arrlen > 0)
	{
		appendStringInfo(&s, l->arrlen == 1 ? "cardinality(%s)" : "array_length(%s, 1)", att);
		return s.data;
	}
	if (l->parent >= 0)
	{
		const char *p = leaf_name(c, l->parent);

		appendStringInfo(&s, leaf_of(c, l->parent)->kind == SCL_OBJECT ? "%s.*.%s" : "%s[].%s", p, l->name);
		return s.data;
	}
	if (l->name[0] == '#')
		appendStringInfoString(&s, strcmp(l->name, "#typeof") == 0 ? "jsonb_typeof(" : "jsonb_array_length(");
	appendStringInfoString(&s, att);
	foreach_ptr(String, k, l->path)
		appendStringInfo(&s, ".%s", strVal(k));
	if (l->name[0] == '#')
		appendStringInfoChar(&s, ')');
	return s.data;
}

/* Local expression slots share parameter numbers with PostgreSQL, but not their identity. */
bool
null_input_param(Node *n)
{
	return IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC &&
		((Param *) n)->paramid == SIFT_AGG_NULL_INPUT && ((Param *) n)->location == SIFT_AGG_NULL_INPUT_LOC;
}

/* A condition's parameter that the scan reads from the executor when it starts: the rows an enclosing query level passes down set it (topn_level_params) */
bool
level_param(Node *n)
{
	return IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->location == SIFT_AGG_LEVEL_LOC;
}

/* `CASE WHEN <input absent> THEN NULL ELSE x END`, what a placeholder computed inside the nullable input becomes: x, else NULL */
Node *
null_guarded_arm(Node *n)
{
	CaseExpr   *ce;
	CaseWhen   *when;

	if (!IsA(n, CaseExpr) || (ce = (CaseExpr *) n)->arg != NULL || list_length(ce->args) != 1 || ce->defresult == NULL)
		return NULL;
	when = linitial_node(CaseWhen, ce->args);
	if (!null_input_param((Node *) when->expr) || !IsA(when->result, Const) || !((Const *) when->result)->constisnull)
		return NULL;
	return (Node *) ce->defresult;
}

/*
 * An expression the executor may evaluate once per scan: no Vars, no volatile function, no executor parameter (but those level_param marked). A non-NULL context (the query's
 * AggCtx) lets the EXISTS sub-plans through, and the outputs of the ARRAY sub-selects a lookup table asks once: the clause was compiled, and
 * compile_exists and array_initplan_ok proved the ones it kept.
 */
bool
has_exec_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan) && context)
		return false;
	if (level_param(node))
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC)
		return !context || !array_initplan_ok(context, (Param *) node);
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, Aggref) || IsA(node, WindowFunc))
		return true;
	return expression_tree_walker(node, has_exec_param_walker, context);
}

bool
pseudo_const(Node *n)
{
	return !contain_var_clause(n) && !contain_volatile_functions(n) && !has_exec_param_walker(n, NULL);
}

Node *
strip_relabel(Node *n)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

Oid
func_of(Node *n)
{
	if (IsA(n, FuncExpr))
		return ((FuncExpr *) n)->funcid;
	if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		return ((OpExpr *) n)->opfuncid;
	}
	return InvalidOid;
}

List *
func_args(Node *n)
{
	return IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : IsA(n, OpExpr) ? ((OpExpr *) n)->args : NIL;
}

bool
paths_equal(List *a, List *b)
{
	if (list_length(a) != list_length(b))
		return false;
	for (int i = 0; i < list_length(a); i++)
		if (strcmp(strVal(list_nth(a, i)), strVal(list_nth(b, i))) != 0)
			return false;
	return true;
}

/* the attribute whose index (table) expression is e, 0 if none */
int
attr_of_expr(AggCtx *c, Node *e)
{
	for (int a = 0; a < list_length(c->attexpr); a++)
	{
		Node	   *x = list_nth(c->attexpr, a);
		Node	   *y = c->attalias ? list_nth(c->attalias, a) : NULL;

		if ((x && equal(e, x)) || (y && equal(e, y)))
			return a + 1;
	}
	return 0;
}
