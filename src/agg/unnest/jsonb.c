/*
 * agg/unnest/jsonb.c - the lateral jsonb_to_recordset of the aggregate scan of an unnest (agg/unnest/jsonb.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, and the elements of one row of the table */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "agg/agg.h"
#include "agg/ht/ht.h"
#include "agg/gather.h"
#include "agg/plan/int.h"
#include "agg/datum.h"
#include "agg/expr/pg.h"
#include "agg/unnest/jsonb.h"
#include "agg/vexpr/str.h"
#include "col/am.h"

bool
sift_v6_leaf_holds(Oid typid, int32 typmod, const SiftColDef *d)
{
	if (typmod != -1 || d->kind != SIFT_COLK_SCALAR || d->lane != 0 || (d->flags & SIFT_COLD_LOSSY) ||
		((d->flags & SIFT_COLD_ASTEXT) && typid != TEXTOID && typid != VARCHAROID))
		return false;
	switch (typid)
	{
		case TEXTOID:
		case VARCHAROID:
			return d->store == SIFT_COLS_STR && d->type == SIFT_COLT_TEXT;
		case BOOLOID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_BOOL;
		case INT2OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT2;
		case INT4OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT4;
		case INT8OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT8;
		case FLOAT4OID:
			return d->store == SIFT_COLS_F4 && d->type == SIFT_COLT_FLOAT4;
		case FLOAT8OID:
			return d->store == SIFT_COLS_F8 && d->type == SIFT_COLT_FLOAT8;
		default:
			return false;
	}
}

/* the type of the column of the statement that holds the leaf as it is stored: text, an integer or a boolean (InvalidOid for any other leaf) */
static Oid
leaf_type(const SiftColDef *d)
{
	Oid			typ[] = {TEXTOID, INT2OID, INT4OID, INT8OID, BOOLOID};

	for (int i = 0; i < lengthof(typ); i++)
		if (sift_v6_leaf_holds(typ[i], -1, d))
			return typ[i];
	return InvalidOid;
}

bool
sift_v6_unnest_rte(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *rel, SiftV6ElemRte *out)
{
	return rel->joininfo == NIL && sift_v6_unnest_rte_joined(root, fact, rel, out);
}

/* doc -> 'key' or doc['key'] of a constant key: the document and the key */
static const char *
field_key(const Expr *e, const Expr **doc)
{
	const OpExpr *op = (const OpExpr *) e;
	const Const *k;
	const Var  *v;
	const char *key;

	if (IsA(e, SubscriptingRef))
	{
		if (!sift_v6_json_member(e, false, &v, &key))
			return NULL;
		*doc = (const Expr *) v;
		return key;
	}
	if (!IsA(e, OpExpr) || get_opcode(op->opno) != F_JSONB_OBJECT_FIELD || list_length(op->args) != 2)
		return NULL;
	k = lsecond(op->args);
	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	*doc = linitial(op->args);
	return TextDatumGetCString(k->constvalue);
}

int
sift_v6_unnest_rte_col(SiftV6ElemRte *rte, const char *name, Oid type, bool kind)
{
	int			at = 0;

	foreach_node(String, n, rte->names)
	{
		if (strcmp(strVal(n), name) == 0 && list_nth_oid(rte->types, at) == type && (rte->kinds != NIL && list_nth_int(rte->kinds, at) != 0) == kind)
			return at;
		at++;
	}
	if (at == SIFT_V6_XELEM_COLS)
		return -1;
	rte->names = lappend(rte->names, makeString(pstrdup(name)));
	rte->types = lappend_oid(rte->types, type);
	rte->kinds = lappend_int(rte->kinds, kind);
	return at;
}

void
sift_v6_unnest_array_rte(const Expr *array, SiftV6ElemRte *out)
{
	*out = (SiftV6ElemRte) {.fn = SV6E_PATH,.array = array,.base = array};
	out->key = field_key(array, &out->base);
}

/* the function of the lateral relation, taking the column array of the fact table: unnest of an array, or jsonb_to_recordset of a jsonb one with the columns it declares */
bool
sift_v6_unnest_rte_function(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out)
{
	const FuncExpr *fe = (const FuncExpr *) rtf->funcexpr;
	const Oid	vartype = exprType((const Node *) v);

	*out = (SiftV6ElemRte) {.array = v,.base = v};
	if (fe->funcid != F_UNNEST_ANYARRAY)
		out->key = field_key(v, &out->base);
	if (fe->funcid == F_UNNEST_ANYARRAY)
	{
		out->fn = SV6E_UNNEST;
		out->elem = get_element_type(vartype);
		return rtf->funccolcount == 1 && fe->funcresulttype == out->elem &&
			(out->elem == TEXTOID || out->elem == VARCHAROID || out->elem == INT2OID || out->elem == INT4OID || out->elem == INT8OID);
	}
	if (fe->funcid == F_JSONB_ARRAY_ELEMENTS_TEXT)
		return sift_v6_elems_rte(rtf, v, out);
	if (fe->funcid == F_JSONB_ARRAY_ELEMENTS)
		return sift_v6_members_rte(rtf, v, out);
	if (fe->funcid == F_JSONB_EACH || fe->funcid == F_JSONB_EACH_TEXT)
	{
		out->fn = fe->funcid == F_JSONB_EACH ? SV6E_EACH : SV6E_EACH_TEXT;
		return vartype == JSONBOID && rtf->funccolcount == 2;
	}
	if (fe->funcid != F_JSONB_TO_RECORDSET || vartype != JSONBOID || rtf->funccolcount < 1 || rtf->funccolcount > SIFT_V6_XELEM_COLS)
		return false;
	out->fn = SV6E_RECORDSET;
	out->names = rtf->funccolnames;
	for (int c = 0; c < rtf->funccolcount; c++)
	{
		const Oid	t = list_nth_oid(rtf->funccoltypes, c);

		if (list_nth_int(rtf->funccoltypmods, c) != -1)
			return false;
		out->types = lappend_oid(out->types, t);
	}
	return true;
}

bool
sift_v6_unnest_array_ok(PlannerInfo *root, const RelOptInfo *fact, const Expr *e)
{
	if (IsA(e, Var))
		return ((const Var *) e)->varno == fact->relid && ((const Var *) e)->varlevelsup == 0;
	return bms_equal(pull_varnos(root, (Node *) e), bms_make_singleton(fact->relid));
}

bool
sift_v6_unnest_rte_joined(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *rel, SiftV6ElemRte *out)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	const RangeTblFunction *rtf;
	const FuncExpr *fe;

	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 ||
		!bms_equal(rel->lateral_relids, bms_make_singleton(fact->relid)))
		return false;
	rtf = linitial(rte->functions);
	fe = (const FuncExpr *) rtf->funcexpr;
	if (!IsA(rtf->funcexpr, FuncExpr) || list_length(fe->args) != 1 || !sift_v6_unnest_array_ok(root, fact, linitial(fe->args)))
		return false;
	return sift_v6_unnest_rte_function(rtf, linitial(fe->args), out);
}

typedef struct OuterTerms
{
	const SubPlan *sp;
	bool		ok;
} OuterTerms;

/* the expression of the sub-select with the parameters its sub-plan passes as the outer expressions they stand for */
static Node *
outer_terms(Node *n, OuterTerms *c)
{
	if (n != NULL && IsA(n, Param))
	{
		const Param *pr = (const Param *) n;
		int			k = 0;

		foreach_int(id, c->sp->parParam)
		{
			if (pr->paramkind == PARAM_EXEC && pr->paramid == id)
				return copyObject(list_nth(c->sp->args, k));
			k++;
		}
		c->ok = false;
		return n;
	}
	return n == NULL ? NULL : expression_tree_mutator(n, outer_terms, c);
}

bool
sift_v6_unnest_exists_rte(const RangeTblEntry *rte, const SubPlan *sp, SiftV6ElemRte *out)
{
	const RangeTblFunction *rtf = rte->functions != NIL ? linitial(rte->functions) : NULL;
	OuterTerms	c = {.sp = sp,.ok = true};
	Node	   *array;

	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 || !IsA(rtf->funcexpr, FuncExpr) ||
		list_length(((const FuncExpr *) rtf->funcexpr)->args) != 1)
		return false;
	array = outer_terms(linitial(((const FuncExpr *) rtf->funcexpr)->args), &c);
	return c.ok && sift_v6_unnest_rte_function(rtf, (const Expr *) array, out);
}

/* the lengths leaf of the attribute's array (the document itself, or its key `key`) and its element leaves are the ones of a list of objects (no key leaf, which a map has) */
int
sift_v6_unnest_group_of(const SiftV6Leaves *r, int att, const char *key)
{
	const size_t len = key != NULL ? strlen(key) : 0;
	int			group = -1;

	foreach_att_leaf(i, r, att)
	{
		const SiftColDef *d = &r->schema->cols[i];
		const char *dot = d->kind == SIFT_COLK_GROUP && d->path != NULL ? strchr(d->path, '.') : NULL;

		if (d->kind == SIFT_COLK_GROUP && (key == NULL ? dot == NULL : dot != NULL && strncmp(dot + 1, key, len) == 0 && strcmp(dot + 1 + len, "[]") == 0))
			group = i;
	}
	return group;
}

int
sift_v6_unnest_member_leaf(const SiftV6Leaves *r, int att, int group, const char *name)
{
	foreach_att_leaf(i, r, att)
	{
		const SiftColDef *d = &r->schema->cols[i];
		const char *dot = d->path != NULL ? strrchr(d->path, '.') : NULL;

		if (d->parent == group && d->path != NULL && strcmp(dot != NULL ? dot + 1 : d->path, name) == 0 && strchr(name, '#') == NULL)
			return i;
	}
	return -1;
}

static bool
seed_recordset(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, const char *key, int att, List **names)
{
	const SiftV6Leaves *r = &p->leaves;
	int			c = 0;

	if (!r->json[att] || (e->group_leaf = sift_v6_unnest_group_of(r, att, key)) < 0)
		return false;
	foreach_att_leaf(i, r, att)
		if (r->schema->cols[i].parent == e->group_leaf && r->schema->cols[i].path != NULL && strstr(r->schema->cols[i].path, "#key") != NULL)
			return false;
	foreach_node(String, n, rte->names)
	{
		const int	leaf = sift_v6_unnest_member_leaf(r, att, e->group_leaf, strVal(n));
		const Oid	t = list_nth_oid(rte->types, c);
		const bool	kind = rte->kinds != NIL && list_nth_int(rte->kinds, c) != 0;

		if (leaf < 0 || (!kind && !sift_v6_leaf_holds(t, -1, &r->schema->cols[leaf])))
			return false;
		e->col_leaf[c] = leaf;
		e->col_type[c] = t;
		e->col_kind[c++] = kind;
		*names = lappend(*names, makeString(pstrdup(strVal(n))));
	}
	e->ncols = c;
	return true;
}

Oid
sift_v6_leaf_oid(const SiftColDef *d)
{
	static const Oid held[] = {TEXTOID, INT2OID, INT4OID, INT8OID, FLOAT8OID};

	for (int t = 0; t < lengthof(held); t++)
		if (sift_v6_leaf_holds(held[t], -1, d))
			return held[t];
	return InvalidOid;
}

/* the group of the map of attribute att: `col.* type` of the document itself (key NULL), or `col.key.* type` of its key `key` (-1: the attribute has none) */
static int
map_group_of(const SiftV6Leaves *r, int att, const char *key)
{
	const size_t len = key != NULL ? strlen(key) : 0;

	foreach_att_leaf(i, r, att)
	{
		const SiftColDef *d = &r->schema->cols[i];
		const char *dot = d->kind == SIFT_COLK_GROUP && d->path != NULL ? strchr(d->path, '.') : NULL;

		if (dot != NULL && (key == NULL ? strcmp(dot + 1, "*") == 0 : strncmp(dot + 1, key, len) == 0 && strcmp(dot + 1 + len, ".*") == 0))
			return i;
	}
	return -1;
}

/* the leaves of the map of attribute att that is the document itself (key NULL) or its key `key` */
static bool
map_leaves_of(const SiftV6Leaves *r, int att, const char *mkey, int *group, int *key, int *value)
{
	*key = *value = -1;
	if (!r->json[att] || (*group = map_group_of(r, att, mkey)) < 0)
		return false;
	foreach_att_leaf(i, r, att)
	{
		const SiftColDef *d = &r->schema->cols[i];

		if (d->parent != *group)
			continue;
		if (d->path != NULL && strstr(d->path, "#key") != NULL)
			*key = i;
		else if (*value < 0)
			*value = i;
		else
			return false;
	}
	return *key >= 0 && *value >= 0 && sift_v6_leaf_holds(TEXTOID, -1, &r->schema->cols[*key]) && sift_v6_leaf_oid(&r->schema->cols[*value]) != InvalidOid;
}

/* the map `col.* type` or `col.mkey.* type` of attribute att: the leaf of its keys and the one of its values, which are the columns of jsonb_each and jsonb_each_text */
static bool
seed_each(V6Plan *p, SiftV6XElem *e, const char *mkey, int att)
{
	int			key,
				value;

	if (!map_leaves_of(&p->leaves, att, mkey, &e->group_leaf, &key, &value))
		return false;
	e->col_leaf[0] = key;
	e->col_type[0] = TEXTOID;
	e->col_leaf[1] = value;
	e->col_type[1] = sift_v6_leaf_oid(&p->leaves.schema->cols[value]);
	e->ncols = 2;
	return true;
}

/* the lengths leaf of the array of attribute att and the leaf of its elements: the one of unnest */
static bool
seed_unnest_array(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, int att)
{
	const SiftV6Leaves *r = &p->leaves;
	const SiftV6Class cls = sift_v6_class(rte->elem);

	e->group_leaf = e->col_leaf[0] = -1;
	foreach_att_leaf(i, r, att)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			e->group_leaf = i;
	if (e->group_leaf >= 0)
		foreach_att_leaf(i, r, att)
	{
		const SiftColDef *d = &r->schema->cols[i];

		if (d->parent == e->group_leaf && d->kind == SIFT_COLK_SCALAR && d->lane == 0 && !(d->flags & SIFT_COLD_LOSSY) && (cls == SV6C_TEXT ? d->store == SIFT_COLS_STR : d->store == SIFT_COLS_INT))
			e->col_leaf[0] = i;
	}
	e->col_type[0] = rte->elem;
	e->ncols = 1;
	return e->col_leaf[0] >= 0;
}

/* the column of the index (0-based) that is the array: the column of the table, or the expression the index was made over when the format evaluates it (-1: none) */
static int
array_column(const IndexOptInfo *ix, const Expr *array, bool exprs)
{
	int			k = 0;

	for (int i = 0; i < ix->ncolumns; i++)
	{
		if (ix->indexkeys[i] != 0 ? IsA(array, Var) && ix->indexkeys[i] == ((const Var *) array)->varattno :
			exprs && !IsA(array, Var) && equal(array, list_nth(ix->indexprs, k)))
			return i;
		k += ix->indexkeys[i] == 0;
	}
	return -1;
}

/* the column of the index (0-based) that holds the array of rte, and the key of its document that is the array (NULL: the column is) */
static int
rte_column(const V6Plan *p, const SiftV6ElemRte *rte, const char **key)
{
	const bool	exprs = p->ops->jsonb != NULL;
	int			i = rte->key != NULL ? array_column(p->index, rte->base, exprs) : -1;

	*key = rte->key;
	if (i < 0)
	{
		i = array_column(p->index, rte->array, exprs);
		*key = NULL;
	}
	return i;
}

Oid
sift_v6_unnest_member_type(const V6Plan *p, const SiftV6ElemRte *rte, const char *name, bool *astext)
{
	const SiftV6Leaves *r = &p->leaves;
	const char *key;
	const int	i = p->is_table ? -1 : rte_column(p, rte, &key);
	int			group,
				leaf;

	if (i < 0 || !r->json[i + 1] || (group = sift_v6_unnest_group_of(r, i + 1, key)) < 0 || (leaf = name != NULL ? sift_v6_unnest_member_leaf(r, i + 1, group, name) : sift_v6_elems_leaf(r, i + 1, group)) < 0)
		return InvalidOid;
	*astext = (r->schema->cols[leaf].flags & SIFT_COLD_ASTEXT) != 0;
	return leaf_type(&r->schema->cols[leaf]);
}

bool
sift_v6_unnest_doc_leaf(const V6Plan *p, const SiftV6ElemRte *rte, int *leaf)
{
	const char *key = NULL;
	const int	att = rte->key != NULL ? rte_column(p, rte, &key) + 1 : 0;

	*leaf = -1;
	if (key != NULL)
		foreach_att_leaf(c, &p->leaves, att)
			if (sift_col_def_is_present(&p->leaves.schema->cols[c]))
			*leaf = c;
	return rte->key == NULL || *leaf >= 0;
}

/* the attribute of the sift_table that holds the array of rte (0: none), and the key of its document that is the array (NULL: the column is) */
static int
table_column(const V6Plan *p, const SiftV6ElemRte *rte, const char **key)
{
	const Var  *v = (const Var *) rte->base;

	*key = rte->key;
	return IsA(v, Var) && v->varattno > 0 && v->varattno <= p->sch.natts ? v->varattno : 0;
}

/* the leaves of the array of attribute att (its key `key`) that the function of rte reads */
static bool
fn_seed(V6Plan *p, const SiftV6ElemRte *rte, SiftV6XElem *e, const char *key, int att, List **names)
{
	switch (rte->fn)
	{
		case SV6E_RECORDSET:
		case SV6E_PATH:
			return seed_recordset(p, e, rte, key, att, names);
		case SV6E_UNNEST:
			return seed_unnest_array(p, e, rte, att);
		case SV6E_ELEMS_TEXT:
			return sift_v6_elems_seed(&p->leaves, e, key, att, names);
		case SV6E_MEMBERS:
			return sift_v6_members_seed(&p->leaves, e, key, att);
		default:
			return seed_each(p, e, key, att);
	}
}

bool
sift_v6_unnest_elem_seed(V6Plan *p, const SiftV6ElemRte *rte, bool left, SiftV6XElem *e, List **names)
{
	const char *key = NULL;
	const int	att = p->is_table ? table_column(p, rte, &key) : rte_column(p, rte, &key) + 1;
	const SiftV6XElem seed = {.fn = rte->fn,.attno = att,.type = rte->fn == SV6E_UNNEST ? rte->elem : exprType((const Node *) rte->array),.left = left};

	*e = seed;
	*names = NIL;
	if (att <= 0 || (p->leaves.schema != NULL && fn_seed(p, rte, e, key, att, names)))
		return att > 0;
	*e = seed;
	*names = NIL;
	return p->is_table && sift_v6_table_elems_seed(p, e, rte, names);
}

bool
sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left)
{
	SiftV6Ext  *x = sift_v6_ext_new();

	if (!sift_v6_unnest_elem_seed(p, rte, left, &x->elem, &x->elem_names))
		return false;
	x->unnest = true;
	x->elem_array = IsA(rte->array, Var) ? NIL : list_make1(copyObject(unconstify(Expr *, rte->array)));
	p->xseed = x;
	p->elem_rel = elem_rel;
	p->batch_ok = p->ops->jsonb != NULL && p->ops->jsonb->row_quals;
	return true;
}

/* the value column of the member of jsonb_each or jsonb_each_text */
static bool
is_value_var(const V6Plan *p, const Node *n, Oid type)
{
	const Var  *v = (const Var *) n;

	return IsA(n, Var) && v->varno == p->elem_rel->relid && v->varlevelsup == 0 && v->varattno == 2 && v->vartype == type;
}

/* value #>> '{}': the text of the scalar of the value */
static bool
is_value_text(const V6Plan *p, const Node *n)
{
	const OpExpr *op = (const OpExpr *) n;
	const Const *path;

	if (!IsA(n, OpExpr) || get_opcode(op->opno) != F_JSONB_EXTRACT_PATH_TEXT || list_length(op->args) != 2 || !is_value_var(p, linitial(op->args), JSONBOID) ||
		!IsA(lsecond(op->args), Const))
		return false;
	path = lsecond(op->args);
	return !path->constisnull && ARR_NDIM(DatumGetArrayTypeP(path->constvalue)) == 0;
}

/*
 * how e reads the value of the member: the cast of its text to the type of the leaf, or of jsonb_each's jsonb as it is (not before 18, where the cast of a JSON null raises and the leaf
 * reads it as NULL), or the text of jsonb_each_text itself; SV6V_NONE for anything else
 */
static SiftV6ValueRead
value_read(const V6Plan *p, const Expr *e)
{
	const SiftV6XElem *el = &p->xseed->elem;

	if (!SIFT_JSON_NULL_CAST_RAISES && el->fn == SV6E_EACH && IsA(e, FuncExpr) && sift_v6_jsonb_number_cast(((const FuncExpr *) e)->funcid) &&
		is_value_var(p, linitial(((const FuncExpr *) e)->args), JSONBOID))
		return SV6V_CAST;
	if (IsA(e, CoerceViaIO) && (el->fn == SV6E_EACH ? is_value_text(p, (const Node *) ((const CoerceViaIO *) e)->arg) :
								is_value_var(p, (const Node *) ((const CoerceViaIO *) e)->arg, TEXTOID)))
		return SV6V_TEXT;
	return el->fn == SV6E_EACH_TEXT && is_value_var(p, (const Node *) e, TEXTOID) ? SV6V_RAW : SV6V_NONE;
}

/* the read of the value is read (of the type of the expression, or SV6V_INT* of a float8 leaf), the one the statement makes: the column of the leaf */
static bool
value_slot(V6Plan *p, SiftV6ValueRead read, SiftV6Col *out)
{
	SiftV6XElem *el = &p->xseed->elem;

	if (el->vread != SV6V_NONE && el->vread != read)
		return false;
	el->vread = read;
	*out = (SiftV6Col) {.attno = el->attno,.typid = el->col_type[1],.src = SV6SRC_ELEM,.slot = 1};
	return true;
}

/*
 * whether a pair has an element of the recordset: the kind of its first member, which every element read from the leaves has (a missing member is kind 0) and the pair a LEFT JOIN keeps
 * without one has not (NULL); a column of the elements added at its first read
 */
static bool
present_column(V6Plan *p, SiftV6Col *out)
{
	SiftV6XElem *el = p->elem_stream == 0 ? &p->xseed->elem : &p->xseed->streams[p->elem_stream - 1];
	List	  **names = p->elem_stream == 0 ? &p->xseed->elem_names : (List **) &list_nth_cell(p->xseed->stream_names, p->elem_stream - 1)->ptr_value;
	int			at = 0;

	if (el->fn != SV6E_RECORDSET || el->group_leaf < 0 || el->ncols == 0)
		return false;
	while (at < el->ncols && !el->col_kind[at])
		at++;
	if (at == el->ncols)
	{
		if (el->ncols == SIFT_V6_XELEM_COLS)
			return false;
		el->col_leaf[at] = el->col_leaf[0];
		el->col_type[at] = INT8OID;
		el->col_kind[at] = true;
		el->ncols++;
		*names = lappend(*names, makeString(pstrdup(strVal(linitial(*names)))));
	}
	*out = (SiftV6Col) {.attno = el->attno,.typid = INT8OID,.src = SV6SRC_ELEM,.slot = at};
	return true;
}

/* an expression of the statement that is the value of the member, read as the leaf of the map holds it: the type the leaf has */
static bool
value_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const SiftV6ValueRead read = value_read(p, e);

	return read != SV6V_NONE && exprType((const Node *) e) == p->xseed->elem.col_type[1] && value_slot(p, read, out);
}

bool
sift_v6_unnest_value_int(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const SiftV6XElem *el = p->xseed != NULL && p->elem_rel != NULL ? &p->xseed->elem : NULL;
	const Oid	fn = IsA(e, FuncExpr) ? ((const FuncExpr *) e)->funcid : InvalidOid;
	const SiftV6ValueRead read = fn == F_INT2_JSONB ? SV6V_INT2 : fn == F_INT4_JSONB ? SV6V_INT4 : fn == F_INT8_JSONB ? SV6V_INT8 : SV6V_NONE;

	return el != NULL && el->fn == SV6E_EACH && !SIFT_JSON_NULL_CAST_RAISES && read != SV6V_NONE && el->col_type[1] == FLOAT8OID &&
		is_value_var(p, linitial(((const FuncExpr *) e)->args), JSONBOID) && value_slot(p, read, out);
}

bool
sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	const SiftV6XElem *el = p->xseed != NULL ? sift_v6_ext_stream(p->xseed, p->elem_stream) : NULL;
	SiftV6Class cls;
	int			slot;

	if (p->elem_rel == NULL || el == NULL)
		return false;
	if (IsA(e, Var) && v->varno == p->elem_rel->relid && v->varattno == SIFT_V6_ELEM_PRESENT)
		return present_column(p, out);
	if (el->fn == SV6E_MEMBERS)
		return sift_v6_members_column(p, e, out);
	if (el->fn >= SV6E_EACH && (!IsA(e, Var) || v->varattno == 2))
		return value_column(p, e, out);
	if (!IsA(e, Var) || v->varno != p->elem_rel->relid || v->varlevelsup != 0 || v->varattno < 1 || v->varattno > el->ncols || (el->fn >= SV6E_EACH && v->varattno != 1))
		return false;
	cls = sift_v6_class(v->vartype);
	if (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_BOOL && cls != SV6C_TEXT && cls != SV6C_F64)
		return false;
	slot = p->elem_map != NULL ? p->elem_map[v->varattno - 1] : v->varattno - 1;
	if (sift_v6_class(el->col_type[slot]) != cls || (cls == SV6C_I32 && el->col_type[slot] != v->vartype))
		return false;
	*out = (SiftV6Col) {.attno = el->attno,.typid = v->vartype,.src = SV6SRC_ELEM,.slot = slot};
	return true;
}

/* date_trunc('month', date column)::date: the month of the date, which PostgreSQL finds through timestamptz and the zone of the session, is a key the batches of the scan compute (v6_unnest_run.c) */
bool
sift_v6_unnest_month_key(V6Plan *p, const Expr *e, SiftV6Col *k)
{
	const FuncExpr *out = (const FuncExpr *) e;
	const FuncExpr *trunc,
			   *cast;
	const Const *unit;

	if (p->xseed == NULL || !p->xseed->unnest || !IsA(e, FuncExpr) || out->funcid != F_DATE_TIMESTAMPTZ || !IsA(linitial(out->args), FuncExpr))
		return false;
	trunc = linitial(out->args);
	if (trunc->funcid != F_DATE_TRUNC_TEXT_TIMESTAMPTZ || list_length(trunc->args) != 2 || !IsA(linitial(trunc->args), Const) || !IsA(lsecond(trunc->args), FuncExpr))
		return false;
	unit = linitial(trunc->args);
	cast = lsecond(trunc->args);
	return !unit->constisnull && pg_strcasecmp(TextDatumGetCString(unit->constvalue), "month") == 0 && cast->funcid == F_TIMESTAMPTZ_DATE &&
		sift_v6_plan_column(p, linitial(cast->args), false, k) && k->typid == DATEOID;
}

SiftV6Col
sift_v6_elem_vatt_col(int attno, Oid typid)
{
	const bool	elem = attno >= SIFT_V6_ELEM_VATT && attno < SIFT_V6_ELEM_VATT + SIFT_V6_XELEM_COLS;

	return (SiftV6Col) {.attno = attno,.typid = typid,.src = elem ? SV6SRC_ELEM : SV6SRC_ATTR,.slot = elem ? attno - SIFT_V6_ELEM_VATT : 0};
}

/* ---- the document of a row the leaves cannot hold ---- */

static Oid
jsonb_cast_fn(Oid type)
{
	return type == INT2OID ? F_INT2_JSONB : type == INT4OID ? F_INT4_JSONB : type == INT8OID ? F_INT8_JSONB : F_FLOAT8_JSONB;
}

/* the cast of jsonb_each's jsonb that a member the leaf cannot hold is read with */
static Oid
value_cast_fn(SiftV6ValueRead read, Oid type)
{
	return read == SV6V_INT2 ? F_INT2_JSONB : read == SV6V_INT4 ? F_INT4_JSONB : read == SV6V_INT8 ? F_INT8_JSONB : jsonb_cast_fn(type);
}

/* the columns of the members of jsonb_each and jsonb_each_text: the key and the value, as the function gives them */
static void
each_columns(TupleDesc desc, const SiftV6XElem *e)
{
	TupleDescInitEntry(desc, 1, "key", TEXTOID, -1, 0);
	TupleDescInitEntry(desc, 2, "value", e->fn == SV6E_EACH ? JSONBOID : TEXTOID, -1, 0);
}

SiftV6RecFn *
sift_v6_recfn_new(MemoryContext cx, const SiftV6XElem *e, const List *names)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6RecFn *f = palloc0(sizeof(SiftV6RecFn));
	int			c = 0;
	Oid			typinput;

	f->ncols = e->ncols;
	f->fn = e->fn;
	f->vread = e->vread;
	f->desc = CreateTemplateTupleDesc(f->fn == SV6E_MEMBERS ? 1 : f->ncols);
	if (e->fn >= SV6E_EACH)
		each_columns(f->desc, e);
	if (e->fn == SV6E_MEMBERS)
		TupleDescInitEntry(f->desc, 1, "value", JSONBOID, -1, 0);
	foreach_node(String, n, names)
	{
		if (f->fn == SV6E_MEMBERS)
			f->member[c] = cstring_to_text(strVal(n));
		else
		{
			TupleDescInitEntry(f->desc, c + 1, strVal(n), e->col_kind[c] ? JSONBOID : e->col_type[c], -1, 0);
			TupleDescInitEntryCollation(f->desc, c + 1, sift_v6_class(e->col_type[c]) == SV6C_TEXT ? DEFAULT_COLLATION_OID : InvalidOid);
		}
		c++;
	}
	for (c = 0; c < f->ncols; c++)
	{
		f->type[c] = e->col_type[c];
		f->present[c] = e->col_kind[c];
		if (!sift_v6_elem_late(e, c))
			continue;
		getTypeInputInfo(f->type[c], &typinput, &f->vio[c]);
		fmgr_info(typinput, &f->vin[c]);
	}
	sift_tupdesc_finalize(f->desc);
	BlessTupleDesc(f->desc);
	if (f->fn >= SV6E_EACH)
		fmgr_info(f->vread >= SV6V_CAST ? value_cast_fn(f->vread, f->type[1]) : F_JSONB_EXTRACT_PATH_TEXT, &f->vfn);
	fmgr_info(f->fn == SV6E_MEMBERS ? F_JSONB_ARRAY_ELEMENTS : sift_v6_elem_fn_oid(f->fn), &f->flinfo);
	f->econtext = CreateStandaloneExprContext();
	f->slot = MakeSingleTupleTableSlot(f->desc, &TTSOpsMinimalTuple);
	f->rsinfo = (ReturnSetInfo) {.type = T_ReturnSetInfo,.econtext = f->econtext,.expectedDesc = f->desc,.allowedModes = (int) SFRM_Materialize,.returnMode = SFRM_ValuePerCall};
	MemoryContextSwitchTo(old);
	return f;
}

static void
put_cell(MemoryContext cx, SiftVec *v, Oid type, uint32 k, Datum d, bool isnull)
{
	if (isnull)
		return;
	v->valid[k >> 6] |= UINT64CONST(1) << (k & 63);
	switch (type)
	{
		case TEXTOID:
		case VARCHAROID:
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
				const uint32 len = (uint32) VARSIZE_ANY_EXHDR(t);
				char	   *copy = MemoryContextAlloc(cx, Max(len, 1));

				memcpy(copy, VARDATA_ANY(t), len);
				((SiftStrView *) v->data)[k] = sift_vstr_make(copy, len);
				break;
			}
		case FLOAT8OID:
			((double *) v->data)[k] = DatumGetFloat8(d);
			break;
		case FLOAT4OID:
			((double *) v->data)[k] = DatumGetFloat4(d);
			break;
		case BOOLOID:
			((int64 *) v->data)[k] = DatumGetBool(d);
			break;
		case INT2OID:
			((int64 *) v->data)[k] = DatumGetInt16(d);
			break;
		case INT4OID:
			((int64 *) v->data)[k] = DatumGetInt32(d);
			break;
		default:
			((int64 *) v->data)[k] = DatumGetInt64(d);
	}
}

static void
recfn_invoke(SiftV6RecFn *f, Datum doc, MemoryContext cx)
{
	LOCAL_FCINFO(fcinfo, 1);

	f->rsinfo.setResult = NULL;
	f->econtext->ecxt_per_query_memory = cx;
	InitFunctionCallInfoData(*fcinfo, &f->flinfo, 1, InvalidOid, NULL, (Node *) &f->rsinfo);
	fcinfo->args[0].value = doc;
	fcinfo->args[0].isnull = false;
	FunctionCallInvoke(fcinfo);
}

void
sift_v6_recfn_open(SiftV6RecFn *f, Datum doc, MemoryContext cx, ErrorData **err)
{
	if (err == NULL)
	{
		recfn_invoke(f, doc, cx);
		return;
	}
	PG_TRY();
	{
		recfn_invoke(f, doc, cx);
	}
	PG_CATCH();
	{
		if (ERRCODE_TO_CATEGORY(geterrcode()) != ERRCODE_DATA_EXCEPTION)
			PG_RE_THROW();
		MemoryContextSwitchTo(cx);
		*err = CopyErrorData();
		FlushErrorState();
		f->rsinfo.setResult = NULL;
	}
	PG_END_TRY();
}

uint32
sift_v6_recfn_next(SiftV6RecFn *f, MemoryContext cx, SiftVec *ev, uint32 max)
{
	Tuplestorestate *ts = f->rsinfo.setResult;
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint32		k = 0;

	CHECK_FOR_INTERRUPTS();
	for (int c = 0; c < f->ncols; c++)
	{
		ev[c].type = sift_v6_class(f->type[c]) == SV6C_F64 ? SVEC_F64 : sift_v6_class(f->type[c]) == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
		ev[c].data = MemoryContextAllocAligned(cx, (Size) Max(max, 1) * sift_gather_width(ev[c].type) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		ev[c].valid = MemoryContextAllocZero(cx, ((max + 63) / 64 + 1) * sizeof(uint64));
		ev[c].all_valid = false;
	}
	if (f->fn >= SV6E_MEMBERS)
	{
		f->val = MemoryContextAlloc(cx, Max(max, 1) * sizeof(Datum));
		f->val_null = MemoryContextAllocZero(cx, Max(max, 1) * sizeof(bool));
		f->key = MemoryContextAlloc(cx, Max(max, 1) * sizeof(Datum));
	}
	for (; k < max && ts != NULL && tuplestore_gettupleslot(ts, true, false, f->slot); k++)
	{
		slot_getallattrs(f->slot);
		if (f->fn == SV6E_MEMBERS)
		{
			f->val[k] = datumCopy(f->slot->tts_values[0], false, -1);
			for (int c = 0; c < f->ncols; c++)
				if (f->type[c] == TEXTOID)
				{
					bool		isnull;
					const Datum d = sift_v6_member_text(f->val[k], f->member[c], &isnull);

					put_cell(cx, &ev[c], TEXTOID, k, d, isnull);
				}
			continue;
		}
		if (f->fn >= SV6E_EACH)
		{
			put_cell(cx, &ev[0], TEXTOID, k, f->slot->tts_values[0], f->slot->tts_isnull[0]);
			f->key[k] = datumCopy(f->slot->tts_values[0], false, -1);
			f->val_null[k] = f->slot->tts_isnull[1];
			f->val[k] = f->val_null[k] ? (Datum) 0 : datumCopy(f->slot->tts_values[1], false, -1);
			continue;
		}
		for (int c = 0; c < f->ncols; c++)
			put_cell(cx, &ev[c], f->type[c], k, f->present[c] ? Int64GetDatum(1) : f->slot->tts_values[c], f->present[c] ? false : f->slot->tts_isnull[c]);
	}
	for (int c = 0; c < f->ncols; c++)
		ev[c].len = (uint16) Min(k, UINT16_MAX);
	if (k < max)
	{
		if (ts != NULL)
			tuplestore_end(ts);
		f->rsinfo.setResult = NULL;
		ResetExprContext(f->econtext);
	}
	MemoryContextSwitchTo(old);
	return k;
}

uint32
sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev, ErrorData **err)
{
	sift_v6_recfn_open(f, doc, cx, err);
	return sift_v6_recfn_next(f, cx, ev, f->rsinfo.setResult == NULL ? 0 : (uint32) tuplestore_tuple_count(f->rsinfo.setResult) + 1);
}

void
sift_v6_recfn_free(SiftV6RecFn *f)
{
	FreeExprContext(f->econtext, true);
	ExecDropSingleTupleTableSlot(f->slot);
}

/* the value of member k of jsonb_each as the statement reads it: the cast of the jsonb, or the input function of the text of the scalar; the errors are those of the statement */
static Datum
member_value(SiftV6RecFn *f, uint32 k, bool *isnull)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		d = f->val[k];

	*isnull = f->val_null[k];
	if (*isnull || f->vread == SV6V_RAW)
		return d;
	if (f->vread >= SV6V_CAST)
	{
		InitFunctionCallInfoData(*fcinfo, &f->vfn, 1, InvalidOid, NULL, NULL);
		fcinfo->args[0] = (NullableDatum) {d, false};
		d = FunctionCallInvoke(fcinfo);
		*isnull = fcinfo->isnull;
		return f->vread == SV6V_INT2 ? Float8GetDatum(DatumGetInt16(d)) : f->vread == SV6V_INT4 ? Float8GetDatum(DatumGetInt32(d)) :
			f->vread == SV6V_INT8 ? Float8GetDatum((double) DatumGetInt64(d)) : d;
	}
	if (f->fn == SV6E_EACH)
	{
		InitFunctionCallInfoData(*fcinfo, &f->vfn, 2, InvalidOid, NULL, NULL);
		fcinfo->args[0] = (NullableDatum) {d, false};
		fcinfo->args[1] = (NullableDatum) {PointerGetDatum(construct_empty_array(TEXTOID)), false};
		d = FunctionCallInvoke(fcinfo);
		if ((*isnull = fcinfo->isnull))
			return (Datum) 0;
	}
	return InputFunctionCall(&f->vin[1], TextDatumGetCString(d), f->vio[1], -1);
}

/* the late columns of element k of SV6E_MEMBERS: the cast of the text of its member, NULL for an element that has none */
static void
member_casts(SiftV6RecFn *f, uint32 k, MemoryContext cx, SiftVec *ev)
{
	for (int c = 0; c < f->ncols; c++)
		if (f->type[c] != TEXTOID)
		{
			bool		isnull;
			const Datum t = sift_v6_member_text(f->val[k], f->member[c], &isnull);

			put_cell(cx, &ev[c], f->type[c], k, isnull ? (Datum) 0 : InputFunctionCall(&f->vin[c], TextDatumGetCString(t), f->vio[c], -1), isnull);
		}
}

void
sift_v6_recfn_values(SiftV6RecFn *f, const uint64 *keep, MemoryContext cx, SiftVec *ev, uint32 n, ErrorData **late)
{
	if (f->fn < SV6E_MEMBERS || (f->fn >= SV6E_EACH && f->vread == SV6V_NONE))
		return;
	for (uint32 k = 0; k < n; k++)
		if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
		{
			PG_TRY();
			{
				bool		isnull;

				if (f->fn == SV6E_MEMBERS)
					member_casts(f, k, cx, ev);
				else
				{
					const Datum d = member_value(f, k, &isnull);

					put_cell(cx, &ev[1], f->type[1], k, d, isnull);
				}
			}
			PG_CATCH();
			{
				if (ERRCODE_TO_CATEGORY(geterrcode()) != ERRCODE_DATA_EXCEPTION)
					PG_RE_THROW();
				MemoryContextSwitchTo(cx);
				late[k] = CopyErrorData();
				FlushErrorState();
			}
			PG_END_TRY();
		}
}
