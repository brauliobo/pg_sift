/*
 * col/scope_guard.c - what a scope declaration needs of the relations and the journals it names, said when the index is made or altered (col/scope_decl.c asks): the journal that is
 * not one, the notices for a relation that makes some reads run as written, the journal that a scope index depends on, and an index whose segments were written without the declaration it
 * has now.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

/* What the first sift_col index of the relation lacks to be the journal of its pairs, for the detail of the error; NULL when it has none */
char *
sift_scope_journal_unfit(Relation rel, AttrNumber member, AttrNumber key)
{
	char	   *detail = NULL;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		const char *lack = !sift_col_is_index(idx) ? NULL : !idx->rd_index->indisvalid || !sift_index_ready(idx) ? "is not valid (a concurrent build that failed or runs)" :
			!sift_col_track_ctid(idx) ? "has no track_changes = 'ctid'" : idx->rd_index->indkey.values[0] != member ? "does not have the member column first" :
			sift_conn_index_column(idx, key) == InvalidAttrNumber ? "does not hold the scope key column" : NULL;

		if (lack != NULL && detail == NULL)
			detail = psprintf("The sift_col index \"%s\" of \"%s\" %s.", RelationGetRelationName(idx), RelationGetRelationName(rel), lack);
		index_close(idx, AccessShareLock);
	}
	return detail;
}

/* What a relation of the declaration has that makes some reads run as written: said at CREATE INDEX and REINDEX, not at each read */
void
sift_scope_notice_relation(const char *key_name, const char *name, Relation rel)
{
	if (rel->rd_rel->relrowsecurity)
		ereport(NOTICE, (errmsg("scope \"%s\": \"%s\" has row-level security enabled", key_name, name),
						 errdetail("A statement of a role the policies apply to reads its join to the relation as written.")));
}

/* A scope of two paths is read from its units only when the journals of its three relations are there: it has no error for the missing one, a REINDEX of the index finds it */
void
sift_scope_notice_no_journal(const char *key_name, const char *name, Relation rel, AttrNumber first, AttrNumber second)
{
	ereport(NOTICE, (errmsg("scope \"%s\": \"%s\" has no sift_col index that journals its changes", key_name, name),
					 errdetail("Reads of the scope index run as written until the journal exists and REINDEX INDEX makes the scope segments under it."),
					 errhint("CREATE INDEX ON %s USING sift_col (%s, %s) WITH (track_changes = 'ctid')",
							 RelationGetRelationName(rel), get_attname(RelationGetRelid(rel), first, false), get_attname(RelationGetRelid(rel), second, false))));
}

/* The sift_col indexes with a scope that depend on the index: the ones that took it for the journal (or the btree) of a relation of their scopes */
static List *
scope_users(Oid index)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *users = NIL;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(index));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->refobjsubid == 0 && get_rel_relkind(d->objid) == RELKIND_INDEX)
		{
			Relation	user = index_open(d->objid, AccessShareLock);

			if (sift_col_is_index(user) && sift_scope_specs(user) != NULL)
				users = lappend_oid(users, d->objid);
			index_close(user, AccessShareLock);
		}
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return users;
}

/* ALTER INDEX of a sift_col index that is the journal of a scope: it keeps its log of the pairs, or the scope index reads pairs that nothing tells it changed */
void
sift_scope_journal_alter(Relation index)
{
	if (sift_col_track_ctid(index))
		return;
	foreach_oid(user, scope_users(RelationGetRelid(index)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" is the journal of a scope of index \"%s\"", RelationGetRelationName(index), get_rel_name(user)),
						errhint("The journal keeps track_changes = 'ctid': a read of the scope index learns from it which pairs changed. ALTER INDEX %s RESET (scope) first to give the scope up.", get_rel_name(user))));
}

/* After ALTER INDEX of an option that changed the leaf list of the segments (a scope declared or given up): the index is not scanned until a REINDEX writes them again */
void
sift_scope_notice_stale(Relation index)
{
	if (sift_col_rel(index)->stale)
		ereport(NOTICE, (errmsg("sift_col index \"%s\": the segments were written without this scope declaration", RelationGetRelationName(index)),
						 errhint("REINDEX INDEX %s writes them again; until then queries do not scan the index.", RelationGetRelationName(index))));
}
