/*
 * agg/flat/partarms.c - the top-N of a partitioned table that the query around it runs for each of its rows, as one top-N for each partition
 * `LATERAL (SELECT .. FROM ev WHERE ev.tenant = d.id ORDER BY ts DESC LIMIT 1)` over a partitioned ev is a scan of every partition and a sort for each row of d unless
 * the plan reads each partition's top-N: the scan keeps its state of one table from one row of d to the next, not of the partitions as one source. The same
 * query as `SELECT .. FROM (partition 1 ORDER BY .. LIMIT n UNION ALL partition 2 ORDER BY .. LIMIT n ..) ORDER BY .. LIMIT n` is n rows of each partition, merged
 * by a Merge Append, and when the first ORDER BY column is the key of a range partitioning, the partitions in the order of the sort with no sort at all: the
 * first partitions answer and the others are never executed. A hash partitioning is read where the query pins its key to a value (`tenant = d.id`): each arm tests that the value
 * is in its partition and only that one runs. The planner hook plans the query both ways and keeps this one only if it costs less.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/plan_int.h"
#include "agg/topn.h"

/* the columns of the partitioned table the query reads, in the order of the columns of the union */
typedef struct ArmCols
{
	List	   *attnos;
} ArmCols;

static int
arm_pos(ArmCols *cols, AttrNumber attno)
{
	foreach_int(a, cols->attnos)
		if (a == attno)
			return foreach_current_index(a) + 1;
	return 0;
}

/* the columns of the table that the query reads */
static void
arm_cols(Query *q, ArmCols *cols)
{
	Bitmapset  *used = NULL;
	int			x = -1;

	pull_varattnos((Node *) q->targetList, 1, &used);
	pull_varattnos(q->jointree->quals, 1, &used);
	cols->attnos = NIL;
	while ((x = bms_next_member(used, x)) >= 0)
		cols->attnos = lappend_int(cols->attnos, x + FirstLowInvalidHeapAttributeNumber);
}

/* a column of the table that the arms and the query above them read, as a column of the union */
static Node *
remap_mutator(Node *n, ArmCols *cols)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varno == 1 && ((Var *) n)->varlevelsup == 0)
	{
		Var		   *v = copyObject((Var *) n);

		v->varno = v->varnosyn = 2;
		v->varattno = v->varattnosyn = arm_pos(cols, v->varattno);
		return (Node *) v;
	}
	return expression_tree_mutator(n, remap_mutator, cols);
}

/* the value a condition of the query pins the column of a hash partitioning to (`tenant = d.id`), which the arms test to read only the partition that holds it */
static Node *
hash_pin(Query *q, PartitionKey key)
{
	Node	   *quals = q->jointree->quals;
	List	   *conj = IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR ? ((BoolExpr *) quals)->args : list_make1(quals);

	if (key->partnatts != 1 || key->partattrs[0] == 0)
		return NULL;
	foreach_ptr(Node, c, conj)
	{
		OpExpr	   *op = (OpExpr *) c;

		if (!IsA(c, OpExpr) || list_length(op->args) != 2 || get_op_opfamily_strategy(op->opno, key->partopfamily[0]) != HTEqualStrategyNumber)
			continue;
		for (int i = 0; i < 2; i++)
		{
			Node	   *v = list_nth(op->args, i),
					   *other = list_nth(op->args, 1 - i);

			if (IsA(v, Var) && ((Var *) v)->varno == 1 && ((Var *) v)->varlevelsup == 0 && ((Var *) v)->varattno == key->partattrs[0] && !contain_vars_of_level(other, 0) &&
				exprType(other) == key->parttypid[0] && exprCollation(other) == key->partcollation[0])
				return other;
		}
	}
	return NULL;
}

/* whether the value pinned is in the partition: the arm is a gating Result of a one-time filter, and the others of the table are not executed for the row */
static Node *
hash_arm_qual(Oid parent, Oid leaf, Node *pin)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(leaf));
	PartitionBoundSpec *bs;

	SIFT_INVARIANT(HeapTupleIsValid(tp), "cache lookup failed for relation %u", leaf);
	bs = castNode(PartitionBoundSpec, stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(RELOID, tp, Anum_pg_class_relpartbound))));
	ReleaseSysCache(tp);
	return (Node *) makeFuncExpr(F_SATISFIES_HASH_PARTITION, BOOLOID,
								 list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(parent), false, true),
											makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(bs->modulus), false, true),
											makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(bs->remainder), false, true), copyObject(pin)),
								 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

/* the leaves of the table, or NIL where a partition is not a heap table of the same columns as the table's (*pin: the value of a hash partitioning the query pins) */
static List *
leaf_oids(Oid relid, ArmCols *cols, bool *ordered, bool *asc, Query *q, Node **pin)
{
	List	   *leaves = NIL;
	Relation	rel = relation_open(relid, NoLock);
	PartitionKey key = RelationGetPartitionKey(rel);
	PartitionDesc desc = RelationGetPartitionDesc(rel, true);
	SortGroupClause *first = linitial_node(SortGroupClause, q->sortClause);
	Node	   *e = (Node *) get_sortgroupclause_expr(first, q->targetList);
	Oid			opfamily,
				opcintype;
	SiftCmpStrategy strategy;
	bool		ok = true;

	*ordered = false;
	*pin = NULL;
	foreach_oid(leaf, find_all_inheritors(relid, AccessShareLock, NULL))
	{
		char		kind = get_rel_relkind(leaf);

		if (kind == RELKIND_PARTITIONED_TABLE)
			continue;
		ok &= kind == RELKIND_RELATION && pg_class_aclcheck(leaf, GetUserId(), ACL_SELECT) == ACLCHECK_OK;
		foreach_int(a, cols->attnos)
			if (a <= 0 || get_attnum(leaf, get_attname(relid, a, false)) != a)
				ok = false;
		leaves = lappend_oid(leaves, leaf);
	}
	/* the partitions in the order of the first ORDER BY column: the key of a range partitioning with no default partition whose partitions are all leaves */
	if (ok && key->strategy == PARTITION_STRATEGY_RANGE && key->partnatts == 1 && key->partattrs[0] != 0 && !OidIsValid(get_default_partition_oid(relid)) &&
		IsA(e, Var) && ((Var *) e)->varattno == key->partattrs[0] && ((Var *) e)->varlevelsup == 0 && exprCollation(e) == key->partcollation[0] &&
		get_ordering_op_properties(first->sortop, &opfamily, &opcintype, &strategy) && opfamily == key->partopfamily[0])
	{
		*ordered = list_length(leaves) == desc->nparts;
		*asc = strategy == SIFT_CMP_LT;
		if (*ordered)
		{
			leaves = NIL;
			for (int i = 0; i < desc->nparts; i++)
				leaves = *asc ? lappend_oid(leaves, desc->oids[i]) : lcons_oid(desc->oids[i], leaves);
		}
	}
	if (key->strategy == PARTITION_STRATEGY_HASH)
	{
		/* no other order than the sort, and a partition to read only where the query names the value of the key */
		*pin = hash_pin(q, key);
		ok &= *pin != NULL && list_length(leaves) == desc->nparts;
	}
	relation_close(rel, NoLock);
	return ok ? leaves : NIL;
}

/* the n rows of one partition that the sorted query asks for */
static Query *
arm_query(Query *q, RangeTblEntry *parent, Oid leaf, ArmCols *cols, int64 rows, int up)
{
	Query	   *arm = makeNode(Query);
	RangeTblEntry *lr = copyObject(parent);
	RangeTblRef *rtr = makeNode(RangeTblRef);
	RTEPermissionInfo *pi;
	int			ref = 0;

	/* the partition is read as its own table, whose privilege the arm checks (the table's stays with the query) */
	pi = copyObject(getRTEPermissionInfo(q->rteperminfos, parent));
	pi->relid = leaf;
	pi->inh = false;
	arm->rteperminfos = list_make1(pi);
	lr->relid = leaf;
	lr->relkind = RELKIND_RELATION;
	lr->inh = false;
	lr->perminfoindex = 1;
	rtr->rtindex = 1;
	arm->commandType = CMD_SELECT;
	arm->canSetTag = true;
	arm->rtable = list_make1(lr);
	/* the conditions are those of the query, whose outer references are now farther by the union and the query that reads it (and the gate of the arm) */
	arm->jointree = makeFromExpr(list_make1(rtr), copyObject(q->jointree->quals));
	IncrementVarSublevelsUp(arm->jointree->quals, up, 1);
	foreach_int(a, cols->attnos)
	{
		Oid			t,
					c;
		int32		m;

		get_atttypetypmodcoll(parent->relid, a, &t, &m, &c);
		arm->targetList = lappend(arm->targetList, makeTargetEntry((Expr *) makeVar(1, a, t, m, c, 0), foreach_current_index(a) + 1, get_attname(parent->relid, a, false), false));
	}
	foreach_node(SortGroupClause, s, q->sortClause)
	{
		SortGroupClause *c = copyObject(s);
		TargetEntry *te = list_nth_node(TargetEntry, arm->targetList, arm_pos(cols, ((Var *) get_sortgroupclause_expr(s, q->targetList))->varattno) - 1);

		if (te->ressortgroupref == 0)
			te->ressortgroupref = ++ref;
		c->tleSortGroupRef = te->ressortgroupref;
		arm->sortClause = lappend(arm->sortClause, c);
	}
	arm->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(rows), false, FLOAT8PASSBYVAL);
	return arm;
}

/*
 * The arm of a hash partition under a test that its value of the key is in the partition: a condition that no column of the arm names is a one-time filter of the
 * Result above it, which the executor tests once for each row of the query around and does not run the arm for when it fails. In the arm itself the scan would take it
 * for a condition of its rows, after opening the index. The LIMIT keeps the test out of the arm.
 */
static Query *
gated_arm(Query *inner, Oid parent, Oid leaf, Node *pin, ArmCols *cols, List *names)
{
	Query	   *arm = makeNode(Query);
	RangeTblEntry *ir = makeNode(RangeTblEntry);
	RangeTblRef *rtr = makeNode(RangeTblRef);
	Node	   *gate = hash_arm_qual(parent, leaf, pin);

	IncrementVarSublevelsUp(gate, 2, 1);
	ir->rtekind = RTE_SUBQUERY;
	ir->subquery = inner;
	ir->inFromCl = true;
	ir->eref = makeAlias("arm", names);
	rtr->rtindex = 1;
	arm->commandType = CMD_SELECT;
	arm->canSetTag = true;
	arm->rtable = list_make1(ir);
	arm->jointree = makeFromExpr(list_make1(rtr), gate);
	foreach_int(a, cols->attnos)
	{
		Oid			t,
					c;
		int32		m;

		get_atttypetypmodcoll(parent, a, &t, &m, &c);
		arm->targetList = lappend(arm->targetList, makeTargetEntry((Expr *) makeVar(1, foreach_current_index(a) + 1, t, m, c, 0), foreach_current_index(a) + 1, get_attname(parent, a, false), false));
	}
	return arm;
}

bool
sift_agg_partition_arms(Query *q, bool apply)
{
	RangeTblEntry *parent,
			   *ur;
	ArmCols		cols;
	Query	   *u;
	List	   *leaves,
			   *names = NIL;
	Node	   *tree = NULL,
			   *limit,
			   *skip,
			   *pin;
	bool		ordered,
				asc = true;
	int64		count,
				offset = 0;

	if (q->commandType != CMD_SELECT || q->limitCount == NULL || q->sortClause == NIL || list_length(q->rtable) != 1 || q->jointree == NULL ||
		list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef) || q->setOperations != NULL || q->hasAggs || q->hasWindowFuncs ||
		q->hasTargetSRFs || q->hasSubLinks || q->hasForUpdate || q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->distinctClause != NIL ||
		q->cteList != NIL || q->rowMarks != NIL)
		return false;
	/* LIMIT and OFFSET are int8 casts of constants until the planner folds them */
	limit = eval_const_expressions(NULL, q->limitCount);
	skip = q->limitOffset != NULL ? eval_const_expressions(NULL, q->limitOffset) : NULL;
	if (!IsA(limit, Const) || ((Const *) limit)->constisnull || (skip != NULL && (!IsA(skip, Const) || ((Const *) skip)->constisnull)))
		return false;
	parent = linitial_node(RangeTblEntry, q->rtable);
	/* the query runs for each row of the one around it: a condition reads a column of it */
	if (parent->rtekind != RTE_RELATION || parent->relkind != RELKIND_PARTITIONED_TABLE || parent->securityQuals != NIL || parent->tablesample != NULL ||
		!contain_vars_of_level(q->jointree->quals, 1) || contain_volatile_functions((Node *) q) || !has_column_source(parent->relid))
		return false;
	count = DatumGetInt64(((Const *) limit)->constvalue);
	if (skip != NULL)
		offset = DatumGetInt64(((Const *) skip)->constvalue);
	if (count < 0 || offset < 0 || count > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) || offset > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS))
		return false;
	/* the arms sort by columns of the table, which they hold */
	foreach_node(SortGroupClause, s, q->sortClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(s, q->targetList);

		if (!IsA(e, Var) || ((Var *) e)->varno != 1 || ((Var *) e)->varlevelsup != 0)
			return false;
	}
	arm_cols(q, &cols);
	if (cols.attnos == NIL || linitial_int(cols.attnos) <= 0)
		return false;
	leaves = leaf_oids(parent->relid, &cols, &ordered, &asc, q, &pin);
	if (list_length(leaves) < 2 || list_length(leaves) > sift_limit(partitions, SIFT_AGG_MAX_PARTS))
		return false;
	if (!apply)
		return true;

	u = makeNode(Query);
	u->commandType = CMD_SELECT;
	u->canSetTag = true;
	u->jointree = makeFromExpr(NIL, NULL);
	foreach_int(a, cols.attnos)
		names = lappend(names, makeString(get_attname(parent->relid, a, false)));
	foreach_oid(leaf, leaves)
	{
		RangeTblEntry *ar = makeNode(RangeTblEntry);
		RangeTblRef *rtr = makeNode(RangeTblRef);

		ar->rtekind = RTE_SUBQUERY;
		ar->subquery = arm_query(q, parent, leaf, &cols, count + offset, pin == NULL ? 2 : 3);
		if (pin != NULL)
			ar->subquery = gated_arm(ar->subquery, parent->relid, leaf, pin, &cols, names);
		ar->eref = makeAlias(psprintf("*SELECT* %d", foreach_current_index(leaf) + 1), names);
		u->rtable = lappend(u->rtable, ar);
		rtr->rtindex = list_length(u->rtable);
		if (tree == NULL)
			tree = (Node *) rtr;
		else
		{
			SetOperationStmt *so = makeNode(SetOperationStmt);

			so->op = SETOP_UNION;
			so->all = true;
			so->larg = tree;
			so->rarg = (Node *) rtr;
			foreach_int(a, cols.attnos)
			{
				Oid			t,
							c;
				int32		m;

				get_atttypetypmodcoll(parent->relid, a, &t, &m, &c);
				so->colTypes = lappend_oid(so->colTypes, t);
				so->colTypmods = lappend_int(so->colTypmods, m);
				so->colCollations = lappend_oid(so->colCollations, c);
			}
			tree = (Node *) so;
		}
	}
	foreach_int(a, cols.attnos)
	{
		Oid			t,
					c;
		int32		m;

		get_atttypetypmodcoll(parent->relid, a, &t, &m, &c);
		u->targetList = lappend(u->targetList, makeTargetEntry((Expr *) makeVar(1, foreach_current_index(a) + 1, t, m, c, 0), foreach_current_index(a) + 1, get_attname(parent->relid, a, false), false));
	}
	u->setOperations = tree;

	ur = makeNode(RangeTblEntry);
	ur->rtekind = RTE_SUBQUERY;
	ur->subquery = u;
	ur->inFromCl = true;
	ur->eref = makeAlias("arms", names);
	q->targetList = (List *) remap_mutator((Node *) q->targetList, &cols);
	/* the table stays in the range table for its privileges; the query reads the union */
	q->rtable = lappend(q->rtable, ur);
	((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex = 2;
	q->jointree->quals = NULL;
	if (ordered)
	{
		/* the partitions come in the order of the sort, each sorted: no sort is left to do */
		q->sortClause = NIL;
		foreach_node(TargetEntry, te, q->targetList)
			te->ressortgroupref = 0;
	}
	sift_agg_by_cost = true;
	return true;
}
