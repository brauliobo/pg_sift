/*
 * col/scope_guard.c - the journals of a scope (which index is one) and what a scope declaration needs of the relations and the journals it names, said when the index is made or altered (col/scope_decl.c asks): the journal that is
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
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

/* The predicate of an index as an implicit-AND list over Vars of varno 1 (a copy), NIL for a plain one */
List *
sift_scope_index_predicate(Oid indexoid)
{
	Relation	idx = index_open(indexoid, AccessShareLock);
	List	   *pred = list_copy(RelationGetIndexPredicate(idx));

	index_close(idx, AccessShareLock);
	return pred;
}

/*
 * The journal indexes of the scope relation: ready sift_col indexes with track_changes = 'ctid' whose first column is the member and that hold the scope key; the lowest oid of those that hear of
 * every pair. A journal records the pairs a statement ends and inserts, so a partial one qualifies only when the predicate of the btree that defines the pairs implies its predicate (every pair
 * row is in it): else it never hears of a pair inserted outside its predicate and a read would take the member for unchanged.
 */
static Oid
journal_scan(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	List	   *pair_pred = sift_scope_index_predicate(pair_btree);

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		bool		ok = sift_col_is_index(idx) && sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_track_ctid(idx) &&
			idx->rd_index->indkey.values[0] == member && sift_conn_index_column(idx, scope_key) != InvalidAttrNumber;

		if (ok)
		{
			List	   *jpred = RelationGetIndexPredicate(idx);

			ok = jpred == NIL || predicate_implied_by(jpred, pair_pred, false);
		}
		index_close(idx, AccessShareLock);
		if (ok)
			return ioid;
	}
	return InvalidOid;
}

Oid
sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	return journal_scan(rel, member, scope_key, pair_btree);
}

/* The journal index whose leaves list exactly the pairs (its predicate and the one of the btree imply each other): a plain journal of a partial btree holds the soft deleted rows too */
Oid
sift_scope_pairs_journal(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	Oid			journal = sift_scope_journal_index(rel, member, scope_key, pair_btree);

	return OidIsValid(journal) && predicate_implied_by(sift_scope_index_predicate(pair_btree), sift_scope_index_predicate(journal), false) ? journal : InvalidOid;
}

Oid
sift_scope_hop_journal(Relation rel, int hop, AttrNumber a, AttrNumber b, Oid btree)
{
	return hop == 2 ? journal_scan(rel, b, a, btree) : journal_scan(rel, a, b, btree);
}

/* What a relation of the declaration has that makes some reads run as written: said at CREATE INDEX and REINDEX, not at each read */
void
sift_scope_notice_relation(const char *key_name, const char *name, Relation rel)
{
	if (rel->rd_rel->relrowsecurity)
		ereport(NOTICE, (errmsg("scope \"%s\": \"%s\" has row-level security enabled", key_name, name),
						 errdetail("A statement of a role the policies apply to reads its join to the relation as written.")));
}

/* The sift_col indexes with a scope that depend on the index: the ones that took it for the journal (or the btree) of a relation of their scopes */
List *
sift_scope_users(Oid index)
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
	foreach_oid(user, sift_scope_users(RelationGetRelid(index)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" is the journal of a scope of index \"%s\"", RelationGetRelationName(index), get_rel_name(user)),
						errhint("The journal keeps track_changes = 'ctid': a read of the scope index learns from it which pairs changed. ALTER INDEX %s RESET (scope) first to give the scope up.", get_rel_name(user))));
}

/* Has the index a segment: the rows of its table are in it, which a REINDEX would write again */
static bool
has_segments(Relation index)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	bool		found = false;

	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs && !found; i++)
		found = dir->segs[i].kind == SIFT_SEG_IMMUTABLE;
	pfree(dir);
	return found;
}

/*
 * After ALTER INDEX of an option that changed the leaf list of the segments (a scope declared or given up, paths, connect ..): the index is not scanned until a REINDEX writes them again. An
 * index that has segments is not left so by an ALTER INDEX that declares a scope (scope_set): it is refused, and the index goes on serving while its replacement is built beside it (the build is
 * a CREATE INDEX CONCURRENTLY, which writes the stamp of its scope as a REINDEX does). RESET (scope), the way to give a scope up, and an index with no segment, whose REINDEX writes nothing, say what the REINDEX does.
 */
void
sift_scope_notice_stale(Relation index, bool scope_set)
{
	const char *name = RelationGetRelationName(index);

	if (!sift_col_rel(index)->stale)
		return;
	if (scope_set && sift_scope_specs(index) != NULL && has_segments(index))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col index \"%s\": its segments were written with other options than this ALTER INDEX gives it", name),
						errdetail("Every scan of the index would stop until a REINDEX writes them again."),
						errhint("CREATE INDEX CONCURRENTLY a new index with the options, then DROP INDEX %s: no scan is lost while it is built.", name)));
	ereport(NOTICE, (errmsg("sift_col index \"%s\": its segments were written with other options than it has now", name),
					 errhint("REINDEX INDEX %s writes them again; until then queries do not scan the index.", name)));
}
