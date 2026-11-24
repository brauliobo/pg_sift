/*
 * col/f2/f2_v6_proj_sort_plan.c - the path and the node of SiftProjSortScan (col/f2/f2_v6_proj_sort_plan.h).
 */
/* check-memory: allow-allocs every array here follows the columns and quals of one statement, not the rows of the table */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/uuid.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

#include "agg/v6/v6_safety.h"
#include "agg/plan/format_ops.h"
#include "col/f2/f2_v6_proj_sort_ios.h"
#include "col/f2/f2_v6_proj_ctx.h"
#include "col/f2/f2_v6_proj_key.h"
#include "col/f2/f2_v6_proj_sort_exec.h"
#include "col/f2/f2_v6_proj_sort_plan.h"
#include "col/am.h"
#include "col/am/format_ops.h"
#include "col/col.h"
#include "col/f2/f2.h"
#include "col/f2/col_meta.h"
#include "core/compat.h"
#include "core/memtrack.h"
#include "table/dispatch.h"
#include "table/f2/f2_slot.h"

#define SORT_LIMIT_GUESS		100		/* the rows a LIMIT whose value the plan does not know is costed for */
#define SORT_RESIDUAL_SEL_MIN	1e-6
#define SORT_LOOKUP_MAX_ROWS	10000	/* the most rows a statement with no ORDER BY and no LIMIT may expect for a projection to seek them: it hands out all of them */
#define SORT_RESIDUAL_MAX_ROWS	100000	/* the most rows a statement with quals no cell can judge may have vetted against the relation to fill its limit */

/* What the plan carries, in custom_private (sort_pack / sort_unpack) */
typedef struct SortSpec
{
	bool		table;			/* a sift_table; else a sift_col index */
	Oid			indexoid;
	bool		residual;		/* the statement has quals no cell can judge: every row is vetted against the relation */
	bool		ios;			/* every attribute the statement reads is in the cells of the projection: a row of a page the relation's visibility map says is all visible needs no fetch */
	bool		null_arg;		/* a parameter of a qual is NULL: no row satisfies the statement, which the relation answers */
	int			limit;			/* the rows LIMIT and OFFSET ask for together: at execution -1 when there is no bound; in the plan the rows it is costed for */
	Node	   *limit_expr;
	Node	   *offset_expr;
	int			norder;
	int			order_att[SV6P_MAX];	/* of the order columns: the attribute of the relation the store reads, and of the table the sort reads */
	AttrNumber	heap_att[SV6P_MAX];
	Oid			sortop[SV6P_MAX];
	Oid			collid[SV6P_MAX];
	bool		desc[SV6P_MAX];
	bool		nulls_first[SV6P_MAX];
	int			nkeys;
	ScanKeyData *key;
	SortIosCols iosc;
	char		proj[SIFT_PROJ_NAME_LEN];
} SortSpec;

static const CustomPathMethods sort_path_methods;
static const CustomScanMethods sort_plan_methods;
static set_rel_pathlist_hook_type prev_pathlist_hook;

/* ---- the plan's private list ---- */

static List *
sort_pack(const SortSpec *s, List *keys)
{
	List	   *ints = list_make5_int(s->table, (int) s->indexoid, s->residual, s->norder, s->ios);

	for (int i = 0; i < s->norder; i++)
	{
		ints = list_concat(ints, list_make4_int(s->order_att[i], s->heap_att[i], (int) s->sortop[i], (int) s->collid[i]));
		ints = list_concat(ints, list_make2_int(s->desc[i], s->nulls_first[i]));
	}
	ints = lappend_int(ints, s->iosc.n);
	for (int i = 0; i < s->iosc.n; i++)
		ints = list_concat(ints, list_make2_int(s->iosc.att[i], s->iosc.leaf[i]));
	return list_make5(ints, keys, makeString(pstrdup(s->proj)), s->limit_expr, s->offset_expr);
}

/* A value the plan can stand in for a parameter that has none yet, of the type the key needs: what the choice of a projection reads is the operator and the column, not the value */
static Datum
placeholder_value(Oid type, bool *ok)
{
	int16		len;
	bool		byval;
	char		align;
	Oid			elem = get_element_type(type);

	*ok = true;
	if (elem != InvalidOid)
	{
		bool		eok;
		Datum		d = placeholder_value(elem, &eok);

		get_typlenbyvalalign(elem, &len, &byval, &align);
		*ok = eok;
		return eok ? PointerGetDatum(construct_array(&d, 1, elem, len, byval, align)) : (Datum) 0;
	}
	switch (type)
	{
		case TEXTOID:
		case VARCHAROID:
			return PointerGetDatum(cstring_to_text(""));
		case UUIDOID:
			return PointerGetDatum(palloc0(UUID_LEN));
		default:
			get_typlenbyvalalign(type, &len, &byval, &align);
			*ok = byval;
			return (Datum) 0;
	}
}

static bool
placeholder_known(Oid type)
{
	bool		ok;

	(void) placeholder_value(type, &ok);
	return ok;
}

/* The value of the argument of a key: the constant, the parameter's value when the statement runs (econtext), or a placeholder while it is planned (NULL econtext) */
static Datum
key_value(Node *arg, ExprContext *econtext, bool *null)
{
	bool		ok;

	*null = false;
	if (IsA(arg, Const))
		return ((Const *) arg)->constvalue;
	if (econtext == NULL)
		return placeholder_value(exprType(arg), &ok);
	return ExecEvalExprSwitchContext(ExecInitExpr((Expr *) arg, NULL), econtext, null);
}

static void
sort_keys(SortSpec *s, const List *keys, ExprContext *econtext)
{
	s->key = palloc(Max(list_length(keys), 1) * sizeof(ScanKeyData));
	s->nkeys = 0;
	foreach_ptr(List, k, keys)
	{
		const List *a = linitial(k);
		bool		null;
		Datum		v = key_value(lsecond(k), econtext, &null);

		s->null_arg |= null;
		ScanKeyEntryInitialize(&s->key[s->nkeys++], list_nth_int(a, 4), list_nth_int(a, 0), list_nth_int(a, 1), (Oid) list_nth_int(a, 2), (Oid) list_nth_int(a, 3),
							   (RegProcedure) list_nth_int(a, 5), v);
	}
}

/* The rows LIMIT and OFFSET ask for together; -1 when the statement is bounded by neither or by a value that is no bound */
static int
bound_rows(const SortSpec *s, ExprContext *econtext)
{
	bool		null;
	int64		n = 0,
				off = 0;
	Datum		v;

	if (s->limit_expr == NULL)
		return -1;
	v = ExecEvalExprSwitchContext(ExecInitExpr((Expr *) s->limit_expr, NULL), econtext, &null);
	if (null)
		return -1;
	n = DatumGetInt64(v);
	v = ExecEvalExprSwitchContext(ExecInitExpr((Expr *) s->offset_expr, NULL), econtext, &null);
	off = null ? 0 : DatumGetInt64(v);
	return n > 0 && off >= 0 && n + off <= INT_MAX ? (int) (n + off) : -1;
}

/* Whether the plan reads a sift_table, which decides the slot of its tuples before the statement runs */
static bool
sort_on_table(const List *l)
{
	return list_nth_int(linitial(l), 0);
}

static bool
sort_is_ios(const List *l)
{
	return list_nth_int(linitial(l), 4);
}

static SortSpec *
sort_unpack(const List *l, ExprContext *econtext)
{
	const List *ints = linitial(l);
	const List *keys = lsecond(l);
	SortSpec   *s = palloc0(sizeof(SortSpec));
	int			at = 0;

	s->table = sort_on_table(l);
	at++;
	s->indexoid = (Oid) list_nth_int(ints, at++);
	s->residual = list_nth_int(ints, at++);
	s->norder = list_nth_int(ints, at++);
	s->ios = list_nth_int(ints, at++);
	s->limit_expr = lfourth(l);
	s->offset_expr = list_nth(l, 4);
	for (int i = 0; i < s->norder; i++)
	{
		s->order_att[i] = list_nth_int(ints, at++);
		s->heap_att[i] = (AttrNumber) list_nth_int(ints, at++);
		s->sortop[i] = (Oid) list_nth_int(ints, at++);
		s->collid[i] = (Oid) list_nth_int(ints, at++);
		s->desc[i] = list_nth_int(ints, at++);
		s->nulls_first[i] = list_nth_int(ints, at++);
	}
	s->iosc.n = list_nth_int(ints, at++);
	for (int i = 0; i < s->iosc.n; i++)
	{
		s->iosc.att[i] = (AttrNumber) list_nth_int(ints, at++);
		s->iosc.leaf[i] = (int16) list_nth_int(ints, at++);
	}
	sort_keys(s, keys, econtext);
	s->limit = bound_rows(s, econtext);
	strlcpy(s->proj, strVal(lthird(l)), sizeof(s->proj));
	return s;
}

/* ---- what the statement asks, as a projection query ---- */

static SiftV6ProjQuery
sort_query(const SortSpec *s, const SiftF2Store *store, const SiftProjPred *preds)
{
	SiftV6ProjQuery q = {.wants_rows = true,.norder = s->norder,.limit = s->limit};

	for (int i = 0; i < s->norder; i++)
	{
		int16		leaf = (int16) store->att_first[s->order_att[i]];

		q.order[i] = (SiftProjKey) {.leaf = leaf,.fn = SPF_COLUMN,.unit = SPU_NONE,.desc = s->desc[i],.nulls_first = s->nulls_first[i]};
		q.read[q.nread++] = leaf;
	}
	for (int k = 0; k < s->nkeys && q.npred < SV6P_MAX; k++)
	{
		q.pred[q.npred++] = sift_v6_proj_query_pred(&preds[k]);
		if (q.nread < lengthof(q.read))
			q.read[q.nread++] = preds[k].leaf;
	}
	for (int i = 0; i < s->iosc.n && s->ios; i++)
		q.read[q.nread++] = s->iosc.leaf[i];
	return q;
}

/* ---- the path ---- */

typedef struct SortPlan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	IndexOptInfo *ix;			/* a sift_col: its index; NULL for a sift_table */
	Relation	owner;			/* the index, or the table */
	SiftF2Store *store;
	SiftProjColumns cols;
	SortSpec	spec;
	List	   *keys;
	double		read_rows;		/* the rows of the projection a statement with residual quals reads to fill its limit */
} SortPlan;

static const Var *
rel_var(const RelOptInfo *rel, const Node *e)
{
	while (e != NULL && IsA(e, RelabelType))
		e = (const Node *) ((const RelabelType *) e)->arg;
	if (e == NULL || !IsA(e, Var))
		return NULL;
	return ((const Var *) e)->varno == rel->relid && ((const Var *) e)->varlevelsup == 0 && ((const Var *) e)->varattno > 0 ? (const Var *) e : NULL;
}

/* The attribute of the relation the store reads a column from: the column of the index, or the attribute of the table; 0: the index does not hold it */
static int
store_att(const SortPlan *p, AttrNumber attno)
{
	if (p->ix == NULL)
		return attno <= p->store->natts ? attno : 0;
	for (int i = 0; i < p->ix->ncolumns; i++)
		if (p->ix->indexkeys[i] == attno)
			return i + 1;
	return 0;
}

static bool
order_type(Oid t, Oid collid)
{
	switch (t)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case BOOLOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case UUIDOID:
			return true;
		case TEXTOID:
		case VARCHAROID:
			return sift_v6_proj_collation_bytewise(collid);
		default:
			return false;
	}
}

static bool
add_order(SortPlan *p, const PathKey *pk)
{
	const EquivalenceClass *ec = pk->pk_eclass;
	const EquivalenceMember *em = linitial(ec->ec_members);
	const Var  *v = list_length(ec->ec_members) == 1 && !ec->ec_has_volatile ? rel_var(p->rel, (const Node *) em->em_expr) : NULL;
	SortSpec   *s = &p->spec;
	int			att = v != NULL ? store_att(p, v->varattno) : 0;
	int			leaf = att > 0 ? p->store->att_first[att] : -1;

	if (leaf < 0 || s->norder == SV6P_MAX || !p->cols.col[leaf].usable || !order_type(v->vartype, ec->ec_collation))
		return false;
	s->order_att[s->norder] = att;
	s->heap_att[s->norder] = v->varattno;
	s->sortop[s->norder] = sift_pathkey_sortop(pk, em->em_datatype);
	s->collid[s->norder] = ec->ec_collation;
	s->desc[s->norder] = sift_pathkey_descending(pk);
	s->nulls_first[s->norder++] = pk->pk_nulls_first;
	return OidIsValid(s->sortop[s->norder - 1]);
}

/* The btree strategy of an operator, 0 when it is none of < <= = >= > */
static StrategyNumber
op_strategy(Oid opno)
{
	foreach_int(strategy, sift_op_strategies(opno))
		if (strategy >= BTLessStrategyNumber && strategy <= BTGreaterStrategyNumber)
			return (StrategyNumber) strategy;
	return 0;
}

/* The key of "column IS [NOT] NULL": the cell of the column says it, and a NULL constant stands for the value */
static List *
null_test_key(SortPlan *p, const NullTest *nt)
{
	const Var  *v = nt->argisrow ? NULL : rel_var(p->rel, (const Node *) nt->arg);
	int			att = v != NULL ? store_att(p, v->varattno) : 0;
	int			flags = SK_ISNULL | (nt->nulltesttype == IS_NULL ? SK_SEARCHNULL : SK_SEARCHNOTNULL);

	if (att == 0 || p->store->att_first[att] < 0)
		return NIL;
	return list_make2(lappend_int(list_make5_int(att, BTEqualStrategyNumber, (int) v->vartype, (int) InvalidOid, flags), 0), makeNullConst(v->vartype, -1, InvalidOid));
}

/* The key of a clause "column op constant" or "column = ANY (constant array)" as the list the plan keeps: ints (attribute, strategy, subtype, collation, flags, function) and the constant */
static List *
qual_key(SortPlan *p, Expr *clause)
{
	Oid			opno,
				subtype,
				collid;
	Node	   *l,
			   *r;
	int			flags = 0;
	const Var  *v;
	int			att;
	StrategyNumber st;

	if (IsA(clause, NullTest))
		return null_test_key(p, (const NullTest *) clause);
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;

		opno = op->opno;
		collid = op->inputcollid;
		l = linitial(op->args);
		r = lsecond(op->args);
		if (IsA(l, Const) && !IsA(r, Const))
		{
			Node	   *t = l;

			l = r;
			r = t;
			opno = get_commutator(opno);
		}
	}
	else if (IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->useOr && list_length(((ScalarArrayOpExpr *) clause)->args) == 2)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;

		opno = sa->opno;
		collid = sa->inputcollid;
		l = linitial(sa->args);
		r = lsecond(sa->args);
		flags = SK_SEARCHARRAY;
	}
	else
		return NIL;
	if (!OidIsValid(opno) || !(IsA(r, Const) ? !((Const *) r)->constisnull : IsA(r, Param) && ((Param *) r)->paramkind == PARAM_EXTERN) ||
		(v = rel_var(p->rel, l)) == NULL || (att = store_att(p, v->varattno)) == 0 || p->store->att_first[att] < 0)
		return NIL;
	if (!IsA(r, Const) && !placeholder_known(exprType(r)))
		return NIL;
	st = op_strategy(opno);
	subtype = flags ? get_element_type(exprType(r)) : exprType(r);
	if (st == 0 || (flags && st != BTEqualStrategyNumber))
		return NIL;
	return list_make2(lappend_int(list_make5_int(att, (int) st, (int) subtype, (int) collid, flags), (int) get_opcode(opno)), r);
}

/* The index's own predicate holds for every row it has: a qual it implies is true of every row of the projection */
static bool
implied_by_index(const SortPlan *p, const RestrictInfo *ri)
{
	return p->ix != NULL && p->ix->indpred != NIL && predicate_implied_by(list_make1(ri->clause), p->ix->indpred, false);
}

/*
 * Every restriction of the relation as a key, or as a residual qual the relation decides on the rows the projection hands out (those are vetted before they count toward the limit); false
 * when one cannot be evaluated twice (a volatile one, a pseudoconstant, a security barrier), or when the residuals select so few rows that filling the limit would vet more than SORT_RESIDUAL_MAX_ROWS.
 */
static bool
compile_quals(SortPlan *p)
{
	List	   *residual = NIL;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		List	   *k;

		if (ri->pseudoconstant || ri->security_level > 0 || contain_volatile_functions((Node *) ri->clause))
			return false;
		k = qual_key(p, ri->clause);
		if (k != NIL)
			p->keys = lappend(p->keys, k);
		else if (!implied_by_index(p, ri))
			residual = lappend(residual, ri);
	}
	p->spec.residual = residual != NIL;
	p->read_rows = p->spec.limit;
	if (residual != NIL)
		p->read_rows = p->spec.limit / Max(clauselist_selectivity(p->root, residual, p->rel->relid, JOIN_INNER, NULL), SORT_RESIDUAL_SEL_MIN);
	return p->read_rows <= SORT_RESIDUAL_MAX_ROWS;
}

/* The attributes the statement reads: every one an integer-class column the store keeps; false when one is not, or the statement has quals only the relation's row can decide */
static bool
compile_ios(SortPlan *p)
{
	Bitmapset  *need = NULL;
	int			attno = -1;
	SortSpec   *s = &p->spec;
	Oid			relid = planner_rt_fetch(p->rel->relid, p->root)->relid;

	if (s->residual)
		return false;
	pull_varattnos((Node *) p->rel->reltarget->exprs, p->rel->relid, &need);
	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, p->rel->relid, &need);
	s->iosc.n = 0;
	while ((attno = bms_next_member(need, attno)) >= 0)
	{
		AttrNumber	att = attno + FirstLowInvalidHeapAttributeNumber;
		int			sa = att > 0 ? store_att(p, att) : 0;
		int			leaf = sa > 0 ? p->store->att_first[sa] : -1;

		if (leaf < 0 || s->iosc.n == SORT_IOS_MAX || !sift_v6_sort_ios_type(get_atttype(relid, att)) || !p->cols.col[leaf].usable || p->cols.col[leaf].kind != SIFT_CELL_INT ||
			p->cols.col[leaf].numeric)
			return false;
		s->iosc.att[s->iosc.n] = att;
		s->iosc.leaf[s->iosc.n++] = (int16) leaf;
	}
	return s->norder + list_length(p->keys) + s->iosc.n <= SIFT_PROJ_MAX_COLS * 2;
}

/* The ready sort projection of the relation that serves the statement by its order; its name, "" when there is none */
static bool
pick_projection(SortPlan *p, const SiftProjPred *preds)
{
	SiftColMetaPageData meta = sift_col_meta_read(p->owner);
	int			nrefs,
				ndefs,
				n = 0;
	SiftManifestProjRef *refs = sift_manifest_read_projs(p->owner, &meta, &nrefs);
	SiftProjDef *defs = sift_proj_defs_load(p->owner, &p->cols, &ndefs);
	const SiftProjSpec **spec = palloc(Max(ndefs, 1) * sizeof(SiftProjSpec *));
	const SiftProjDef **def = palloc(Max(ndefs, 1) * sizeof(SiftProjDef *));
	SiftV6ProjQuery q = sort_query(&p->spec, p->store, preds);
	int			best;

	for (int i = 0; i < ndefs; i++)
	{
		const SiftManifestProjRef *ref = defs[i].spec_ok && defs[i].spec.kind == SIFT_PROJ_SORT ? sift_proj_find(refs, nrefs, defs[i].name) : NULL;

		if (ref != NULL && ref->state == SIFT_PROJ_READY && ref->spec_hash == defs[i].hash)
		{
			spec[n] = &defs[i].spec;
			def[n++] = &defs[i];
		}
	}
	best = sift_v6_proj_pick_sort(spec, n, &p->cols, &q);
	if (best >= 0)
		strlcpy(p->spec.proj, def[best]->name, sizeof(p->spec.proj));
	return best >= 0;
}

/* A LIMIT or OFFSET that is the same for every row: a constant or a parameter, evaluated when the statement runs */
static bool
bound_is_fixed(const Node *e)
{
	return e == NULL || (!contain_var_clause((Node *) e) && !contain_volatile_functions((Node *) e) && !contain_subplans((Node *) e));
}

/* The rows LIMIT and OFFSET ask for together as far as the plan knows them: the constant, else a guess for a parameter */
static int
plan_limit_rows(PlannerInfo *root, const Query *q)
{
	Node	   *count = estimate_expression_value(root, q->limitCount);
	Node	   *offset = q->limitOffset != NULL ? estimate_expression_value(root, q->limitOffset) : NULL;
	int64		n = IsA(count, Const) && !((Const *) count)->constisnull ? DatumGetInt64(((Const *) count)->constvalue) : SORT_LIMIT_GUESS;

	if (offset != NULL && IsA(offset, Const) && !((Const *) offset)->constisnull)
		n += DatumGetInt64(((Const *) offset)->constvalue);
	return n > 0 && n <= INT_MAX ? (int) n : -1;
}

/* No ORDER BY and no LIMIT: the rows the statement's predicates on the leading sort columns select, in any order */
static bool
is_lookup(const PlannerInfo *root)
{
	return root->query_pathkeys == NIL && root->parse->limitCount == NULL && root->parse->limitOffset == NULL;
}

static bool
ordered_limit(PlannerInfo *root)
{
	Query	   *q = root->parse;

	return root->query_pathkeys != NIL && root->query_pathkeys == root->sort_pathkeys && q->limitCount != NULL &&
		bound_is_fixed(q->limitCount) && bound_is_fixed(q->limitOffset) && plan_limit_rows(root, q) > 0 && q->limitOption == LIMIT_OPTION_COUNT;
}

static bool
shape_ok(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Query	   *q = root->parse;

	return q->commandType == CMD_SELECT && (is_lookup(root) || ordered_limit(root)) && !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs &&
		q->groupClause == NIL && q->distinctClause == NIL && q->setOperations == NULL && !agg_query_has_rls(root) && q->rowMarks == NIL && rel->reloptkind == RELOPT_BASEREL &&
		bms_membership(root->all_baserels) == BMS_SINGLETON && rel->lateral_relids == NULL && rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh &&
		rte->tablesample == NULL && agg_rel_security_ok(root, rel);
}

/* The statement on one owner (the table or one index); false when the owner cannot serve it */
static bool
compile_on(SortPlan *p)
{
	SiftProjPred *preds;
	const bool	lookup = is_lookup(p->root);

	if (!sift_proj_relation_has(p->owner) || (lookup && p->rel->rows > SORT_LOOKUP_MAX_ROWS))
		return false;
	p->store = sift_f2_store_open(p->owner);
	p->cols = sift_proj_columns_of_store(p->owner, p->store);
	p->spec = (SortSpec) {.table = p->ix == NULL,.indexoid = p->ix != NULL ? p->ix->indexoid : InvalidOid,.limit = lookup ? (int) clamp_row_est(p->rel->rows) : plan_limit_rows(p->root, p->root->parse),
		.limit_expr = p->root->parse->limitCount,.offset_expr = p->root->parse->limitOffset != NULL ? p->root->parse->limitOffset : (Node *) makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(0), false, true)};
	p->keys = NIL;
	foreach_ptr(PathKey, pk, p->root->query_pathkeys)
		if (!add_order(p, pk))
			return false;
	if (!compile_quals(p))
		return false;
	sort_keys(&p->spec, p->keys, NULL);
	if (!sift_v6_proj_preds_of_keys(p->store, RelationGetDescr(p->owner), p->spec.key, p->spec.nkeys, &preds, CurrentMemoryContext))
		return false;
	p->spec.ios = compile_ios(p);
	if (p->spec.ios && pick_projection(p, preds))
		return true;
	p->spec.ios = false;
	p->spec.iosc.n = 0;
	return pick_projection(p, preds);
}

static void
add_sort_path(SortPlan *p)
{
	CustomPath *cp = makeNode(CustomPath);
	double		rows = clamp_row_est(Min((double) p->spec.limit, p->rel->rows)),
				read = clamp_row_est(Min(p->read_rows, p->rel->tuples));

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = p->rel;
	cp->path.pathtarget = p->rel->reltarget;
	cp->path.pathkeys = p->root->query_pathkeys;
	cp->path.rows = rows;
	cp->path.startup_cost = 100 * cpu_operator_cost;
	cp->path.total_cost = cp->path.startup_cost + read * (cpu_tuple_cost + (is_lookup(p->root) ? cpu_index_tuple_cost : random_page_cost * (p->spec.ios ? 1.0 - p->rel->allvisfrac : 1.0)));
	cp->methods = &sort_path_methods;
	cp->custom_private = sort_pack(&p->spec, p->keys);
	add_path(p->rel, &cp->path);
}

static void
sort_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	SortPlan	p = {.root = root,.rel = rel};
	Relation	heap;

	if (prev_pathlist_hook != NULL)
		prev_pathlist_hook(root, rel, rti, rte);
	if (!shape_ok(root, rel, rte))
		return;
	heap = table_open(rte->relid, AccessShareLock);
	if (sift_tbl_is_table(heap) && sift_tbl_ops(heap) == &sift_tbl_v2_ops)
	{
		p.owner = heap;
		if (compile_on(&p))
			add_sort_path(&p);
	}
	else
		foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		{
			Relation	irel;

			if ((ix->indpred != NIL && !ix->predOK) || ix->hypothetical || !sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid))
				continue;
			irel = index_open(ix->indexoid, AccessShareLock);
			p.ix = ix;
			p.owner = irel;
			if (!sift_col_rel(irel)->stale && compile_on(&p))
				add_sort_path(&p);
			index_close(irel, NoLock);
		}
	table_close(heap, NoLock);
}

bool
sift_v6_proj_sort_offered(const RelOptInfo *rel)
{
	bool		offered = false;

	foreach_ptr(Path, path, rel->pathlist)
		offered |= IsA(path, CustomPath) && ((const CustomPath *) path)->methods == &sort_path_methods;
	return offered;
}

static Plan *
sort_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	Oid			indexoid = (Oid) list_nth_int(linitial(best->custom_private), 1);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_private = best->custom_private;
	cs->methods = &sort_plan_methods;
	if (OidIsValid(indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, indexoid);
	return &cs->scan.plan;
}

/* ---- the node ---- */

typedef struct ProjSortState
{
	CustomScanState css;
	SortSpec   *spec;
	Relation	index;
	SiftF2Store *store;
	SiftV6ProjHost host;
	MemoryContext cx;
	SiftV6SortScan *scan;
	Tuplesortstate *sort;
	TupleTableSlot *vet_slot;
	IndexFetchTableData *fetch;	/* the relation's index fetch: on a sift_col the heap's, which follows a HOT chain from the root the projection holds; on a sift_table the lazy one, which decodes only the columns the statement reads */
	TupleTableSlot *sort_slot;
	Buffer		vmbuf;			/* ios: the page of the relation's visibility map in hand */
	int64		nfetch;			/* ios: the rows the relation had to give because their page is not all visible */
	TableScanDesc rel_scan;		/* no projection serves a statement with no ORDER BY: the relation is scanned and the quals of the node keep its rows */
	bool		began;
} ProjSortState;

static Relation
owner_of(const ProjSortState *st)
{
	return st->index != NULL ? st->index : st->css.ss.ss_currentRelation;
}

/* The version of the row at tid that the statement's snapshot sees, in slot: a sift_col row is found from the address of its index entry, the root of its HOT chain */
static bool
fetch_visible(ProjSortState *st, uint64 tid, TupleTableSlot *slot)
{
	ItemPointerData ip;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&ip, tid);
	return table_index_fetch_tuple(st->fetch, &ip, st->css.ss.ps.state->es_snapshot, slot, &again, &dead);
}

/* The row of vet_slot in the kind of slot the quals and the target list of the node were compiled for: a statement that reads cells only has a virtual slot of its own, which the relation's tuple is copied to */
static TupleTableSlot *
scan_slot_of_vet(ProjSortState *st)
{
	TupleTableSlot *scan = st->css.ss.ss_ScanTupleSlot;

	return scan->tts_ops == st->vet_slot->tts_ops ? st->vet_slot : ExecCopySlot(scan, st->vet_slot);
}

/* The version of the row at tid the snapshot sees, in the slot of the scan */
static bool
fetch_row(ProjSortState *st, uint64 tid, TupleTableSlot *slot)
{
	if (!st->spec->ios)
		return fetch_visible(st, tid, slot);
	st->nfetch++;
	if (!fetch_visible(st, tid, st->vet_slot))
		return false;
	ExecCopySlot(slot, st->vet_slot);
	return true;
}

/* Whether the row at tid is visible and the statement's quals hold on it: the row of a cell the projection could not judge */
static bool
vet_row(void *arg, uint64 tid)
{
	ProjSortState  *st = arg;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	bool		ok;

	if (!fetch_visible(st, tid, st->vet_slot))
		return false;
	econtext->ecxt_scantuple = scan_slot_of_vet(st);
	ok = ExecQual(st->css.ss.ps.qual, econtext);
	ResetExprContext(econtext);
	return ok;
}

/* No projection serves the statement: the relation's rows that satisfy its quals, the first limit of them in the order of the statement (all of them, as they come, when it has no order) */
static void
sort_relation(ProjSortState *st)
{
	const SortSpec *s = st->spec;
	Relation	heap = st->css.ss.ss_currentRelation;
	TupleDesc	td = RelationGetDescr(heap);
	TableScanDesc scan = sift_table_beginscan(heap, st->css.ss.ps.state->es_snapshot, 0, NULL);
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	MemoryContext old;

	if (s->norder == 0)
	{
		st->rel_scan = scan;
		return;
	}
	old = MemoryContextSwitchTo(st->cx);

	st->sort = tuplesort_begin_heap(td, s->norder, (AttrNumber *) s->heap_att, (Oid *) s->sortop, (Oid *) s->collid, (bool *) s->nulls_first, work_mem, NULL, TUPLESORT_ALLOWBOUNDED);
	st->sort_slot = MakeSingleTupleTableSlot(td, &TTSOpsMinimalTuple);
	MemoryContextSwitchTo(old);
	if (s->limit > 0)
		tuplesort_set_bound(st->sort, s->limit);
	while (table_scan_getnextslot(scan, ForwardScanDirection, st->vet_slot))
	{
		econtext->ecxt_scantuple = scan_slot_of_vet(st);
		if (ExecQual(st->css.ss.ps.qual, econtext))
			tuplesort_puttupleslot(st->sort, st->vet_slot);
		ResetExprContext(econtext);
	}
	table_endscan(scan);
	tuplesort_performsort(st->sort);
}

static void
sort_begin_scan(ProjSortState *st)
{
	const SortSpec *s = st->spec;
	SiftProjPred *preds;
	SiftV6ProjQuery q;
	MemoryContext old;

	st->began = true;
	st->cx = sift_mem_allocset_create(st->css.ss.ps.state->es_query_cxt, "sift proj sort scan");
	old = MemoryContextSwitchTo(st->cx);
	st->vet_slot = table_slot_create(st->css.ss.ss_currentRelation, NULL);
	st->fetch = sift_table_index_fetch_begin(st->css.ss.ss_currentRelation);
	st->host = (SiftV6ProjHost) {.rel = owner_of(st),.store = st->store,.heap = st->index != NULL ? st->css.ss.ss_currentRelation : NULL,
		.snap = st->css.ss.ps.state->es_snapshot,.keys = st->index != NULL ? s->key : NULL,.nkeys = st->index != NULL ? s->nkeys : 0,.npreds = s->nkeys};
	if (!s->null_arg && sift_v6_proj_preds_of_keys(st->store, RelationGetDescr(owner_of(st)), s->key, s->nkeys, &preds, st->cx))
	{
		st->host.preds = preds;
		q = sort_query(s, st->store, preds);
		st->scan = sift_v6_proj_sort_begin(&st->host, &q, vet_row, st, s->residual, st->cx);
	}
	if (st->scan == NULL)
		sort_relation(st);
	MemoryContextSwitchTo(old);
}

static void
sort_end_scan(ProjSortState *st)
{
	if (st->scan != NULL)
		sift_v6_proj_sort_end(st->scan);
	if (st->sort != NULL)
		tuplesort_end(st->sort);
	if (st->rel_scan != NULL)
		table_endscan(st->rel_scan);
	if (st->fetch != NULL)
		table_index_fetch_end(st->fetch);
	if (BufferIsValid(st->vmbuf))
		ReleaseBuffer(st->vmbuf);
	if (st->vet_slot != NULL)
		ExecDropSingleTupleTableSlot(st->vet_slot);
	if (st->sort_slot != NULL)
		ExecDropSingleTupleTableSlot(st->sort_slot);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->scan = NULL;
	st->sort = NULL;
	st->rel_scan = NULL;
	st->vet_slot = st->sort_slot = NULL;
	st->vmbuf = InvalidBuffer;
	st->nfetch = 0;
	st->fetch = NULL;
	st->cx = NULL;
	st->began = false;
}

static TupleTableSlot *
sort_next(ScanState *ss)
{
	ProjSortState  *st = (ProjSortState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	uint64		tid;
	const SiftCell *row;

	if (!st->began)
		sort_begin_scan(st);
	ExecClearTuple(slot);
	if (st->rel_scan != NULL)
		return table_scan_getnextslot(st->rel_scan, ForwardScanDirection, st->vet_slot) ? ExecCopySlot(slot, st->vet_slot) : NULL;
	if (st->sort != NULL)
		return tuplesort_gettupleslot(st->sort, true, false, st->sort_slot, NULL) ? ExecCopySlot(slot, st->sort_slot) : NULL;
	while (sift_v6_proj_sort_next(st->scan, &tid, &row))
	{
		if (st->spec->ios && sift_v6_sort_ios_visible(ss->ss_currentRelation, tid, &st->vmbuf) && sift_v6_sort_ios_fill(slot, &st->spec->iosc, row))
			return slot;
		if (fetch_row(st, tid, slot))
			return slot;
	}
	return NULL;
}

static bool
sort_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static void
sort_begin(CustomScanState *node, EState *estate, int eflags)
{
	ProjSortState  *st = (ProjSortState *) node;

	st->spec = sort_unpack(((CustomScan *) node->ss.ps.plan)->custom_private, node->ss.ps.ps_ExprContext);
	if (!st->spec->table)
		st->index = index_open(st->spec->indexoid, AccessShareLock);
	st->store = sift_f2_store_open(owner_of(st));
}

static TupleTableSlot *
sort_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, sort_next, sort_recheck);
}

static void
sort_end(CustomScanState *node)
{
	ProjSortState  *st = (ProjSortState *) node;

	sort_end_scan(st);
	if (st->index != NULL)
		index_close(st->index, AccessShareLock);
}

static void
sort_rescan(CustomScanState *node)
{
	sort_end_scan((ProjSortState *) node);
	ExecScanReScan(&node->ss);
}

static void
sort_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ProjSortState  *st = (ProjSortState *) node;

	ExplainPropertyText("Projection", st->scan != NULL ? sift_v6_proj_sort_name(st->scan) : st->spec->proj, es);
	if (st->spec->ios)
		ExplainPropertyBool("Index Only", true, es);
	if (es->analyze && st->began)
	{
		SiftV6SortStats stats = st->scan != NULL ? sift_v6_proj_sort_stats(st->scan) : (SiftV6SortStats) {0};

		ExplainPropertyText("Source", st->scan != NULL ? "projection" : "relation, bounded sort", es);
		if (st->spec->ios && st->scan != NULL)
			ExplainPropertyInteger("Heap Fetches", NULL, st->nfetch, es);
		ExplainPropertyInteger("Projection Parts", NULL, stats.nparts, es);
		ExplainPropertyInteger("Projection Parts Sought", NULL, stats.nseeked, es);
		ExplainPropertyInteger("Projection Granules Skipped", NULL, stats.granules_skipped, es);
		ExplainPropertyInteger("Projection Granules Read", NULL, stats.granules_read, es);
		ExplainPropertyInteger("Projection Base Parts", NULL, stats.nbase, es);
		ExplainPropertyInteger("Projection Base Rows", NULL, stats.nrest_rows, es);
	}
}

static const CustomExecMethods sort_exec_methods = {
	.CustomName = "SiftProjSortScan",
	.BeginCustomScan = sort_begin,
	.ExecCustomScan = sort_exec,
	.EndCustomScan = sort_end,
	.ReScanCustomScan = sort_rescan,
	.ExplainCustomScan = sort_explain,
};

static Node *
sort_create_state(CustomScan *cscan)
{
	ProjSortState  *st = (ProjSortState *) newNode(sizeof(ProjSortState), T_CustomScanState);

	st->css.methods = &sort_exec_methods;
	st->css.slotOps = sort_on_table(cscan->custom_private) ? sift_f2_slot_ops() : sort_is_ios(cscan->custom_private) ? &TTSOpsVirtual : &TTSOpsBufferHeapTuple;
	return (Node *) st;
}

static const CustomPathMethods sort_path_methods = {"SiftProjSortScan", sort_plan_path, NULL};
static const CustomScanMethods sort_plan_methods = {"SiftProjSortScan", sort_create_state};

void
sift_v6_proj_sort_plan_init(void)
{
	RegisterCustomScanMethods(&sort_plan_methods);
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sort_pathlist;
}
