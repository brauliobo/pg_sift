/*
 * agg/plan_exists.c - existential predicates: any element, EXISTS, jsonb_path_exists, @>, ?| and dependency recording
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- existential predicates: some element of one array passes (SAP_ANY_ELEM) ---------- */

/*
 * The element predicate is compiled with the array in scope as the query's group (so its leaves are element leaves of that array,
 * not a second unnest) and with its own set of leaves; whatever it changes but the counters is undone.
 */
static AggCtx
scope_enter(AggCtx *c, int grp)
{
	AggCtx		saved = *c;

	c->group = grp;
	c->used = NULL;
	return saved;
}

static void
scope_leave(AggCtx *c, const AggCtx *saved, int grp)
{
	c->elem_used = bms_join(c->elem_used, c->used);
	c->used = bms_add_member(saved->used, grp);
	c->group = saved->group;
	c->func_rti = saved->func_rti;
	c->func = saved->func;
	c->func_kind = saved->func_kind;
	c->rtable = saved->rtable;
	c->dpctx = saved->dpctx;
	c->oparams = saved->oparams;
}

typedef SiftAggPred *(*ElemBuild) (AggCtx *c, int grp, void *arg);

/* some element of array leaf grp passes the predicate that build compiles over the leaves of its elements */
static SiftAggPred *
any_elem(AggCtx *c, int grp, ElemBuild build, void *arg)
{
	SiftAggPred *p = new_pred(c, SAP_ANY_ELEM),
			   *e;
	int			n0 = c->npred;
	AggCtx		saved = scope_enter(c, grp);

	e = build(c, grp, arg);
	scope_leave(c, &saved, grp);
	if (e == NULL)
		return NULL;
	p->leaf = grp;
	p->nchild = 1;
	p->child = palloc(sizeof(SiftAggPred *));
	p->child[0] = e;
	c->elem_work += (c->npred - n0) * elems_of(c, grp);
	p->desc = psprintf("EXISTS %s[] (%s)", leaf_name(c, grp), e->desc);
	return p;
}

/* a value the element leaf is compared with: a JSON string, number or boolean */
typedef struct Lit
{
	char		kind;			/* 's', 'n', 'b', 'z' (JSON null) */
	const char *s;
	int			len;
	Numeric		num;
	bool		b;
} Lit;

static int
child_named(AggCtx *c, int group, const char *name, bool astext)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && l->astext == astext && l->kind == SCL_SCALAR && strcmp(l->name, name) == 0)
			return i;
	}
	return -1;
}

/* a string no number or boolean prints as: the text of an astext leaf equals it for a JSON string only */
static bool
lit_is_plain_text(const Lit *lit)
{
	return lit->kind == 's' && (lit->len == 0 || (!isdigit((unsigned char) lit->s[0]) && lit->s[0] != '-')) &&
		!(lit->len == 4 && memcmp(lit->s, "true", 4) == 0) && !(lit->len == 5 && memcmp(lit->s, "false", 5) == 0);
}

/* element leaf (JSON member `name`) op lit with the semantics of a jsonpath / containment comparison of JSON values, or NULL when not provable */
static SiftAggPred *
lit_cmp(AggCtx *c, int grp, const char *name, int op, const Lit *lit)
{
	int			leaf = child_named(c, grp, name, false);
	SiftAggPred *p;
	Const	   *k;
	Oid			typid;

	/* the text leaf of any scalar answers an equality with a string of its own kind: the comparison of another JSON type is unknown, not equal */
	if (leaf < 0 && op == SCMP_EQ && lit_is_plain_text(lit))
		leaf = child_named(c, grp, name, true);
	if (leaf < 0)
		return NULL;
	typid = leaf_of(c, leaf)->typid;
	if (lit->kind == 's' && typid == TEXTOID && (op == SCMP_EQ || op == SCMP_NE) && lut_leaf_ok(c, leaf))
	{
		/* bytewise equality: the C collation is deterministic whatever the database collation is */
		Param	   *pm = makeNode(Param);
		char	   *str = pnstrdup(lit->s, lit->len);
		Oid			eq = get_opfamily_member(TEXT_BTREE_FAM_OID, TEXTOID, TEXTOID, BTEqualStrategyNumber);
		OpExpr	   *e;

		pm->paramkind = PARAM_EXEC;
		pm->paramtype = TEXTOID;
		pm->paramtypmod = -1;
		pm->paramcollid = C_COLLATION_OID;
		pm->location = -1;
		k = makeConst(TEXTOID, -1, C_COLLATION_OID, -1, PointerGetDatum(cstring_to_text_with_len(lit->s, lit->len)), false, false);
		e = (OpExpr *) make_opclause(op == SCMP_NE ? get_negator(eq) : eq, BOOLOID, false, (Expr *) pm, (Expr *) k, InvalidOid, C_COLLATION_OID);
		set_opfuncid(e);
		p = make_leaf_pred(c, SAP_LUT, leaf);
		p->expr = (Node *) e;
		p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), quote_literal_cstr(str));
		return p;
	}
	/* jsonb numbers are numerics: only an integer leaf and an integral literal compare exactly (a float8 leaf holds the rounded value) */
	if (lit->kind == 'n' && (typid == INT2OID || typid == INT4OID || typid == INT8OID))
	{
		bool		err;
		int64		v = sift_numeric_int8(lit->num, &err);

		if (err || !DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(lit->num),
													 DirectFunctionCall1(int8_numeric, Int64GetDatum(v)))))
			return NULL;
		k = makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(v), false, FLOAT8PASSBYVAL);
	}
	else if (lit->kind == 'b' && typid == BOOLOID)
		k = makeConst(BOOLOID, -1, InvalidOid, 1, BoolGetDatum(lit->b), false, true);
	else
		return NULL;
	p = make_leaf_pred(c, SAP_CMP, leaf);
	p->op = op;
	p->expr = (Node *) k;
	p->argtype = k->consttype;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), expr_text(c, (Node *) k));
	return p;
}

static SiftAggPred *
or_of(AggCtx *c, SiftAggPred *a, SiftAggPred *b)
{
	SiftAggPred *p;

	if (a == NULL || b == NULL)
		return NULL;
	p = new_junction(c, SAP_OR, 2);
	p->child[0] = a;
	p->child[1] = b;
	junction_done(p);
	return p;
}

static bool
source_can_jnull(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_jnull(get_rel_relam(c->indexoid), true) : sift_colsrc_can_jnull(get_rel_relam(c->heapoid), false);
}

/* the NULL slots of an element leaf by what they are (SAP_NULL ops); a JSON null and a missing key need the kinds of the source */
static SiftAggPred *
null_pred(AggCtx *c, int leaf, int op)
{
	static const char *const what[] = {"IS NULL", "IS NOT NULL", "IS JSON NULL", "IS MISSING"};
	SiftAggPred *p;

	/* a segment that did not keep the kinds cannot tell them: the normal plan, which reads the documents (docs/COL.md 2.2) */
	if (op >= SNULL_JSON && (!source_can_jnull(c) || c->stats.nokinds[leaf]))
		return NULL;
	p = make_leaf_pred(c, SAP_NULL, leaf);
	p->op = op;
	p->desc = psprintf("%s %s", leaf_name(c, leaf), what[op]);
	return p;
}

static int
negate_op(int op)
{
	static const int neg[] = {SCMP_NE, SCMP_EQ, SCMP_GE, SCMP_GT, SCMP_LE, SCMP_LT};

	return neg[op];
}

/*
 * `@.name op lit` as a jsonpath filter has it, or with neg its negation `!(@.name op lit)`, on the element leaf of the member. The leaf
 * holds the members of its type and sets the others aside (overflow, an error for a scan that reads the leaf), so what is left is a value
 * of the type of the literal, a JSON null or a missing key (both NULL slots): a comparison is false for a missing key and for a JSON null
 * (true for != alone), so the negation is true for both (false for a JSON null under != ). Never unknown, which `!` could not negate.
 */
static SiftAggPred *
lit_test(AggCtx *c, int grp, const char *name, int op, const Lit *lit, bool neg)
{
	int			leaf = child_named(c, grp, name, false);

	if (leaf < 0)
		return !neg && op == SCMP_EQ ? lit_cmp(c, grp, name, op, lit) : NULL;
	if (lit->kind == 'z')
	{
		if (op != SCMP_EQ && op != SCMP_NE)
			return NULL;
		/* a JSON null equals only a JSON null; every other value, a missing key apart, differs from it */
		if (op == SCMP_EQ)
			return neg ? or_of(c, null_pred(c, leaf, 1), null_pred(c, leaf, SNULL_MISSING)) : null_pred(c, leaf, SNULL_JSON);
		return null_pred(c, leaf, neg ? 0 : 1);
	}
	if (!neg && op != SCMP_NE)
		return lit_cmp(c, grp, name, op, lit);
	if (!neg)
		return or_of(c, lit_cmp(c, grp, name, SCMP_NE, lit), null_pred(c, leaf, SNULL_JSON));
	if (op == SCMP_NE)
		return or_of(c, lit_cmp(c, grp, name, SCMP_EQ, lit), null_pred(c, leaf, SNULL_MISSING));
	return or_of(c, lit_cmp(c, grp, name, negate_op(op), lit), null_pred(c, leaf, 0));
}

/* ---- EXISTS (SELECT 1 FROM <unnest of the array> AS t(..) WHERE <element conjunction>) ---- */

typedef struct ExistsArg
{
	Query	   *q;
	RangeTblFunction *f;
	FuncKind	kind;
	List	   *params;			/* OuterParam: the columns of the outer row the sub-query's parameters stand for */
} ExistsArg;

/* the query of a correlated EXISTS sub-plan, if it is the plain shape: one unnesting function and a WHERE */
static Query *
exists_query(AggCtx *c, SubPlan *sp)
{
	Query	   *q;
	RangeTblEntry *rte;

	if (sp->subLinkType != EXISTS_SUBLINK || sp->testexpr || sp->useHashTable || sp->setParam || sp->parParam == NIL ||
		list_length(sp->parParam) != list_length(sp->args) || sp->plan_id < 1 ||
		sp->plan_id > list_length(c->root->glob->subroots))
		return NULL;
	q = ((PlannerInfo *) list_nth(c->root->glob->subroots, sp->plan_id - 1))->parse;
	if (q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->groupClause || q->havingQual ||
		q->distinctClause || q->limitCount || q->limitOffset || q->setOperations || q->cteList || q->rowMarks ||
		list_length(q->rtable) != 1 || list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = linitial_node(RangeTblEntry, q->rtable);
	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 ||
		contain_volatile_functions(q->jointree->quals))
		return NULL;
	return q;
}

/* the outer expressions the sub-plan's parameters stand for */
static Node *
outer_mutator(Node *n, SubPlan *sp)
{
	if (n && IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		ListCell   *lp,
				   *la;

		forboth(lp, sp->parParam, la, sp->args)
			if (lfirst_int(lp) == ((Param *) n)->paramid)
				return copyObject(lfirst(la));
	}
	return expression_tree_mutator(n, outer_mutator, sp);
}

/* the conjunction as one predicate */
static SiftAggPred *
compile_conj(AggCtx *c, List *quals)
{
	SiftAggPred *p;

	if (quals == NIL)
	{
		p = new_pred(c, SAP_TRUE);
		p->desc = pstrdup("true");
		return p;
	}
	return list_length(quals) == 1 ? compile_pred(c, linitial(quals)) : compile_junction(c, SAP_AND, quals);
}

static SiftAggPred *
build_exists(AggCtx *c, int grp, void *arg)
{
	ExistsArg  *x = arg;

	c->func_rti = 1;
	c->func = x->f;
	c->func_kind = x->kind;
	c->rtable = x->q->rtable;
	c->dpctx = NIL;
	c->oparams = x->params;
	return compile_conj(c, (List *) x->q->jointree->quals);
}

/* the parameters of the sub-plan whose outer expression is a leaf, and the leaf (compile_elem_row takes the ones of the row) */
static List *
outer_params(AggCtx *c, SubPlan *sp)
{
	List	   *out = NIL;
	ListCell   *lp,
			   *la;

	forboth(lp, sp->parParam, la, sp->args)
	{
		int			leaf = match_leaf(c, lfirst(la));
		OuterParam *o;

		if (leaf < 0)
			continue;
		o = palloc(sizeof(OuterParam));
		o->id = lfirst_int(lp);
		o->leaf = leaf;
		out = lappend(out, o);
	}
	return out;
}

static SiftAggPred *
compile_exists(AggCtx *c, SubPlan *sp)
{
	ExistsArg	x;
	Query	   *q = exists_query(c, sp);
	int			grp;

	if (q == NULL)
		return NULL;
	x.q = q;
	x.f = linitial_node(RangeTblFunction, linitial_node(RangeTblEntry, q->rtable)->functions);
	x.kind = func_kind_of(x.f);
	if (x.kind == FK_NONE)
		return NULL;
	x.params = outer_params(c, sp);
	grp = group_leaf_of(c, outer_mutator(linitial(((FuncExpr *) x.f->funcexpr)->args), sp), func_leaf_kind(x.kind));
	return grp < 0 ? NULL : any_elem(c, grp, build_exists, &x);
}

/* a JSON string, number, boolean or null as a literal an element leaf is compared with */
static bool
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
static SiftAggPred *
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

static SiftAggPred *
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
static SiftAggPred *
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

/*
 * The existential tests: EXISTS over the elements of an array, jsonb_path_exists / @? and @> on an array of objects. The result is
 * never NULL for EXISTS; the others are NULL for a NULL document, which the executor (like WHERE) treats as false: *target is that
 * document's expression, for the negation to prove it NOT NULL.
 */
SiftAggPred *
compile_existential(AggCtx *c, Node *n, Node **target)
{
	Oid			fid;

	*target = NULL;
	if (IsA(n, SubPlan))
		return compile_exists(c, (SubPlan *) n);
	fid = func_of(n);
	if (fid == F_JSONB_PATH_EXISTS || fid == F_JSONB_PATH_EXISTS_OPR)
	{
		*target = linitial(func_args(n));
		return compile_path_exists(c, n);
	}
	if (fid == F_JSONB_EXISTS || fid == F_JSONB_EXISTS_ANY || fid == F_JSONB_EXISTS_ALL)
	{
		*target = linitial(func_args(n));
		return compile_has_keys(c, fid, *target, lsecond(func_args(n)));
	}
	if (fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED)
	{
		bool		fwd = fid == F_JSONB_CONTAINS;

		*target = fwd ? linitial(func_args(n)) : lsecond(func_args(n));
		return compile_contains(c, *target, fwd ? lsecond(func_args(n)) : linitial(func_args(n)));
	}
	return NULL;
}

/* the document is a column with a NOT NULL constraint */
bool
target_not_null(AggCtx *c, Node *target)
{
	Var		   *v = (Var *) strip_relabel(target);

	return IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0 &&
		bms_is_member(v->varattno, c->rel->notnullattnums);
}

static bool
dep_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		record_plan_function_dependency(root, ((FuncExpr *) n)->funcid);
	else if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		record_plan_function_dependency(root, ((OpExpr *) n)->opfuncid);
	}
	else if (IsA(n, ScalarArrayOpExpr))
	{
		set_sa_opfuncid((ScalarArrayOpExpr *) n);
		record_plan_function_dependency(root, ((ScalarArrayOpExpr *) n)->opfuncid);
	}
	else if (IsA(n, CoerceViaIO) || IsA(n, ArrayCoerceExpr) || IsA(n, Aggref))
		;
	return expression_tree_walker(n, dep_walker, root);
}

void
record_deps(PlannerInfo *root, Node *n)
{
	dep_walker(n, root);
}

/* the tree of preds as one flat array, ids ascending, to count them */
void
pred_deps(PlannerInfo *root, SiftAggPred *p)
{
	if (p == NULL)
		return;
	record_deps(root, p->expr);
	for (int i = 0; i < p->nchild; i++)
		pred_deps(root, p->child[i]);
}

bool
out_walker(Node *n, OutWalk *w)
{
	if (n == NULL || w->bad)
		return false;
	foreach_ptr(Node, k, w->keys)
		if (equal(n, k))
			return false;
	if (IsA(n, Aggref) || IsA(n, GroupingFunc))
	{
		if (!list_member(w->aggs, n))
			w->aggs = lappend(w->aggs, n);
		return false;
	}
	if (IsA(n, Var) || IsA(n, PlaceHolderVar) || IsA(n, WindowFunc) || IsA(n, SubPlan) || IsA(n, SubLink))
	{
		w->bad = true;
		return false;
	}
	return expression_tree_walker(n, out_walker, w);
}
