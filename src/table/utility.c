/*
 * table/utility.c - the statements on a sift_table that its format has to see around the server's own work (ALTER TABLE, SET SCHEMA and RENAME COLUMN): the hook finds the table of
 * the format and calls the callbacks it has (table/dispatch.h); a format without them (Format 1 follows through its event trigger) is left alone.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/rel.h"

#include "table/dispatch.h"

static ProcessUtility_hook_type prev_utility;

/* The table the statement is on, locked as the statement will lock it, InvalidOid when it names no sift_table */
static Oid
statement_table(Node *parse, LOCKMODE *lock)
{
	RangeVar   *rv = NULL;
	Oid			relid;
	Relation	rel;
	bool		ours;

	*lock = AccessExclusiveLock;
	if (IsA(parse, AlterTableStmt) && castNode(AlterTableStmt, parse)->objtype == OBJECT_TABLE)
	{
		rv = castNode(AlterTableStmt, parse)->relation;
		*lock = AlterTableGetLockLevel(castNode(AlterTableStmt, parse)->cmds);
	}
	else if (IsA(parse, AlterObjectSchemaStmt) && castNode(AlterObjectSchemaStmt, parse)->objectType == OBJECT_TABLE)
		rv = castNode(AlterObjectSchemaStmt, parse)->relation;
	else if (IsA(parse, RenameStmt) && castNode(RenameStmt, parse)->renameType == OBJECT_COLUMN && castNode(RenameStmt, parse)->relationType == OBJECT_TABLE)
		rv = castNode(RenameStmt, parse)->relation;
	if (rv == NULL || !OidIsValid(relid = RangeVarGetRelid(rv, *lock, true)))
		return InvalidOid;
	rel = relation_open(relid, NoLock);
	ours = sift_tbl_is_table(rel);
	relation_close(rel, NoLock);
	return ours ? relid : InvalidOid;
}

static void
tbl_utility(PlannedStmt *pstmt, const char *query, bool readonly, ProcessUtilityContext context, ParamListInfo params, QueryEnvironment *env, DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parse = pstmt->utilityStmt;
	LOCKMODE	lock;
	Oid			relid = statement_table(parse, &lock);
	const SiftTableFormatOps *ops = NULL;
	Relation	rel;

	if (OidIsValid(relid))
	{
		rel = relation_open(relid, NoLock);
		ops = sift_tbl_ops(rel);
		if (ops->ddl_before != NULL && IsA(parse, AlterTableStmt))
			ops->ddl_before(rel, castNode(AlterTableStmt, parse));
		relation_close(rel, NoLock);
	}
	if (prev_utility != NULL)
		prev_utility(pstmt, query, readonly, context, params, env, dest, qc);
	else
		standard_ProcessUtility(pstmt, query, readonly, context, params, env, dest, qc);
	if (ops == NULL)
		return;
	rel = relation_open(relid, NoLock);
	if (ops->ddl_after != NULL)
		ops->ddl_after(rel, parse);
	if (ops->ddl_rename_column != NULL && IsA(parse, RenameStmt))
		ops->ddl_rename_column(rel, castNode(RenameStmt, parse)->subname, castNode(RenameStmt, parse)->newname);
	relation_close(rel, NoLock);
}

void
sift_tbl_utility_init(void)
{
	prev_utility = ProcessUtility_hook;
	ProcessUtility_hook = tbl_utility;
}
