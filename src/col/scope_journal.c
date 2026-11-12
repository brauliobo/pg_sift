/*
 * col/scope_journal.c - the journals of the scope relations of a scope index (col/scope.h, docs/COL-CONNECT.md "The rule")
 *
 * The scope segments hold the pairs (member, scope key) as of the stamps they were written under. What changed in the relations since is in their journals: a sift_col
 * index of a relation with track_changes = 'ctid' whose first column is the journaled key keeps, in its tombstone log, a record of kind KEYCHG for the key of every row an UPDATE,
 * DELETE or key change ended and one of kind KEYADD for the key of every row inserted (col/conn.c). The rule, the stamp, the verdicts and the set of changed keys are those of the
 * connections of 4.3; this file finds the journals of a scope and applies them to each. A scope of one path has one (the pairs relation keyed by its member); one of two paths has
 * three, and a member's pairs depend on its direct rows (J1), on its rows in the via relation (J3) and on the rows of the joined relation of each of its groups (J2: its journal
 * names the groups that changed, and the members of a group are the via relation's rows of it).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/scope.h"

/* The relations of the journals of a scope, and the btree of the via relation that lists the members of a group, open and locked */
typedef struct ScopeJournals
{
	int			n;
	Relation	rel[SIFT_SCOPE_JOURNALS];
	Relation	jidx[SIFT_SCOPE_JOURNALS];
	Oid			via_btree;		/* two paths: the unique btree of the via relation, probed by its via column for the members of a group */
	AttrNumber	via;
	AttrNumber	member;
} ScopeJournals;

/*
 * The relations and journal indexes of the journals of the scope, locked AccessShare (lock on the index: lock); false: the scope is not declared now, or one of its relations has no
 * journal index. Journal j: 0 the pairs relation keyed by its member, 1 the joined relation keyed by its via column, 2 the via relation keyed by its member column.
 */
static bool
journals_open(Relation index, int scope, LOCKMODE lock, ScopeJournals *J)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	const SiftScopeSpec *s;
	Relation	heap;
	bool		ok = true;

	*J = (ScopeJournals) {0};
	if (specs == NULL || scope < 0 || scope >= specs->n)
		return false;
	s = specs->s[scope];
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	J->n = sift_scope_njournals(s);
	for (int j = 0; j < J->n && ok; j++)
	{
		AttrNumber	a,
					b;
		Oid			btree = j == 0 ? sift_scope_relation_index(heap, index, s, &J->rel[j], &a, &b) : sift_scope_hop_index(heap, index, s, j, &J->rel[j], &a, &b);
		Oid			joid = sift_scope_hop_journal(J->rel[j], j, a, b, btree);

		if (j == 2)
		{
			J->via_btree = btree;
			J->via = a;
			J->member = b;
		}
		ok = OidIsValid(joid);
		if (ok)
			J->jidx[j] = index_open(joid, lock);
		else
			J->n = j + 1;
	}
	table_close(heap, NoLock);
	return ok;
}

static void
journals_close(ScopeJournals *J)
{
	for (int j = 0; j < J->n; j++)
	{
		if (J->jidx[j] != NULL)
			index_close(J->jidx[j], NoLock);
		table_close(J->rel[j], NoLock);
	}
}

bool
sift_scope_two_paths(Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int i = 0; specs != NULL && i < specs->n; i++)
		if (specs->s[i]->joined != NULL)
			return true;
	return false;
}

bool
sift_scope_has_journal(Relation index, int scope)
{
	ScopeJournals J;
	bool		ok = journals_open(index, scope, AccessShareLock, &J);

	journals_close(&J);
	return ok;
}

SiftConnWhy
sift_scope_take(Relation index, int scope, Snapshot snap, SiftConnStamp *stamp)
{
	ScopeJournals J;
	SiftConnWhy why = SIFT_CONN_NO_JOURNAL;

	memset(stamp, 0, SIFT_SCOPE_JOURNALS * sizeof(SiftConnStamp));
	if (journals_open(index, scope, AccessShareLock, &J))
		for (int j = 0; j < J.n && (j == 0 || why == SIFT_CONN_OK); j++)
			why = sift_conn_take(J.rel[j], J.jidx[j], snap, &stamp[j], NULL);
	journals_close(&J);
	if (why != SIFT_CONN_OK)
		memset(stamp, 0, SIFT_SCOPE_JOURNALS * sizeof(SiftConnStamp));
	return why;
}

static SiftConnWhy
journals_vouch(const ScopeJournals *J, const SiftConnStamp *stamp, Snapshot snap)
{
	SiftConnWhy why = SIFT_CONN_OK;

	for (int j = 0; j < J->n && why == SIFT_CONN_OK; j++)
		why = sift_conn_vouches(J->rel[j], J->jidx[j], &stamp[j], snap);
	return why;
}

SiftConnWhy
sift_scope_vouch(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap)
{
	ScopeJournals J;
	SiftConnWhy why = SIFT_CONN_NO_JOURNAL;

	if (journals_open(index, scope, AccessShareLock, &J))
		why = journals_vouch(&J, stamp, snap);
	journals_close(&J);
	return why;
}

/* The members the changed groups have, with those of the journals of the members, as a growing array: compacted when it doubles the cap, so that it never holds many more than the cap */
typedef struct MemberSet
{
	int64	   *m;
	uint32		n;
	uint32		space;
	uint32		cap;
	bool		over;
} MemberSet;

static void
members_add(MemberSet *ms, int64 member)
{
	if (ms->n == ms->space)
	{
		ms->n = sift_conn_keys_sort_unique(ms->m, ms->n);
		ms->over |= ms->n > ms->cap;
		if (ms->n * 2 > ms->space)
			ms->m = repalloc(ms->m, (ms->space = Max(ms->space * 2, 64)) * sizeof(int64));
	}
	ms->m[ms->n++] = member;
}

static bool
member_of_group(void *arg, TupleTableSlot *slot)
{
	const ScopeJournals *J = ((void **) arg)[0];
	MemberSet  *ms = ((void **) arg)[1];
	int64		member;

	if (sift_conn_slot_key(slot, J->member, &member))
		members_add(ms, member);
	return !ms->over;
}

/* The members of each group of keys that the via relation lists under snap, in ms */
static void
group_members(const ScopeJournals *J, const SiftConnKeys *groups, Snapshot snap, MemberSet *ms)
{
	SiftConnProbe *p;
	SiftConnProbeScan s;
	void	   *arg[2] = {(void *) J, ms};

	if (groups->n == 0)
		return;
	p = sift_conn_probe_begin_on(CurrentMemoryContext, J->rel[2], J->via_btree, J->via, snap);
	sift_conn_probe_open(p, &s);
	for (uint32 i = 0; i < groups->n && !ms->over; i++)
		sift_conn_probe_key(p, &s, groups->keys[i], member_of_group, arg);
	sift_conn_probe_close(&s);
}

/* The union of the changed keys of the journals in k[0..n) and the members of the groups of k[1] */
static SiftConnWhy
journals_changed(const ScopeJournals *J, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	SiftConnKeys k[SIFT_SCOPE_JOURNALS] = {{0}};
	SiftConnWhy why = SIFT_CONN_OK;
	MemberSet	ms = {.cap = cap};

	for (int j = 0; j < J->n && why == SIFT_CONN_OK; j++)
		why = sift_conn_changed_by(J->rel[j], J->jidx[j], &stamp[j], snap, cap, true, &k[j]);
	if (why == SIFT_CONN_OK && J->n == 1)
	{
		*out = k[0];
		return why;
	}
	if (why == SIFT_CONN_OK)
	{
		ms.m = palloc(64 * sizeof(int64));
		ms.space = 64;
		for (int j = 0; j < J->n; j += 2)
			for (uint32 i = 0; i < k[j].n; i++)
				members_add(&ms, k[j].keys[i]);
		group_members(J, &k[1], snap, &ms);
		ms.n = sift_conn_keys_sort_unique(ms.m, ms.n);
		if (ms.over || ms.n > cap)
			why = SIFT_CONN_OVERFLOW;
		else
			*out = (SiftConnKeys) {.keys = ms.m,.kept = palloc0(Max(ms.n, 1) * sizeof(bool)),.n = ms.n,.cap = ms.n};
	}
	for (int j = 0; j < J->n; j++)
		sift_conn_keys_free(&k[j]);
	if (why != SIFT_CONN_OK && ms.m != NULL)
		pfree(ms.m);
	return why;
}

SiftConnWhy
sift_scope_changed(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	ScopeJournals J;
	SiftConnWhy why = SIFT_CONN_NO_JOURNAL;

	*out = (SiftConnKeys) {0};
	if (journals_open(index, scope, AccessShareLock, &J))
		why = journals_changed(&J, stamp, snap, cap, out);
	journals_close(&J);
	return why;
}

/*
 * The journals of the scopes of the index, as those of the parents of a connected child are (sift_conn_start): the writers of the scope relations are held out from here to the end of the
 * statement, and an index that has one already is not locked at all.
 */
void
sift_scope_start(Relation heap, Relation index, bool read_after)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	List	   *seen = NIL;

	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		ScopeJournals J;
		bool		ok = journals_open(index, i, ShareUpdateExclusiveLock, &J);

		for (int j = 0; ok && j < J.n; j++)
		{
			if (!list_member_oid(seen, RelationGetRelid(J.jidx[j])))
				(void) sift_conn_enable(J.rel[j], J.jidx[j], read_after);
			seen = lappend_oid(seen, RelationGetRelid(J.jidx[j]));
		}
		journals_close(&J);
	}
	list_free(seen);
}

/*
 * Refresh: is a scope segment worth a rewrite for what its stamps say? The rule of sift_conn_stale for each scope whose journals vouch now: never filled (lineage 0), filled under a journal
 * a relation has since lost (REINDEX, TRUNCATE) or without the journals the scope has now, a journal cannot tell what changed, or the members changed since the stamps are
 * sift.col_connect_refresh_pct percent of the rows of the pairs relation, and for two paths of its via relation too (an upper bound of the distinct members: below that a read patches them).
 */
struct SiftScopeStale
{
	int			n;
	Snapshot	snap;
	Relation	index;
	Relation	heap;
	int			priced;			/* the refresh is due by its price: 0 not asked yet, 1 due, -1 not */
	struct
	{
		ScopeJournals J;
		SiftConnStamp now[SIFT_SCOPE_JOURNALS];
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
	st->index = index;
	st->heap = table_open(index->rd_index->indrelid, AccessShareLock);
	st->snap = RegisterSnapshot(IsInParallelMode() ? GetActiveSnapshot() : GetTransactionSnapshot());
	for (int i = 0; i < st->n; i++)
		if (journals_open(index, i, AccessShareLock, &st->s[i].J))
		{
			const ScopeJournals *J = &st->s[i].J;

			st->s[i].vouches = sift_scope_take(index, i, st->snap, st->s[i].now) == SIFT_CONN_OK;
			st->s[i].members = Max(sift_conn_rows_of(J->rel[0]) + (J->n > 1 ? sift_conn_rows_of(J->rel[2]) : 0), 1);
		}
	return st;
}

/*
 * Is the refresh cheaper than the reads of the index that pay for the members that changed since the stamps (sift_scope_refresh_due): what a read of the worst key of each scope pays, against
 * the rewrite of the scope segments, which refreshes every scope at once.
 */
static bool
refresh_priced(SiftScopeStale *st)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	double		read = 0,
				entry_columns = 0;
	uint64		rows = 0;

	sift_dir_read(st->index, dir);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !sift_seg_is_scope(&dir->segs[i]))
			rows += dir->segs[i].ndocs;
	for (int i = 0; i < st->n; i++)
	{
		SiftScopeCounts c;
		int			kept,
					all;

		sift_scope_columns(st->index, sift_scope_specs(st->index)->s[i]->key_name, &kept, &all);
		for (int j = 0; j < dir->nsegs; j++)
			if (sift_seg_is_scope(&dir->segs[j]) && sift_seg_scope_ordinal(&dir->segs[j]) == i)
				entry_columns += (double) dir->segs[j].ndocs * kept;
		if (st->s[i].vouches && sift_scope_patch_counts(st->heap, st->index, st->snap, i, &c))
			read += sift_scope_changed_price(&c);
	}
	pfree(dir);
	return sift_scope_refresh_due(read, sift_scope_refresh_price(rows, entry_columns));
}

bool
sift_scope_stale(SiftScopeStale *st, const SiftColSegment *seg)
{
	if (!(seg->entry.flags & SIFT_SEG_SCOPE))
		return false;
	for (int i = 0; i < st->n; i++)
	{
		const ScopeJournals *J = &st->s[i].J;
		SiftConnStamp stamp[SIFT_SCOPE_JOURNALS] = {{0}};
		double		limit = st->s[i].members * sift_col_connect_refresh_pct / 100;
		SiftConnKeys keys;
		bool		many;

		if (!st->s[i].vouches)
			continue;
		if (i < seg->nscope && seg->scope_journals[i] == J->n)
			memcpy(stamp, seg->scope[i], J->n * sizeof(SiftConnStamp));
		for (int j = 0; j < J->n; j++)
			if (stamp[j].lineage != st->s[i].now[j].lineage)
				return true;
		if (journals_changed(J, stamp, st->snap, (uint32) Max(limit, 1), &keys) != SIFT_CONN_OK)
			return true;
		many = keys.n >= limit;
		sift_conn_keys_free(&keys);
		if (many)
			return true;
	}
	if (st->priced == 0)
		st->priced = refresh_priced(st) ? 1 : -1;
	return st->priced > 0;
}

void
sift_scope_stale_end(SiftScopeStale *st)
{
	table_close(st->heap, NoLock);
	UnregisterSnapshot(st->snap);
	for (int i = 0; i < st->n; i++)
		journals_close(&st->s[i].J);
}
