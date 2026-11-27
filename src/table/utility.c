/*
 * table/utility.c - the statements on a sift_table that its format has to see around the server's own work (ALTER TABLE, SET SCHEMA and RENAME COLUMN): the hook finds the table of
 * the format and calls the callbacks it has (table/dispatch.h); a format without them (Format 1 follows through its event trigger) is left alone. A sift_table that becomes a partition
 * (ATTACH PARTITION, CREATE TABLE ... PARTITION OF) is checked against the publications of the table it joins, DISCARD ALL empties the caches of the extension, and the rows of an index
 * that REINDEX CONCURRENTLY builds again follow it to its new OID.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "executor/spi.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "core/cachebudget.h"
#include "col/f2/compact/par.h"
#include "core/catalog.h"
#include "core/owner_sql.h"
#include "table/dispatch.h"
#include "table/wal.h"

static ProcessUtility_hook_type prev_utility;

/* A statement that copies tables into new heaps runs now: ALTER TABLE, VACUUM FULL, CLUSTER */
static bool rewriting = false;

static bool
rewrites_tables(Node *parse)
{
#if PG_VERSION_NUM >= 190000
	return IsA(parse, AlterTableStmt) || IsA(parse, VacuumStmt) || IsA(parse, RepackStmt);
#else
	return IsA(parse, AlterTableStmt) || IsA(parse, VacuumStmt) || IsA(parse, ClusterStmt);
#endif
}

/* The new heap of a rewrite is named after the table it replaces (make_new_heap), in its schema: nothing else of the catalog ties the two while the rows are copied */
Oid
sift_tbl_rewrite_source(Relation rel)
{
	const char *name = RelationGetRelationName(rel);
	char		heap[NAMEDATALEN];
	Oid			relid;

	if (!rewriting || sscanf(name, "pg_temp_%u", &relid) != 1)
		return InvalidOid;
	snprintf(heap, sizeof(heap), "pg_temp_%u", relid);
	if (strcmp(name, heap) != 0 || get_rel_namespace(relid) != RelationGetNamespace(rel) || !sift_relid_is_sift_table(relid))
		return InvalidOid;
	return relid;
}

/* The table the statement names, NULL when it is not a statement on a table the format has to see */
static RangeVar *
statement_relation(Node *parse, LOCKMODE *lock)
{
	*lock = AccessExclusiveLock;
	if (IsA(parse, AlterTableStmt) && castNode(AlterTableStmt, parse)->objtype == OBJECT_TABLE)
	{
		*lock = AlterTableGetLockLevel(castNode(AlterTableStmt, parse)->cmds);
		return castNode(AlterTableStmt, parse)->relation;
	}
	if (IsA(parse, AlterObjectSchemaStmt) && castNode(AlterObjectSchemaStmt, parse)->objectType == OBJECT_TABLE)
		return castNode(AlterObjectSchemaStmt, parse)->relation;
	if (IsA(parse, RenameStmt) && castNode(RenameStmt, parse)->renameType == OBJECT_COLUMN && castNode(RenameStmt, parse)->relationType == OBJECT_TABLE)
		return castNode(RenameStmt, parse)->relation;
	return NULL;
}

/* A subcommand the server runs on the tables that inherit from the one named too (tablecmds.c recurses with it), of those the format has to see */
static bool
cmd_recurses(const AlterTableCmd *cmd)
{
	return cmd->subtype == AT_AddColumn || cmd->subtype == AT_DropColumn || cmd->subtype == AT_AlterColumnType || cmd->subtype == AT_DropNotNull || cmd->subtype == AT_DropConstraint;
}

/* The statement as the server runs it on an inheritor (a partition, a child of INHERITS): its subcommands that recurse; NULL when none does */
static Node *
inherited_statement(Node *parse)
{
	AlterTableStmt *stmt;

	if (IsA(parse, RenameStmt))
		return parse;
	if (!IsA(parse, AlterTableStmt))
		return NULL;
	stmt = makeNode(AlterTableStmt);
	*stmt = *castNode(AlterTableStmt, parse);
	stmt->cmds = NIL;
	foreach_node(AlterTableCmd, cmd, castNode(AlterTableStmt, parse)->cmds)
		if (cmd_recurses(cmd))
			stmt->cmds = lappend(stmt->cmds, cmd);
	return stmt->cmds != NIL ? (Node *) stmt : NULL;
}

static bool
any_sift_table(List *relids)
{
	foreach_oid(relid, relids)
		if (sift_relid_is_sift_table(relid))
			return true;
	return false;
}

/* Of the locked tables, those of a format the hook serves */
static List *
ours_of(List *relids)
{
	List	   *ours = NIL;

	foreach_oid(relid, relids)
	{
		Relation	rel;

		if (!sift_relid_is_sift_table(relid))
			continue;
		rel = relation_open(relid, NoLock);
		if (sift_tbl_is_table(rel))
			ours = lappend_oid(ours, relid);
		relation_close(rel, NoLock);
	}
	return ours;
}

/* The table the statement names, locked once its owner is the role that runs it: a role without the right fails before it waits for the lock */
static Oid
lock_owned(RangeVar *rv, LOCKMODE lock)
{
	return RangeVarGetRelidExtended(rv, lock, RVR_MISSING_OK, RangeVarCallbackOwnsRelation, NULL);
}

/*
 * The sift_tables the statement changes, locked as the statement will lock them: the one it names and, unless it says ONLY, those that inherit from it. A table of another access method is not
 * locked before the server checks the right to the statement, but for one whose inheritors are sift_tables: the statement locks the inheritors after it, and they are locked in that order.
 */
static List *
statement_tables(Node *parse, LOCKMODE *lock, Oid *named)
{
	RangeVar   *rv = statement_relation(parse, lock);
	Oid			relid = *named = rv != NULL ? RangeVarGetRelid(rv, NoLock, true) : InvalidOid;

	if (!OidIsValid(relid))
		return NIL;
	if (rv->inh && inherited_statement(parse) != NULL && has_subclass(relid) && any_sift_table(find_all_inheritors(relid, NoLock, NULL)))
	{
		relid = *named = lock_owned(rv, *lock);
		return OidIsValid(relid) ? ours_of(find_all_inheritors(relid, *lock, NULL)) : NIL;
	}
	if (!sift_relid_is_sift_table(relid) || !OidIsValid(relid = *named = lock_owned(rv, *lock)))
		return NIL;
	return ours_of(list_make1_oid(relid));
}

/* A sift_table that is a partition of a published table is published, which the catalog says only once the statement has made it a partition */
static void
check_partition_publication(RangeVar *child)
{
	Oid			relid = RangeVarGetRelid(child, NoLock, true);
	Relation	rel;

	if (!OidIsValid(relid))
		return;
	rel = relation_open(relid, NoLock);
	if (sift_tbl_is_table(rel))
		sift_tbl_check_publication(rel);
	relation_close(rel, NoLock);
}

static void
check_new_partitions(Node *parse)
{
	ListCell   *lc;

	if (IsA(parse, CreateStmt) && castNode(CreateStmt, parse)->partbound != NULL)
		check_partition_publication(castNode(CreateStmt, parse)->relation);
	else if (IsA(parse, AlterTableStmt))
		foreach(lc, castNode(AlterTableStmt, parse)->cmds)
		{
			AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

			if (cmd->subtype == AT_AttachPartition)
				check_partition_publication(castNode(PartitionCmd, cmd->def)->name);
		}
}

/* The participants VACUUM (PARALLEL n) asks the compaction of its indexes and tables for (par.h); the options of every other statement ask none */
static int
vacuum_parallel(Node *parse)
{
	if (IsA(parse, VacuumStmt))
		foreach_ptr(DefElem, opt, castNode(VacuumStmt, parse)->options)
			if (strcmp(opt->defname, "parallel") == 0)
				return defGetInt32(opt);
	return 0;
}

/* An index that a catalog of the extension names by OID (sift.projection_defs, sift.tier_policies), as it was before REINDEX CONCURRENTLY built it again */
typedef struct Rebuilt
{
	Oid			relid;
	Oid			table;
	Oid			nsp;
	NameData	name;
} Rebuilt;

typedef struct RebuiltRead
{
	MemoryContext cx;
	List	   *out;
} RebuiltRead;

/* The two catalogs read with the policies: a database without the extension, or with the SQL of a version before them, has none */
static Oid
rebind_catalog(void)
{
	Oid			nsp = get_namespace_oid("sift", true);

	return OidIsValid(nsp) ? get_relname_relid("tier_policies", nsp) : InvalidOid;
}

static bool
reindex_concurrently(Node *parse)
{
	if (IsA(parse, ReindexStmt))
		foreach_ptr(DefElem, opt, castNode(ReindexStmt, parse)->params)
			if (strcmp(opt->defname, "concurrently") == 0)
				return defGetBoolean(opt);
	return false;
}

static void
read_rebuilt(void *arg)
{
	RebuiltRead *r = arg;
	MemoryContext old = MemoryContextSwitchTo(r->cx);

	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		Oid			relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
		HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		Rebuilt    *b;

		if (!HeapTupleIsValid(tp))
			continue;
		if (((Form_pg_class) GETSTRUCT(tp))->relkind == RELKIND_INDEX)
		{
			b = palloc(sizeof(Rebuilt));
			*b = (Rebuilt) {relid, IndexGetRelation(relid, false), ((Form_pg_class) GETSTRUCT(tp))->relnamespace, ((Form_pg_class) GETSTRUCT(tp))->relname};
			r->out = lappend(r->out, b);
		}
		ReleaseSysCache(tp);
	}
	MemoryContextSwitchTo(old);
}

/*
 * The indexes the catalogs of the extension name, read before REINDEX CONCURRENTLY: it builds each index it rebuilds under a new OID, gives it the name of the old one and drops that one without
 * the event triggers of a DROP, so only the name and the table tie the two
 */
static List *
named_indexes(Node *parse)
{
	Oid			catalog = reindex_concurrently(parse) ? rebind_catalog() : InvalidOid;
	RebuiltRead r = {CurrentMemoryContext, NIL};

	if (OidIsValid(catalog))
		sift_owner_sql(catalog, &(SiftOwnerSql) {.sql = "SELECT rel::pg_catalog.oid FROM sift.projection_defs UNION SELECT relid::pg_catalog.oid FROM sift.tier_policies",
					   .read_only = true,.expect = SPI_OK_SELECT,.rows = read_rebuilt,.out = &r});
	return r.out;
}

/* The index REINDEX CONCURRENTLY built in place of b: once b is gone, the index of its table that has its name */
static Oid
rebuilt_as(const Rebuilt *b)
{
	Oid			relid = SearchSysCacheExists1(RELOID, ObjectIdGetDatum(b->relid)) ? InvalidOid : get_relname_relid(NameStr(b->name), b->nsp);

	return OidIsValid(relid) && get_rel_relkind(relid) == RELKIND_INDEX && IndexGetRelation(relid, false) == b->table ? relid : InvalidOid;
}

static const char *const rebind_sql[] = {
	"UPDATE sift.projection_defs SET rel = $2 WHERE rel::pg_catalog.oid = $1", "UPDATE sift.tier_policies SET relid = $2 WHERE relid::pg_catalog.oid = $1",
};

/*
 * The rows of each rebuilt index name the new one: its projections are built again from their definitions, and its policy keeps its scope, so the sweep of the new index finds the objects of the
 * old one (another relfilenumber) unnamed and deletes them as it deletes those of a REINDEX
 */
static void
rebind_rebuilt(List *named)
{
	foreach_ptr(Rebuilt, b, named)
	{
		Oid			to = rebuilt_as(b);
		Oid			types[] = {OIDOID, OIDOID};
		Datum		values[] = {ObjectIdGetDatum(b->relid), ObjectIdGetDatum(to)};

		if (OidIsValid(to))
			for (int i = 0; i < (int) lengthof(rebind_sql); i++)
				sift_owner_sql(rebind_catalog(), &(SiftOwnerSql) {.sql = rebind_sql[i],.nargs = 2,.types = types,.values = values,.expect = SPI_OK_UPDATE});
	}
}

/* The format of each table as the statement finds it: a table the statement takes off sift_table is still told to the callbacks of the format it left */
static List *
formats_of(List *relids)
{
	List	   *formats = NIL;

	foreach_oid(relid, relids)
	{
		Relation	rel = relation_open(relid, NoLock);

		formats = lappend(formats, unconstify(SiftTableFormatOps *, sift_tbl_ops(rel)));
		relation_close(rel, NoLock);
	}
	return formats;
}

/* Calls the callback of the format on each table, the statement as it runs on that table */
static void
each_table(List *relids, List *formats, Oid named, Node *parse, void (*call) (Relation rel, const SiftTableFormatOps *ops, Node *stmt))
{
	Node	   *inherited = relids != NIL ? inherited_statement(parse) : NULL;
	ListCell   *lr,
			   *lf;

	forboth(lr, relids, lf, formats)
	{
		Relation	rel = relation_open(lfirst_oid(lr), NoLock);

		call(rel, lfirst(lf), lfirst_oid(lr) == named ? parse : inherited);
		relation_close(rel, NoLock);
	}
}

static void
call_before(Relation rel, const SiftTableFormatOps *ops, Node *stmt)
{
	if (ops->ddl_before != NULL && IsA(stmt, AlterTableStmt))
		ops->ddl_before(rel, castNode(AlterTableStmt, stmt));
}

/* A table that is still a sift_table may have been rewritten into another format; one that left it has the format it was in */
static void
call_after(Relation rel, const SiftTableFormatOps *before, Node *stmt)
{
	const SiftTableFormatOps *ops = sift_tbl_is_table(rel) ? sift_tbl_ops(rel) : before;

	if (ops->ddl_after != NULL)
		ops->ddl_after(rel, stmt);
	if (ops->ddl_rename_column != NULL && IsA(stmt, RenameStmt))
		ops->ddl_rename_column(rel, castNode(RenameStmt, stmt)->subname, castNode(RenameStmt, stmt)->newname);
}

static void
tbl_utility(PlannedStmt *pstmt, const char *query, bool readonly, ProcessUtilityContext context, ParamListInfo params, QueryEnvironment *env, DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parse = pstmt->utilityStmt;
	LOCKMODE	lock;
	Oid			named;
	List	   *relids = statement_tables(parse, &lock, &named);
	int			workers = sift_vacuum_parallel_workers;
	bool		outer = rewriting;
	List	   *formats = formats_of(relids);
	List	   *rebuilt = named_indexes(parse);

	each_table(relids, formats, named, parse, call_before);
	PG_TRY();
	{
		sift_vacuum_parallel_workers = vacuum_parallel(parse);
		rewriting = rewriting || rewrites_tables(parse);
		if (prev_utility != NULL)
			prev_utility(pstmt, query, readonly, context, params, env, dest, qc);
		else
			standard_ProcessUtility(pstmt, query, readonly, context, params, env, dest, qc);
	}
	PG_FINALLY();
	{
		sift_vacuum_parallel_workers = workers;
		rewriting = outer;
	}
	PG_END_TRY();
	check_new_partitions(parse);
	rebind_rebuilt(rebuilt);
	if (IsA(parse, DiscardStmt) && castNode(DiscardStmt, parse)->target == DISCARD_ALL)
		sift_cache_discard_all();
	each_table(relids, formats, named, parse, call_after);
}

void
sift_tbl_utility_init(void)
{
	prev_utility = ProcessUtility_hook;
	ProcessUtility_hook = tbl_utility;
}
