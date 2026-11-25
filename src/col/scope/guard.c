/*
 * col/scope/guard.c - the journals of a scope (which index is one) and what a scope declaration needs of the relations and the journals it names, said when the index is made or altered (col/scope/decl.c asks): the journal that is
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
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope/scope.h"
#include "core/catname.h"

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
 * The journal indexes of the scope relation: ready sift_col indexes of format major 1 (the key log is that format's) with track_changes = 'ctid' whose first column is the member and that hold the scope key; the lowest oid of those that hear of
 * every pair. A journal records the pairs a statement ends and inserts, so a partial one qualifies only when the predicate of the btree that defines the pairs implies its predicate (every pair
 * row is in it): else it never hears of a pair inserted outside its predicate and a read would take the member for unchanged.
 * The restore of a binary upgrade makes the dumped journal an empty index of the new default format, whose file the old one replaces when the upgrade ends: its catalog entry says what it is.
 */
static Oid
journal_scan(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	List	   *pair_pred = sift_scope_index_predicate(pair_btree);

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		bool		ok = sift_col_is_index(idx) && (IsBinaryUpgrade || sift_col_format_major(idx) == SIFT_FORMAT_MAJOR) && sift_index_ready(idx) && (idx->rd_index->indisvalid || sift_scope_journal_pending(ioid)) && sift_col_track_ctid(idx) &&
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
						errhint("The journal keeps track_changes = 'ctid': a read of the scope index learns from it which pairs changed. DROP INDEX %s first to give the scope up.", quote_identifier(sift_rel_name(user)))));
}
