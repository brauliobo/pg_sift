/*
 * col/scope_make.c - the journal index that CREATE INDEX of a scope index makes when its relation of pairs has none (docs/COL-SCOPE.md "The journal index")
 *
 * The index is the one a user would make by hand: a sift_col index of the relation of pairs, the member first, with track_changes = 'ctid'. It is named by the rule of PostgreSQL for an index
 * (<table>_<member>_<key>_sift_journal), owned by the owner of the table (the statement needs that right), and marked by its comment, which a dump keeps: an index with that comment is the
 * scope's own and goes with the last scope index that depends on it (sift_scope_journal_drop_unused); a journal made by hand, or one whose comment was changed, is the user's and stays.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_description.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_node.h"
#include "parser/parse_utilcmd.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/scope.h"

#define JOURNAL_LABEL	"sift_journal"
#define JOURNAL_COMMENT	"Journal of the pairs of \"%s\" (%s, %s), made by CREATE INDEX of a scope index and managed by sift: it records the pairs that change, and goes with the last scope index that uses it."
#define JOURNAL_MARK	"Journal of the pairs of "

static IndexElem *
journal_column(const char *name)
{
	IndexElem  *e = makeNode(IndexElem);

	e->name = pstrdup(name);
	e->ordering = SORTBY_DEFAULT;
	e->nulls_ordering = SORTBY_NULLS_DEFAULT;
	return e;
}

static const char *
journal_statement(Relation rel, const char *member, const char *key, bool concurrently)
{
	return psprintf("CREATE INDEX %sON %s USING sift_col (%s, %s) WITH (track_changes = 'ctid')", concurrently ? "CONCURRENTLY " : "", quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel)),
					quote_identifier(member), quote_identifier(key));
}

/*
 * The journal index of the pairs of rel (columns member, key; the btree defines the pairs): the one the relation has, else a new one. The relation is locked ShareRowExclusive first, as the
 * trigger of CREATE INDEX does (col/am/lock.c): the writers of the pairs wait from here to the end of the statement, and two builds that make the journal of one relation do not wait for each other
 * while each holds the ShareLock of its own CREATE INDEX (the second finds the first one's journal when it gets the lock).
 */
Oid
sift_scope_journal_make(const SiftScopeSpec *s, Relation rel, int hop, AttrNumber member, AttrNumber key, Oid btree)
{
	Oid			relid = RelationGetRelid(rel);
	const char *mname = get_attname(relid, member, false),
			   *kname = get_attname(relid, key, false);
	Oid			journal;
	char	   *name;
	IndexStmt  *stmt;
	ObjectAddress addr;

	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("scope \"%s\": \"%s\" has no sift_col index that journals its changes, and this role cannot make one: it does not own the table",
																		 s->key_name, RelationGetRelationName(rel)),
						errhint("The owner of the table makes it: %s.", journal_statement(rel, mname, kname, false))));
	LockRelationOid(relid, ShareRowExclusiveLock);
	journal = sift_scope_hop_journal(rel, hop, hop == 2 ? key : member, hop == 2 ? member : key, btree);
	if (OidIsValid(journal))
		return journal;
	name = ChooseRelationName(RelationGetRelationName(rel), psprintf("%s_%s", mname, kname), JOURNAL_LABEL, RelationGetNamespace(rel), false);
	stmt = makeNode(IndexStmt);
	stmt->idxname = name;
	stmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)), pstrdup(RelationGetRelationName(rel)), -1);
	stmt->accessMethod = "sift_col";
	stmt->indexParams = list_make2(journal_column(mname), journal_column(kname));
	stmt->options = list_make1(makeDefElem("track_changes", (Node *) makeString("ctid"), -1));
	stmt = transformIndexStmt(relid, stmt, journal_statement(rel, mname, kname, false));
	addr = sift_define_index(relid, stmt);
	CommandCounterIncrement();
	sift_col_track_made(addr.objectId);
	CreateComments(addr.objectId, RelationRelationId, 0, psprintf(JOURNAL_COMMENT, RelationGetRelationName(rel), mname, kname));
	ereport(NOTICE,
			(errmsg("scope \"%s\": made the journal index \"%s\" of \"%s\" (%s, %s)", s->key_name, name, RelationGetRelationName(rel), mname, kname),
			 errdetail("The writers of \"%s\" wait until this statement ends. The index goes with the last scope index that uses it.", RelationGetRelationName(rel)),
			 errhint("To name it, place it or keep it, make it first: %s.", journal_statement(rel, mname, kname, true))));
	return addr.objectId;
}

/*
 * The journal indexes this library made that no scope index depends on any more are dropped: the last scope index that used one was dropped (DROP INDEX, DROP TABLE, ALTER TABLE .. DROP COLUMN,
 * a DROP of the schema) or gave its scope up (ALTER INDEX .. RESET (scope)). A relation that is in use by a statement of another session keeps its journal until the next time: the drop does not
 * wait for it.
 */
void
sift_scope_journal_drop_unused(void)
{
	Relation	drel = table_open(DescriptionRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *made = NIL;

	ScanKeyInit(&key[0], Anum_pg_description_classoid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_description_objsubid, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
	scan = systable_beginscan(drel, InvalidOid, false, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_description d = (Form_pg_description) GETSTRUCT(tup);
		bool		isnull;
		Datum		descr = heap_getattr(tup, Anum_pg_description_description, RelationGetDescr(drel), &isnull);

		if (!isnull && strncmp(TextDatumGetCString(descr), JOURNAL_MARK, strlen(JOURNAL_MARK)) == 0 && get_rel_relkind(d->objoid) == RELKIND_INDEX)
			made = lappend_oid(made, d->objoid);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	foreach_oid(index, made)
	{
		Oid			heap = IndexGetRelation(index, true);
		ObjectAddress obj;

		if (!OidIsValid(heap) || sift_scope_users(index) != NIL || !ConditionalLockRelationOid(heap, AccessExclusiveLock))
			continue;
		ObjectAddressSet(obj, RelationRelationId, index);
		performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}
}
