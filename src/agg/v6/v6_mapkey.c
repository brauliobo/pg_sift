/*
 * agg/v6/v6_mapkey.c - the members of a map `col.* float8` of a sift_col index of format major 2 that a statement reads by a constant key (`col -> 'k'`, `col['k']`, their casts to float8 and to an integer,
 * and `(col ->> 'k')::float8`) as the columns of an aggregate scan of SV6E_KEYS: the scan gives one row for each row of the table, with the member the key names (NULL when the row has none), where the
 * unnest of the same map gives one row for each member (v6_unnest_jsonb.h).
 */
/* check-memory: allow-allocs every list here follows the expressions of one statement */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_unnest_jsonb.h"
#include "col/am.h"

/* e reads the member `doc -> 'k'` as the type of a column of the scan: the cast of the jsonb to float8 or an integer (the cast of a JSON null raises before PostgreSQL 18, which the leaf does not tell), or the float8 of its text */
static bool
member_read(const Node *e, const Var **doc, const char **key, Oid *type)
{
	if (e != NULL && IsA(e, FuncExpr) && list_length(((const FuncExpr *) e)->args) == 1)
	{
		const FuncExpr *f = (const FuncExpr *) e;

		*type = f->funcresulttype;
#if PG_VERSION_NUM < 180000
		return false;
#else
		return (f->funcid == F_FLOAT8_JSONB || f->funcid == F_INT2_JSONB || f->funcid == F_INT4_JSONB || f->funcid == F_INT8_JSONB) &&
			sift_v6_json_member(linitial(f->args), false, doc, key);
#endif
	}
	if (e != NULL && IsA(e, CoerceViaIO) && ((const CoerceViaIO *) e)->resulttype == FLOAT8OID)
	{
		*type = FLOAT8OID;
		return sift_v6_json_member(((const CoerceViaIO *) e)->arg, true, doc, key);
	}
	return false;
}

/* the attribute of the index (1-based) the column doc is; 0: the index does not hold it */
static int
index_att(const V6Plan *p, const Var *doc)
{
	if (doc->varno != p->rel->relid || doc->varlevelsup != 0)
		return 0;
	for (int i = 0; i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == doc->varattno)
			return i + 1;
	return 0;
}

/* the scalar leaf the index keeps of the key of attribute att (the member of the document is then a column of the index, not of a map); -1: none */
static int
key_scalar_leaf(const V6Plan *p, int att, const char *key)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	const int	leaf = rel.ops != NULL && rel.ops->jsonb != NULL ? rel.ops->jsonb->key_leaf(p->index->indexoid, att, key, strlen(key)) : -1;

	return leaf >= 0 && p->colrel->schema->cols[leaf].kind == SIFT_COLK_SCALAR && p->colrel->schema->cols[leaf].lane == 0 ? leaf : -1;
}

/* the leaf of the key that holds its numbers as float8; -1: none */
static int
key_float8_leaf(const V6Plan *p, int att, const char *key)
{
	const int	leaf = key_scalar_leaf(p, att, key);

	return leaf >= 0 && p->colrel->schema->cols[leaf].store == SIFT_COLS_F8 && p->colrel->schema->cols[leaf].type == SIFT_COLT_FLOAT8 ? leaf : -1;
}

bool
sift_v6_mapkey_on(const V6Plan *p)
{
	return p->xseed != NULL && p->xseed->elem.fn == SV6E_KEYS;
}

bool
sift_v6_mapkey_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	SiftV6Ext  *x = p->xseed;
	SiftV6XElem *el = &x->elem;
	const Var  *doc;
	const char *key;
	Oid			type;
	int			at = 0;
	int			att;
	int			leaf;

	if (!member_read((const Node *) e, &doc, &key, &type) || (att = index_att(p, doc)) == 0)
		return false;
	leaf = key_float8_leaf(p, att, key);
	if (leaf < 0 && att != el->attno)
		return false;
	while (at < el->ncols && !equal(e, list_nth(x->col_expr, at)))
		at++;
	if (at == el->ncols)
	{
		if (el->ncols == SIFT_V6_XELEM_COLS)
			return false;
		el->col_leaf[el->ncols] = leaf;
		el->col_att[el->ncols] = att;
		el->col_type[el->ncols++] = type;
		x->elem_names = lappend(x->elem_names, makeString(pstrdup(key)));
		x->col_expr = lappend(x->col_expr, copyObject((Node *) e));
	}
	*out = (SiftV6Col) {.attno = att,.typid = type,.src = SV6SRC_ELEM,.slot = at};
	return true;
}

bool
sift_v6_mapkey_pair_qual(V6Plan *p, RestrictInfo *ri)
{
	Expr	   *q = sift_v6_mapkey_on(p) && !ri->pseudoconstant ? sift_v6_unnest_elem_qual(p, (Node *) ri->clause) : NULL;
	List	   *quals;
	SiftClause	clause;
	int			col[SIFT_V6_MAX_FCOLS],
				nvec;

	if (q == NULL)
		return false;
	quals = lappend(list_copy(p->xseed->pair_where), q);
	if (!sift_v6_unnest_elem_clause(CurrentMemoryContext, quals, &clause, col, &nvec))
		return false;
	p->xseed->pair_where = quals;
	return true;
}

char *
sift_v6_mapkey_text(const SiftV6Ext *x, TupleDesc index_desc)
{
	StringInfoData b;
	int			c = 0;

	initStringInfo(&b);
	foreach_node(String, k, x->elem_names)
	{
		appendStringInfo(&b, "%s%s['%s'] %s", c > 0 ? ", " : "", NameStr(TupleDescAttr(index_desc, x->elem.col_att[c] - 1)->attname), strVal(k), format_type_be(x->elem.col_type[c]));
		c++;
	}
	return b.data;
}

typedef struct Probe
{
	const V6Plan *p;
	int			att;
} Probe;

/* the first read of a member of a document of the index that the statement has; the float8 of the text of a key that is a leaf is a column of the scan of the columns of the index, as it has been */
static bool
probe(Node *n, Probe *pr)
{
	const Var  *doc;
	const char *key;
	Oid			type;

	if (n == NULL)
		return false;
	if (member_read(n, &doc, &key, &type) && (pr->att = index_att(pr->p, doc)) > 0 && (IsA(n, FuncExpr) || key_scalar_leaf(pr->p, pr->att, key) < 0))
		return true;
	return expression_tree_walker(n, probe, pr);
}

static bool
reads_member(V6Plan *p, GroupPathExtraData *extra, List *group_exprs, int *att)
{
	Probe		pr = {p, 0};
	bool		found = probe((Node *) p->out->reltarget->exprs, &pr) || probe((Node *) group_exprs, &pr) || probe(extra != NULL ? extra->havingQual : NULL, &pr);

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		found = found || probe((Node *) ri->clause, &pr);
	*att = pr.att;
	return found;
}

/* the Ext of the scan that reads the members of the map of attribute att by key (none: the reads are of keys that are leaves) */
static void
seed(V6Plan *p, int att)
{
	int			group = -1,
				key = -1,
				value = -1;
	SiftV6Ext  *x = sift_v6_ext_new();

	if (!sift_v6_map_leaves(p->colrel, att, &group, &key, &value) || p->colrel->schema->cols[value].store != SIFT_COLS_F8 || p->colrel->schema->cols[value].type != SIFT_COLT_FLOAT8)
		group = key = value = att = -1;
	x->unnest = true;
	x->elem = (SiftV6XElem) {.fn = SV6E_KEYS,.attno = att,.type = JSONBOID,.group_leaf = group,.key_leaf = key,.val_leaf = value};
	p->xseed = x;
}

bool
sift_v6_mapkey_paths(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	int			att;

	if (!reads_member(p, extra, group_exprs, &att))
		return false;
	seed(p, att);
	p->batch_ok = false;
	if (sift_v6_compile_on(p, extra, group_exprs) && p->xseed->elem.ncols > 0 && sift_v6_unnest_ready(p))
		sift_v6_add_join_path(p, group_exprs, (List *) extra->havingQual, NIL, 0, p->rel->rows * cpu_operator_cost * (1 + p->xseed->elem.ncols), p->rel->rows);
	return true;
}
