/*
 * col/scope/make.c - the journal index that CREATE INDEX of a scope index makes when its relation of pairs has none (docs/COL-SCOPE.md "The journal index")
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
#include "access/xact.h"
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
#include "utils/memutils.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#ifdef SIFT_TESTING
#include "bm25/build/build.h"
#endif
#include "core/dispatch.h"
#include "core/meta_peek.h"
#include "col/am.h"
#include "col/v1/build.h"
#include "col/scope/scope.h"
#include "core/restrict.h"
#include "core/ring.h"

#define JOURNAL_LABEL	"sift_journal"
#define JOURNAL_COMMENT	"Journal of the pairs of \"%s\" (%s, %s), made by CREATE INDEX of a scope index and managed by sift: it records the pairs that change, and goes with the last scope index that uses it."
#define JOURNAL_MARK	"Journal of the pairs of "

bool		sift_col_journal_empty = false;
bool		sift_col_journal_validating = false;

/*
 * The journals this backend made empty in the build that is running: ready for writes, not valid, filled by the validation pass of that build. Only the backend that made them reads them as journals
 * (journal_scan), and an abort forgets them: what is left is an invalid index that DROP INDEX removes and no scope index takes for a journal.
 */
static List *pending = NIL;
static bool pending_hooked = false;

bool
sift_scope_journal_pending(Oid index)
{
	return list_member_oid(pending, index);
}

#define MANAGED_MEMO_N 8

static struct ManagedMemo
{
	Oid			index;
	uint64		gen;
	bool		managed;
} managed_memo[MANAGED_MEMO_N];
static SiftRing managed_memo_ring;

/* Is the index a journal that CREATE INDEX of a scope index made (its comment says so): a key log, no column source of an aggregate. Read from the catalog once for each invalidation */
bool
sift_scope_journal_managed(Oid index)
{
	const uint64 gen = SharedInvalidMessageCounter;
	char	   *comment;
	struct ManagedMemo *m;

	for (int i = 0; i < MANAGED_MEMO_N; i++)
		if (managed_memo[i].index == index && managed_memo[i].gen == gen)
			return managed_memo[i].managed;
	comment = GetComment(index, RelationRelationId, 0);
	m = &managed_memo[SIFT_RING_SLOT(managed_memo_ring, MANAGED_MEMO_N)];
	*m = (struct ManagedMemo) {index, gen, comment != NULL && strncmp(comment, JOURNAL_MARK, strlen(JOURNAL_MARK)) == 0};
	return m->managed;
}

static void
pending_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT)
		pending = NIL;
}

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
sift_scope_journal_make(const SiftScopeSpec *s, Relation rel, int hop, AttrNumber member, AttrNumber key, Oid btree, SiftJournalMake make)
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
	sift_col_drain_writers(relid, ShareRowExclusiveLock);
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
	if (make == SIFT_JOURNAL_EMPTY)
		ereport(NOTICE,
				(errmsg("scope \"%s\": making the journal index \"%s\" of \"%s\" (%s, %s) in the validation of this statement", s->key_name, name, RelationGetRelationName(rel), mname, kname),
				 errdetail("The writers of \"%s\" wait only until this transaction ends; the index is filled while they go on and is valid when the statement ends. The index goes with the last scope index that uses it.", RelationGetRelationName(rel)),
				 errhint("To name it, place it or keep it, make it first: %s.", journal_statement(rel, mname, kname, true))));
	else
		ereport(NOTICE,
				(errmsg("scope \"%s\": making the journal index \"%s\" of \"%s\" (%s, %s)", s->key_name, name, RelationGetRelationName(rel), mname, kname),
				 errdetail("The writers of \"%s\" wait until the build of this index ends, as for CREATE INDEX on it, for as long as it takes to read the relation. The index goes with the last scope index that uses it.", RelationGetRelationName(rel)),
				 errhint("To build it without blocking the writers, name it, place it or keep it, make it first: %s.", journal_statement(rel, mname, kname, true))));
	sift_col_journal_empty = make == SIFT_JOURNAL_EMPTY;
	sift_new_format_journal = true;
	PG_TRY();
	{
		addr = sift_create_index(relid, stmt);
	}
	PG_FINALLY();
	{
		sift_col_journal_empty = false;
		sift_new_format_journal = false;
	}
	PG_END_TRY();
	CommandCounterIncrement();
	if (make == SIFT_JOURNAL_EMPTY)
	{
		sift_index_set_state(addr.objectId, INDEX_DROP_CLEAR_VALID);
		CommandCounterIncrement();
		if (!pending_hooked)
			sift_xact_callback(pending_xact, NULL);
		pending_hooked = true;
		{
			MemoryContext old = MemoryContextSwitchTo(PortalContext);

			pending = list_append_unique_oid(pending, addr.objectId);
			MemoryContextSwitchTo(old);
		}
	}
	sift_col_track_made(addr.objectId);
	CreateComments(addr.objectId, RelationRelationId, 0, psprintf(JOURNAL_COMMENT, RelationGetRelationName(rel), mname, kname));
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
		sift_drop_object(&obj);
	}
}

/* validate_index of a journal the statement made empty, which writes the pairs it adds and no record of them (sift_col_journal_validating) */
static void
journal_fill(Oid heap, Oid journal, Snapshot snap)
{
	sift_col_journal_validating = true;
	PG_TRY();
	{
		validate_index(heap, journal, snap);
	}
	PG_FINALLY();
	{
		sift_col_journal_validating = false;
	}
	PG_END_TRY();
}

/*
 * The validation pass of a concurrent build, for the journals it made empty (CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY of a scope index over a relation of pairs without one): the
 * phases that PostgreSQL gives a concurrent index. The journal was ready for writes when the transaction of the build committed, so the writers that began after it keep it (aminsert); the writers
 * that began before it are waited for, and then the heap is read under a snapshot that sees all they did, adding the pairs the journal does not have (validate_index). The transactions whose
 * snapshots are older than that one are waited for before the journal is valid, as for any index. No lock stronger than ShareUpdateExclusiveLock is held.
 *
 * The pairs that pass adds were written by transactions that finished before the lock that began the journal (the writers since record their own), so the pass writes the pairs into the index
 * and no journal record (sift_col_journal_validating). A stamp is not what keeps a read from needing a record of them: the wait for older snapshots does not wait for a prepared transaction
 * or an idle READ COMMITTED one, so the xmin of a stamp can stay at or below the xid of a writer of those pairs. What does is the check of sift_col_track_ok_log that the snapshot of a read sees as
 * committed the transaction that made the journal, which every writer of those pairs finished before: a read that does not see it takes the heap. That check is the only defence.
 */
void
sift_scope_journals_validate(void)
{
	foreach_oid(journal, pending)
	{
		Oid			heap = IndexGetRelation(journal, false);
		Snapshot	snap;

		LockRelationOid(heap, ShareUpdateExclusiveLock);
		sift_col_drain_writers(heap, ShareLock);
#ifdef SIFT_TESTING
		sift_test_phase_wait(SIFT_COL_GATE_JOURNAL);
#endif
		snap = RegisterSnapshot(GetLatestSnapshot());
		PushActiveSnapshot(snap);
		journal_fill(heap, journal, snap);
		PopActiveSnapshot();
		WaitForOlderSnapshots(snap->xmin, false);
		UnregisterSnapshot(snap);
		sift_index_set_state(journal, INDEX_CREATE_SET_VALID);
		CommandCounterIncrement();
	}
	list_free(pending);
	pending = NIL;
}
