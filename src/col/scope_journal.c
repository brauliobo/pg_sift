/*
 * col/scope_journal.c - the journal of the scope relation of a scope index (col/scope.h, docs/COL-CONNECT.md "The rule")
 *
 * The scope segments hold the pairs (member, scope key) of the scope relation as of the stamp they were written under. What changed in the relation since is in its journal: a sift_col
 * index of the relation with track_changes = 'ctid' whose first column is the member key keeps, in its tombstone log, a record of kind KEYCHG for the member of every pair an UPDATE, DELETE
 * or key change ended and one of kind KEYADD for the member of every pair inserted (col/conn.c). The rule, the stamp, the verdicts and the set of changed keys are those of the connections of
 * 4.3; this file only finds the journal of a scope and applies them to it.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/scope.h"

/* The scope relation of the scope and the index that journals it, both open and locked AccessShare; false: the scope is not declared now, or its relation has no journal index */
static bool
journal_open(Relation index, int scope, LOCKMODE lock, Relation *rel, Relation *jidx)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	Relation	heap;
	AttrNumber	member,
				key;
	Oid			joid;

	if (specs == NULL || scope < 0 || scope >= specs->n)
		return false;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	(void) sift_scope_relation_index(heap, index, specs->s[scope], rel, &member, &key);
	table_close(heap, NoLock);
	joid = sift_scope_journal_index(*rel, member, key);
	if (!OidIsValid(joid))
	{
		table_close(*rel, NoLock);
		return false;
	}
	*jidx = index_open(joid, lock);
	return true;
}

static void
journal_close(Relation rel, Relation jidx)
{
	index_close(jidx, NoLock);
	table_close(rel, NoLock);
}

bool
sift_scope_has_journal(Relation index, int scope)
{
	Relation	rel,
				jidx;

	if (!journal_open(index, scope, AccessShareLock, &rel, &jidx))
		return false;
	journal_close(rel, jidx);
	return true;
}

SiftConnWhy
sift_scope_take(Relation index, int scope, Snapshot snap, SiftConnStamp *stamp)
{
	Relation	rel,
				jidx;
	SiftConnWhy why;

	*stamp = (SiftConnStamp) {0};
	if (!journal_open(index, scope, AccessShareLock, &rel, &jidx))
		return SIFT_CONN_NO_JOURNAL;
	why = sift_conn_take(rel, jidx, snap, stamp, NULL);
	journal_close(rel, jidx);
	return why;
}

SiftConnWhy
sift_scope_vouch(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap)
{
	Relation	rel,
				jidx;
	SiftConnWhy why;

	if (!journal_open(index, scope, AccessShareLock, &rel, &jidx))
		return SIFT_CONN_NO_JOURNAL;
	why = sift_conn_vouches(rel, jidx, stamp, snap);
	journal_close(rel, jidx);
	return why;
}

SiftConnWhy
sift_scope_changed(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	Relation	rel,
				jidx;
	SiftConnWhy why;

	*out = (SiftConnKeys) {0};
	if (!journal_open(index, scope, AccessShareLock, &rel, &jidx))
		return SIFT_CONN_NO_JOURNAL;
	why = sift_conn_changed_by(rel, jidx, stamp, snap, cap, true, out);
	journal_close(rel, jidx);
	return why;
}

/*
 * The journals of the scopes of the index, made when the build has read the table, as those of the parents of a connected child are (sift_conn_start): the writers of the scope relation
 * are held out from here to the end of the statement, and an index that has one already is not locked at all.
 */
void
sift_scope_start(Relation heap, Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	List	   *seen = NIL;

	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		Relation	rel,
					jidx;

		if (!journal_open(index, i, ShareUpdateExclusiveLock, &rel, &jidx))
			continue;
		if (!list_member_oid(seen, RelationGetRelid(jidx)))
			(void) sift_conn_enable(rel, jidx);
		seen = lappend_oid(seen, RelationGetRelid(jidx));
		journal_close(rel, jidx);
	}
	list_free(seen);
}

/*
 * Refresh: is a scope segment worth a rewrite for what its stamps say? The rule of sift_conn_stale for each scope whose journal vouches now: never filled (lineage 0), filled under a journal
 * the relation has since lost (REINDEX, TRUNCATE), the journal cannot tell what changed, or the members changed since the stamp are sift.col_connect_refresh_pct percent of the pairs of the
 * relation (an upper bound of its distinct members: below that a read patches them).
 */
struct SiftScopeStale
{
	int			n;
	Snapshot	snap;
	struct
	{
		Relation	rel,
					jidx;
		SiftConnStamp now;
		bool		vouches;
		double		members;
	}			s[SIFT_SCOPE_MAX];
};

SiftScopeStale *
sift_scope_stale_begin(Relation index, MemoryContext mcxt)
{
	SiftScopeStale *st;

	if (!sift_col_is_index(index) || sift_col_rel(index)->nscope == 0)
		return NULL;
	st = MemoryContextAllocZero(mcxt, sizeof(SiftScopeStale));
	st->n = sift_col_rel(index)->nscope;
	st->snap = IsInParallelMode() ? GetActiveSnapshot() : GetTransactionSnapshot();
	for (int i = 0; i < st->n; i++)
		if (journal_open(index, i, AccessShareLock, &st->s[i].rel, &st->s[i].jidx))
		{
			st->s[i].vouches = sift_conn_take(st->s[i].rel, st->s[i].jidx, st->snap, &st->s[i].now, NULL) == SIFT_CONN_OK;
			st->s[i].members = Max(sift_conn_rows_of(st->s[i].rel), 1);
		}
	return st;
}

bool
sift_scope_stale(SiftScopeStale *st, const SiftColSegment *seg)
{
	if (!(seg->entry.flags & SIFT_SEG_SCOPE))
		return false;
	for (int i = 0; i < st->n; i++)
	{
		SiftConnStamp stamp = i < seg->nscope ? seg->scope[i] : (SiftConnStamp) {0};
		double		limit = st->s[i].members * sift_col_connect_refresh_pct / 100;
		SiftConnKeys keys;
		bool		many;

		if (!st->s[i].vouches)
			continue;
		if (stamp.lineage != st->s[i].now.lineage)
			return true;
		if (sift_conn_changed_by(st->s[i].rel, st->s[i].jidx, &stamp, st->snap, (uint32) Max(limit, 1), true, &keys) != SIFT_CONN_OK)
			return true;
		many = keys.n >= limit;
		sift_conn_keys_free(&keys);
		if (many)
			return true;
	}
	return false;
}

void
sift_scope_stale_end(SiftScopeStale *st)
{
	for (int i = 0; i < st->n; i++)
		if (st->s[i].jidx != NULL)
			journal_close(st->s[i].rel, st->s[i].jidx);
}
