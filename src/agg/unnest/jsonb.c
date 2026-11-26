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

/* doc -> 'key' of a constant key: the document and the key */
static const char *
field_key(const Expr *e, const Expr **doc)
{
	const OpExpr *op = (const OpExpr *) e;
	const Const *k;

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
sift_v6_unnest_group_of(const SiftColRel *r, int att, const char *key)
{
	const size_t len = key != NULL ? strlen(key) : 0;
	int			group = -1;

	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];
		const char *dot = d->kind == SIFT_COLK_GROUP && d->path != NULL ? strchr(d->path, '.') : NULL;

		if (d->kind == SIFT_COLK_GROUP && (key == NULL ? dot == NULL : dot != NULL && strncmp(dot + 1, key, len) == 0 && strcmp(dot + 1 + len, "[]") == 0))
			group = i;
	}
	return group;
}

int
sift_v6_unnest_member_leaf(const SiftColRel *r, int att, int group, const char *name)
{
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
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
	const SiftColRel *r = p->colrel;
	int			c = 0;

	if (!r->json[att] || (e->group_leaf = sift_v6_unnest_group_of(r, att, key)) < 0)
		return false;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
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

/* the type of the values a leaf holds, among those an element column has */
static Oid
leaf_oid(const SiftColDef *d)
{
	static const Oid held[] = {TEXTOID, INT2OID, INT4OID, INT8OID, FLOAT8OID};

	for (int t = 0; t < lengthof(held); t++)
		if (sift_v6_leaf_holds(held[t], -1, d))
			return held[t];
	return InvalidOid;
}

/* the group of the map of attribute att: `col.* type` of the document itself (key NULL), or `col.key.* type` of its key `key` (-1: the attribute has none) */
static int
map_group_of(const SiftColRel *r, int att, const char *key)
{
	const size_t len = key != NULL ? strlen(key) : 0;

	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
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
map_leaves_of(const SiftColRel *r, int att, const char *mkey, int *group, int *key, int *value)
{
	*key = *value = -1;
	if (!r->json[att] || (*group = map_group_of(r, att, mkey)) < 0)
		return false;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
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
	return *key >= 0 && *value >= 0 && sift_v6_leaf_holds(TEXTOID, -1, &r->schema->cols[*key]) && leaf_oid(&r->schema->cols[*value]) != InvalidOid;
}

bool
sift_v6_map_leaves(const SiftColRel *r, int att, int *group, int *key, int *value)
{
	return map_leaves_of(r, att, NULL, group, key, value);
}

/* the map `col.* type` or `col.mkey.* type` of attribute att: the leaf of its keys and the one of its values, which are the columns of jsonb_each and jsonb_each_text */
static bool
seed_each(V6Plan *p, SiftV6XElem *e, const char *mkey, int att)
{
	int			key,
				value;

	if (!map_leaves_of(p->colrel, att, mkey, &e->group_leaf, &key, &value))
		return false;
	e->col_leaf[0] = key;
	e->col_type[0] = TEXTOID;
	e->col_leaf[1] = value;
	e->col_type[1] = leaf_oid(&p->colrel->schema->cols[value]);
	e->ncols = 2;
	return true;
}

/* the lengths leaf of the array of attribute att and the leaf of its elements: the one of unnest */
static bool
seed_unnest_array(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, int att)
{
	const SiftColRel *r = p->colrel;
	const SiftV6Class cls = sift_v6_class(rte->elem);

	e->group_leaf = e->col_leaf[0] = -1;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			e->group_leaf = i;
	for (int i = r->first[att]; e->group_leaf >= 0 && i < r->first[att] + r->nleaves[att]; i++)
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
sift_v6_unnest_member_type(const V6Plan *p, const SiftV6ElemRte *rte, const char *name)
{
	const SiftColRel *r = p->colrel;
	const char *key;
	const int	i = rte_column(p, rte, &key);
	int			group,
				leaf;

	if (i < 0 || !r->json[i + 1] || (group = sift_v6_unnest_group_of(r, i + 1, key)) < 0 || (leaf = sift_v6_unnest_member_leaf(r, i + 1, group, name)) < 0)
		return InvalidOid;
	return leaf_type(&r->schema->cols[leaf]);
}

/* A sift_table keeps the array of a row whole in its cell, and the scan takes it apart from the heap row: the elements of unnest(array) need no leaf */
static bool
seed_table_array(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte)
{
	if (rte->fn != SV6E_UNNEST || !IsA(rte->array, Var) || ((const Var *) rte->array)->varattno <= 0 || ((const Var *) rte->array)->varattno > p->sch.natts)
		return false;
	e->attno = ((const Var *) rte->array)->varattno;
	e->group_leaf = e->col_leaf[0] = -1;
	e->col_type[0] = rte->elem;
	e->ncols = 1;
	return true;
}

bool
sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left)
{
	SiftV6Ext  *x = sift_v6_ext_new();
	List	   *names = NIL;
	const char *key;
	int			i;

	if (p->is_table)
	{
		x->unnest = true;
		x->elem = (SiftV6XElem) {.fn = rte->fn,.type = rte->elem,.left = left};
		if (!seed_table_array(p, &x->elem, rte))
			return false;
		p->xseed = x;
		p->elem_rel = elem_rel;
		return true;
	}
	i = rte_column(p, rte, &key);
	if (i < 0)
		return false;
	x->unnest = true;
	x->elem = (SiftV6XElem) {.fn = rte->fn,.attno = i + 1,.type = rte->fn == SV6E_UNNEST ? rte->elem : exprType((const Node *) rte->array),.left = left};
	if (!((rte->fn == SV6E_RECORDSET || rte->fn == SV6E_PATH) ? seed_recordset(p, &x->elem, rte, key, i + 1, &names) :
		  rte->fn == SV6E_UNNEST ? seed_unnest_array(p, &x->elem, rte, i + 1) :
		  rte->fn == SV6E_ELEMS_TEXT ? sift_v6_elems_seed(p->colrel, &x->elem, key, i + 1, &names) :
		  rte->fn == SV6E_MEMBERS ? sift_v6_members_seed(p->colrel, &x->elem, key, i + 1) : seed_each(p, &x->elem, key, i + 1)))
		return false;
	x->elem_names = names;
	x->elem_array = IsA(rte->array, Var) ? NIL : list_make1(copyObject(unconstify(Expr *, rte->array)));
	p->xseed = x;
	p->elem_rel = elem_rel;
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

static bool
is_jsonb_number_cast(Oid funcid)
{
	return funcid == F_INT2_JSONB || funcid == F_INT4_JSONB || funcid == F_INT8_JSONB || funcid == F_FLOAT8_JSONB;
}

/* how e reads the value of the member: the cast of its text to the type of the leaf, or of jsonb_each's jsonb as it is; SV6V_NONE for anything else */
static SiftV6ValueRead
value_read(const V6Plan *p, const Expr *e)
{
	const SiftV6XElem *el = &p->xseed->elem;

	if (el->fn == SV6E_EACH && IsA(e, FuncExpr) && is_jsonb_number_cast(((const FuncExpr *) e)->funcid) &&
		is_value_var(p, linitial(((const FuncExpr *) e)->args), JSONBOID))
		return SV6V_CAST;
	if (IsA(e, CoerceViaIO) && (el->fn == SV6E_EACH ? is_value_text(p, (const Node *) ((const CoerceViaIO *) e)->arg) :
								is_value_var(p, (const Node *) ((const CoerceViaIO *) e)->arg, TEXTOID)))
		return SV6V_TEXT;
	return SV6V_NONE;
}

/* an expression of the statement that is the value of the member, read as the leaf of the map holds it: the type the leaf has */
static bool
value_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	SiftV6XElem *el = &p->xseed->elem;
	const SiftV6ValueRead read = value_read(p, e);

	if (read == SV6V_NONE || exprType((const Node *) e) != el->col_type[1] || (el->vread != SV6V_NONE && el->vread != read))
		return false;
	el->vread = read;
	*out = (SiftV6Col) {.attno = el->attno,.typid = el->col_type[1],.src = SV6SRC_ELEM,.slot = 1};
	return true;
}

/* the column of the element that column v of the relation of the lateral function or of the sub-select of an EXISTS is */
static int
elem_slot(const V6Plan *p, const Var *v)
{
	return p->elem_map != NULL ? p->elem_map[v->varattno - 1] : v->varattno - 1;
}

bool
sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	const SiftV6Ext *x = p->xseed;
	SiftV6Class cls;
	int			slot;

	if (x != NULL && x->elem.fn == SV6E_KEYS)
		return sift_v6_mapkey_column(p, e, out);
	if (p->elem_rel == NULL || x == NULL)
		return false;
	if (x->elem.fn == SV6E_MEMBERS)
		return sift_v6_members_column(p, e, out);
	if (x->elem.fn >= SV6E_EACH && !IsA(e, Var))
		return value_column(p, e, out);
	if (!IsA(e, Var) || v->varno != p->elem_rel->relid || v->varlevelsup != 0 || v->varattno < 1 || v->varattno > x->elem.ncols ||
		(x->elem.fn >= SV6E_EACH && v->varattno != 1))
		return false;
	cls = sift_v6_class(v->vartype);
	if (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_TEXT && v->vartype != FLOAT8OID)
		return false;
	slot = elem_slot(p, v);
	if (sift_v6_class(x->elem.col_type[slot]) != cls || (cls == SV6C_I32 && x->elem.col_type[slot] != v->vartype))
		return false;
	*out = (SiftV6Col) {.attno = x->elem.attno,.typid = v->vartype,.src = SV6SRC_ELEM,.slot = slot};
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

/* ---- the quals of the element ---- */

/* the column of the element that n is, as the virtual attribute of its slot; the value of a member is tested by the clauses of the pairs (a FILTER) only: that of a row read again from the heap is made after the quals of the element */
static Expr *
elem_var(const V6Plan *p, const Node *n, bool pairs)
{
	SiftV6Col	col;

	if (n == NULL || !sift_v6_unnest_column((V6Plan *) p, (const Expr *) n, &col) || col.src != SV6SRC_ELEM || (!pairs && sift_v6_elem_late(&p->xseed->elem, col.slot)))
		return NULL;
	return (Expr *) makeVar(INDEX_VAR, SIFT_V6_ELEM_VATT + col.slot, exprType(n), exprTypmod(n), exprCollation(n), 0);
}

SiftV6Col
sift_v6_elem_vatt_col(int attno, Oid typid)
{
	const bool	elem = attno >= SIFT_V6_ELEM_VATT && attno < SIFT_V6_ELEM_VATT + SIFT_V6_XELEM_COLS;

	return (SiftV6Col) {.attno = attno,.typid = typid,.src = elem ? SV6SRC_ELEM : SV6SRC_ATTR,.slot = elem ? attno - SIFT_V6_ELEM_VATT : 0};
}

static bool
text_compares_bytes(const Node *n, Oid collation)
{
	return sift_v6_class(exprType(n)) != SV6C_TEXT || sift_text_bytes_equal(collation);
}

/* a comparison of a column of the element with a constant, an IN list of constants, or a null test, over the virtual attribute of the column; or an AND, OR or NOT of such tests */
static Expr *
elem_qual(const V6Plan *p, Node *clause, bool pairs)
{
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *q = copyObject((BoolExpr *) clause);
		List	   *args = NIL;

		foreach_ptr(Node, a, q->args)
		{
			Expr	   *t = elem_qual(p, a, pairs);

			if (t == NULL)
				return NULL;
			args = lappend(args, t);
		}
		q->args = args;
		return (Expr *) q;
	}
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *q = copyObject((OpExpr *) clause);
		Node	   *l = linitial(q->args),
				   *r = lsecond(q->args);
		Expr	   *var;

		if (IsA(l, Const) && !IsA(r, Const))
		{
			q->opno = get_commutator(q->opno);
			if (!OidIsValid(q->opno))
				return NULL;
			q->opfuncid = get_opcode(q->opno);
			l = lsecond(q->args);
			r = linitial(q->args);
		}
		if ((var = elem_var(p, l, pairs)) == NULL || !IsA(r, Const) || !text_compares_bytes(l, q->inputcollid))
			return NULL;
		q->args = list_make2(var, r);
		return (Expr *) q;
	}
	if (IsA(clause, ScalarArrayOpExpr) && list_length(((ScalarArrayOpExpr *) clause)->args) == 2)
	{
		ScalarArrayOpExpr *q = copyObject((ScalarArrayOpExpr *) clause);
		Expr	   *var = elem_var(p, linitial(q->args), pairs);

		if (var == NULL || !IsA(lsecond(q->args), Const) || !text_compares_bytes(linitial(q->args), q->inputcollid))
			return NULL;
		linitial(q->args) = var;
		return (Expr *) q;
	}
	if (IsA(clause, NullTest) && !((NullTest *) clause)->argisrow)
	{
		NullTest   *q = copyObject((NullTest *) clause);
		Expr	   *var = elem_var(p, (Node *) q->arg, pairs);

		if (var == NULL)
			return NULL;
		q->arg = var;
		return (Expr *) q;
	}
	return NULL;
}

Expr *
sift_v6_unnest_elem_qual(const V6Plan *p, Node *clause)
{
	return elem_qual(p, clause, true);
}

bool
sift_v6_unnest_elem_quals(V6Plan *p)
{
	SiftClause *clause = palloc(sizeof(SiftClause));
	int			col[SIFT_V6_MAX_FCOLS],
				nvec;
	List	   *quals = NIL;

	if (p->elem_rel == NULL)
		return true;
	foreach_node(RestrictInfo, ri, p->elem_rel->baserestrictinfo)
	{
		Expr	   *q = ri->pseudoconstant ? NULL : elem_qual(p, (Node *) ri->clause, false);

		if (q == NULL)
			return false;
		quals = lappend(quals, q);
	}
	if (quals != NIL && !sift_v6_unnest_elem_clause(CurrentMemoryContext, quals, clause, col, &nvec))
		return false;
	p->xseed->elem_where = quals;
	return true;
}

bool
sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec)
{
	SiftV6Spec	tmp = {0};

	if (!sift_v6_clause_make_or(cx, quals, &tmp, out))
		return false;
	for (int i = 0; i < tmp.nfcols; i++)
		col[i] = tmp.fcol[i].attno - SIFT_V6_ELEM_VATT;
	*nvec = tmp.nfcols;
	return true;
}

/* ---- the document of a row the leaves cannot hold ---- */

struct SiftV6RecFn
{
	FmgrInfo	flinfo;
	ReturnSetInfo rsinfo;
	ExprContext *econtext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	int			ncols;
	Oid			type[SIFT_V6_XELEM_COLS];
	SiftV6ElemFn fn;
	SiftV6ValueRead vread;		/* jsonb_each, jsonb_each_text: the members come with the value as the function gives it, which values() reads as the statement does */
	FmgrInfo	vfn;			/* the cast of jsonb_each's jsonb to the type of the value, or its #>> '{}' */
	FmgrInfo	vin[SIFT_V6_XELEM_COLS];	/* the input function of the type of a column computed late: the value of jsonb_each, the cast of a member of SV6E_MEMBERS */
	Oid			vio[SIFT_V6_XELEM_COLS];
	text	   *member[SIFT_V6_XELEM_COLS];	/* SV6E_MEMBERS: the key each column reads of an element */
	Datum	   *val;			/* the value of jsonb_each, the element of SV6E_MEMBERS */
	bool	   *val_null;
};

static Oid
jsonb_cast_fn(Oid type)
{
	return type == INT2OID ? F_INT2_JSONB : type == INT4OID ? F_INT4_JSONB : type == INT8OID ? F_INT8_JSONB : F_FLOAT8_JSONB;
}

/* the columns of the members of jsonb_each and jsonb_each_text: the key and the value, as the function gives them */
static void
each_columns(TupleDesc desc, const SiftV6Ext *x)
{
	TupleDescInitEntry(desc, 1, "key", TEXTOID, -1, 0);
	TupleDescInitEntry(desc, 2, "value", x->elem.fn == SV6E_EACH ? JSONBOID : TEXTOID, -1, 0);
}

SiftV6RecFn *
sift_v6_recfn_new(MemoryContext cx, const SiftV6Ext *x)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6RecFn *f = palloc0(sizeof(SiftV6RecFn));
	int			c = 0;
	Oid			typinput;

	f->ncols = x->elem.ncols;
	f->fn = x->elem.fn;
	f->vread = x->elem.vread;
	f->desc = CreateTemplateTupleDesc(f->fn == SV6E_MEMBERS ? 1 : f->ncols);
	if (x->elem.fn >= SV6E_EACH)
		each_columns(f->desc, x);
	if (x->elem.fn == SV6E_MEMBERS)
		TupleDescInitEntry(f->desc, 1, "value", JSONBOID, -1, 0);
	foreach_node(String, n, x->elem_names)
	{
		if (f->fn == SV6E_MEMBERS)
			f->member[c] = cstring_to_text(strVal(n));
		else
		{
			TupleDescInitEntry(f->desc, c + 1, strVal(n), x->elem.col_type[c], -1, 0);
			TupleDescInitEntryCollation(f->desc, c + 1, sift_v6_class(x->elem.col_type[c]) == SV6C_TEXT ? DEFAULT_COLLATION_OID : InvalidOid);
		}
		c++;
	}
	for (c = 0; c < f->ncols; c++)
	{
		f->type[c] = x->elem.col_type[c];
		if (!sift_v6_elem_late(&x->elem, c))
			continue;
		getTypeInputInfo(f->type[c], &typinput, &f->vio[c]);
		fmgr_info(typinput, &f->vin[c]);
	}
	sift_tupdesc_finalize(f->desc);
	BlessTupleDesc(f->desc);
	if (f->fn >= SV6E_EACH)
		fmgr_info(f->vread == SV6V_CAST ? jsonb_cast_fn(f->type[1]) : F_JSONB_EXTRACT_PATH_TEXT, &f->vfn);
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

uint32
sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev)
{
	LOCAL_FCINFO(fcinfo, 1);
	Tuplestorestate *ts;
	uint32		n;

	f->rsinfo.setResult = NULL;
	f->econtext->ecxt_per_query_memory = cx;
	InitFunctionCallInfoData(*fcinfo, &f->flinfo, 1, InvalidOid, NULL, (Node *) &f->rsinfo);
	fcinfo->args[0].value = doc;
	fcinfo->args[0].isnull = false;
	FunctionCallInvoke(fcinfo);
	ts = f->rsinfo.setResult;
	n = ts == NULL ? 0 : (uint32) tuplestore_tuple_count(ts);
	for (int c = 0; c < f->ncols; c++)
	{
		ev[c].type = sift_v6_class(f->type[c]) == SV6C_F64 ? SVEC_F64 : sift_v6_class(f->type[c]) == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
		ev[c].data = MemoryContextAllocAligned(cx, (Size) Max(n, 1) * sift_gather_width(ev[c].type) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		ev[c].valid = MemoryContextAllocZero(cx, ((n + 63) / 64 + 1) * sizeof(uint64));
		ev[c].all_valid = false;
		ev[c].len = (uint16) Min(n, UINT16_MAX);
	}
	if (f->fn >= SV6E_MEMBERS)
	{
		f->val = MemoryContextAlloc(cx, Max(n, 1) * sizeof(Datum));
		f->val_null = MemoryContextAlloc(cx, Max(n, 1) * sizeof(bool));
	}
	for (uint32 k = 0; ts != NULL && tuplestore_gettupleslot(ts, true, false, f->slot); k++)
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
			f->val_null[k] = f->slot->tts_isnull[1];
			f->val[k] = f->val_null[k] ? (Datum) 0 : datumCopy(f->slot->tts_values[1], false, -1);
			continue;
		}
		for (int c = 0; c < f->ncols; c++)
			put_cell(cx, &ev[c], f->type[c], k, f->slot->tts_values[c], f->slot->tts_isnull[c]);
	}
	if (ts != NULL)
		tuplestore_end(ts);
	ResetExprContext(f->econtext);
	return n;
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
	if (*isnull)
		return (Datum) 0;
	if (f->vread == SV6V_CAST)
	{
		InitFunctionCallInfoData(*fcinfo, &f->vfn, 1, InvalidOid, NULL, NULL);
		fcinfo->args[0] = (NullableDatum) {d, false};
		d = FunctionCallInvoke(fcinfo);
		*isnull = fcinfo->isnull;
		return d;
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
				MemoryContextSwitchTo(cx);
				late[k] = CopyErrorData();
				FlushErrorState();
			}
			PG_END_TRY();
		}
}
