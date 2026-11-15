/*
 * agg/flat/invert.c - the parents that have a child are the distinct keys of the children that have a parent
 * `SELECT count(*) FROM parent p WHERE EXISTS (SELECT 1 FROM child c WHERE c.fk = p.key AND q)` reads the child once for each parent, or all of the child's rows that
 * q keeps loaded into a semi table. Where p.key is unique (a unique btree on it alone) each key that a child holds is the key of at most one parent, so the count is
 * `SELECT count(DISTINCT c.fk) FROM child c WHERE q AND EXISTS (SELECT 1 FROM parent p WHERE p.key = c.fk AND <the conditions on p>)`: the scan of the child, with the
 * parent a semi join of its rows (which a connected index answers from the flag of the connection, col/conn.h), and the parent's rows not read at all. A NULL key matches
 * no row on either side. NOT EXISTS is the parents the conditions keep, less those: `SELECT count(*) - (<the count above>) FROM parent p WHERE <conditions>`.
 * The planner hook plans the statement both ways and keeps this one where it costs less.
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

#define FLAT_INVERT_MARGIN 1.0

bool		sift_agg_child_first = true;

/* the statement: the parent (rti prti of the query), the child of its EXISTS and the one key that ties them */
typedef struct Invert
{
	Query	   *q;
	Query	   *sub;			/* the child's sub-select */
	Index		prti;
	Var		   *pkey;			/* the parent's key */
	Var		   *ckey;			/* the child's column that equals it */
	OpExpr	   *eq;
	List	   *prest;			/* the other conditions, all on the parent */
	List	   *crest;			/* the other conditions of the sub-select, all on the child */
	bool		not;
} Invert;

static bool
plain_condition(Node *c)
{
	return !checkExprHasSubLink(c) && !contain_volatile_functions(c);
}

/* count(*) alone */
static bool
counts_rows(Query *q)
{
	TargetEntry *te;
	Aggref	   *a;

	if (list_length(q->targetList) != 1)
		return false;
	te = linitial_node(TargetEntry, q->targetList);
	a = (Aggref *) te->expr;
	return IsA(a, Aggref) && a->aggfnoid == F_COUNT_ && a->agglevelsup == 0 && a->aggfilter == NULL && a->aggorder == NIL && a->aggdistinct == NIL;
}

/* [NOT] EXISTS (SELECT .. FROM child WHERE c.fk = p.key AND <child's conditions>) with the key unique in the parent, and nothing else of the child's but conditions */
static bool
tie_of(Invert *iv, SubLink *sl)
{
	Query	   *sq = castNode(Query, sl->subselect);
	Index		crti = flat_only_table(sq);
	Node	   *outer;
	Var		   *pk;
	bool		tied = false;

	if (crti == 0 || sq->hasSubLinks || sq->jointree->quals == NULL || !has_column_source(rt_fetch(crti, sq->rtable)->relid))
		return false;
	foreach_ptr(Node, c, flat_and_list(sq->jointree->quals))
	{
		if (!tied && flat_key_test(c, crti, 1, &iv->ckey, &outer, &iv->eq))
		{
			tied = true;
			outer = strip_relabel(outer);
			if (!IsA(outer, Var))
				return false;
			pk = (Var *) outer;
			if (pk->varno != iv->prti || pk->varlevelsup != 1)
				return false;
			iv->pkey = pk;
		}
		else if (flat_reads_outside(c) || !plain_condition(c))
			return false;
		else
			iv->crest = lappend(iv->crest, c);
	}
	iv->sub = sq;
	return tied;
}

static bool
analyze(Query *q, Invert *iv)
{
	Index		prti = flat_from_table(q);
	RangeTblEntry *prte;
	SubLink    *link = NULL;

	*iv = (Invert) {.q = q, .prti = prti};
	if (prti == 0 || q->commandType != CMD_SELECT || !q->hasAggs || !q->hasSubLinks || q->hasWindowFuncs || q->hasTargetSRFs || q->setOperations != NULL || q->cteList != NIL ||
		q->rowMarks != NIL || q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->distinctClause != NIL || q->sortClause != NIL ||
		q->limitCount != NULL || q->limitOffset != NULL || !counts_rows(q))
		return false;
	prte = rt_fetch(prti, q->rtable);
	if (prte->relkind != RELKIND_RELATION || has_subclass(prte->relid) || q->jointree->quals == NULL)
		return false;
	foreach_ptr(Node, c, flat_and_list(q->jointree->quals))
	{
		bool		not = IsA(c, BoolExpr) && ((BoolExpr *) c)->boolop == NOT_EXPR;
		Node	   *n = not ? linitial(((BoolExpr *) c)->args) : c;

		if (link == NULL && IsA(n, SubLink) && ((SubLink *) n)->subLinkType == EXISTS_SUBLINK)
		{
			link = (SubLink *) n;
			iv->not = not;
		}
		else if (checkExprHasSubLink(c) || contain_volatile_functions(c))
			return false;
		else
			iv->prest = lappend(iv->prest, c);
	}
	if (link == NULL || !tie_of(iv, link))
		return false;
	return iv->pkey->vartype == iv->ckey->vartype && (iv->pkey->vartype == INT2OID || iv->pkey->vartype == INT4OID || iv->pkey->vartype == INT8OID) &&
		sift_agg_unique_key(prte->relid, 1, 0, &(int){iv->pkey->varattno}, NULL);
}

static Query *
new_select(Query *like)
{
	Query	   *r = makeNode(Query);

	r->commandType = CMD_SELECT;
	r->querySource = QSRC_ORIGINAL;
	r->canSetTag = like->canSetTag;
	return r;
}

/* the count of the distinct values of the child's column */
static Aggref *
count_distinct(Var *col)
{
	Aggref	   *a = makeNode(Aggref);
	TargetEntry *arg = makeTargetEntry((Expr *) copyObject(col), 1, NULL, false);
	SortGroupClause *g = makeNode(SortGroupClause);
	Oid			lt,
				eq;

	arg->ressortgroupref = 1;
	get_sort_group_operators(col->vartype, true, true, false, &lt, &eq, NULL, &g->hashable);
	g->tleSortGroupRef = 1;
	g->eqop = eq;
	g->sortop = lt;
	a->aggfnoid = F_COUNT_ANY;
	a->aggtype = INT8OID;
	a->aggargtypes = list_make1_oid(col->vartype);
	a->args = list_make1(arg);
	a->aggdistinct = list_make1(g);
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return a;
}

static Node *
and_of(List *quals)
{
	return quals == NIL ? NULL : list_length(quals) == 1 ? linitial(quals) : (Node *) makeBoolExpr(AND_EXPR, quals, -1);
}

/* the parents, `SELECT 1 FROM parent WHERE p.key = <the child's column> AND <their conditions>`, as the EXISTS the child's rows are tested by */
static SubLink *
parent_exists(Invert *iv)
{
	Query	   *p = new_select(iv->q);
	SubLink    *sl = makeNode(SubLink);
	Var		   *outer = copyObject(iv->ckey);
	OpExpr	   *eq = copyObject(iv->eq);
	Var		   *pk = copyObject(iv->pkey);

	outer->varlevelsup = 1;
	pk->varlevelsup = 0;
	eq->args = list_make2(pk, outer);
	p->rtable = copyObject(iv->q->rtable);
	p->rteperminfos = copyObject(iv->q->rteperminfos);
	p->jointree = makeFromExpr(copyObject(iv->q->jointree->fromlist), and_of(lcons(eq, copyObject(iv->prest))));
	p->targetList = list_make1(makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true), 1, NULL, false));
	sl->subLinkType = EXISTS_SUBLINK;
	sl->subselect = (Node *) p;
	sl->location = -1;
	return sl;
}

/* `SELECT count(DISTINCT c.fk) FROM child c WHERE <the child's conditions> AND EXISTS (the parents)` */
static Query *
keys_query(Invert *iv)
{
	Query	   *k = new_select(iv->q);
	Query	   *sq = iv->sub;

	k->rtable = copyObject(sq->rtable);
	k->rteperminfos = copyObject(sq->rteperminfos);
	k->jointree = makeFromExpr(copyObject(sq->jointree->fromlist), and_of(lappend(copyObject(iv->crest), parent_exists(iv))));
	k->targetList = list_make1(makeTargetEntry((Expr *) count_distinct(iv->ckey), 1, "count", false));
	k->hasAggs = k->hasSubLinks = true;
	return k;
}

static SubLink *
scalar_of(Query *sub)
{
	SubLink    *sl = makeNode(SubLink);

	sl->subLinkType = EXPR_SUBLINK;
	sl->subLinkId = 0;
	sl->subselect = (Node *) sub;
	sl->location = -1;
	return sl;
}

/* the statement's own fields stay: the query is the one the caller holds */
static void
replace(Query *q, Query *with)
{
	with->queryId = q->queryId;
	with->stmt_location = q->stmt_location;
	with->stmt_len = q->stmt_len;
	*q = *with;
}

/* count(*) of the parents the conditions keep, less the keys of the children that have one */
static void
subtract_keys(Invert *iv, Query *keys)
{
	Query	   *q = iv->q;
	TargetEntry *te = linitial_node(TargetEntry, q->targetList);
	Oid			minus = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString("-")), INT8OID, INT8OID);
	OpExpr	   *diff = (OpExpr *) make_opclause(minus, INT8OID, false, te->expr, (Expr *) scalar_of(keys), InvalidOid, InvalidOid);

	diff->opfuncid = F_INT8MI;
	te->expr = (Expr *) diff;
	q->jointree->quals = and_of(iv->prest);
}

bool
sift_agg_invert_parents(Query *q, bool apply)
{
	Invert		iv;
	Query	   *keys;

	if (!sift_agg_child_first || !analyze(q, &iv))
		return false;
	if (!apply)
		return true;
	keys = keys_query(&iv);
	if (iv.not)
		subtract_keys(&iv, keys);
	else
		replace(q, keys);
	sift_agg_by_cost = sift_agg_flat_any = true;
	sift_agg_flat_margin = FLAT_INVERT_MARGIN;
	return true;
}
