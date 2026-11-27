/*
 * table/v1/mvcc.c - visibility of a row version of a sift_table under every snapshot type (docs/COL-TAM.md section 3).
 *
 * A version is visible when its insertion is (write buffer record: xmin/cmin; column group: frozen, or the xmin/cmin its unit keeps for the row) and no
 * DELETE or UPDATE tombstone of it counts as a deletion for the snapshot. LOCK tombstones never delete.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/snapmgr.h"

#include "table/v1/table.h"

/* The chains whose settled prefix a backend keeps: one hop each (16 bytes and the hash entry), past them a walk starts from the chain's start */
#define SIFT_TBL_HOPS_MAX	65536

typedef struct TblHop
{
	uint64		from;			/* key */
	uint64		to;
} TblHop;

bool
sift_tbl_xid_aborted(TransactionId xid)
{
	return !TransactionIdIsCurrentTransactionId(xid) && !TransactionIdIsInProgress(xid) && !TransactionIdDidCommit(xid);
}

bool
sift_tbl_hot_next(const SiftTblRel *st, uint64 key, uint64 *succ)
{
	SiftTblDel	del;

	sift_tbl_del_state(st, key, &del);
	*succ = del.succ;
	return del.hot;
}

bool
sift_tbl_xid_removable(Relation rel, TransactionId xid)
{
	return !TransactionIdIsNormal(xid) || GlobalVisCheckRemovableXid(rel, xid);
}

/* The insertion of rv is visible to snapshot; *dead: no snapshot will ever see it (aborted insertion, killed record) */
bool
sift_tbl_inserted_visible(Snapshot snapshot, const SiftTblRowVer *rv, bool *dead)
{
	TransactionId xmin = rv->xmin;

	*dead = false;
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	if (!TransactionIdIsValid(xmin))
		return true;
	if (rv->flags & SIFT_TBL_REC_DEAD)
	{
		*dead = true;
		return snapshot->snapshot_type == SNAPSHOT_ANY;
	}
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
			return true;
		case SNAPSHOT_MVCC:
			if (TransactionIdIsCurrentTransactionId(xmin))
				return rv->cmin < snapshot->curcid;
			if (XidInMVCCSnapshot(xmin, snapshot))
				return false;
			if (TransactionIdDidCommit(xmin))
				return true;
			*dead = true;
			return false;
		case SNAPSHOT_SELF:
			if (TransactionIdIsCurrentTransactionId(xmin) || TransactionIdDidCommit(xmin))
				return true;
			*dead = !TransactionIdIsInProgress(xmin);
			return false;
		case SNAPSHOT_DIRTY:
			if (TransactionIdIsCurrentTransactionId(xmin))
				return true;
			if (TransactionIdIsInProgress(xmin))
			{
				snapshot->xmin = xmin;
				if (rv->flags & SIFT_TBL_REC_SPEC)
					snapshot->speculativeToken = rv->token;
				return true;
			}
			if (TransactionIdDidCommit(xmin))
				return true;
			*dead = true;
			return false;
		case SNAPSHOT_NON_VACUUMABLE:
			if (TransactionIdIsCurrentTransactionId(xmin) || TransactionIdIsInProgress(xmin) || TransactionIdDidCommit(xmin))
				return true;
			*dead = true;
			return false;
		default:
			SIFT_INVARIANT(false, "sift_table: unsupported snapshot type %d", (int) snapshot->snapshot_type);
	}
	return false;
}

SiftTblVerState
sift_tbl_ver_state(Relation rel, Snapshot snapshot, const SiftColVerSum *sum)
{
	SiftTblRowVer rv = {.xmin = sum->xmin_lo};
	bool		dead;

	if (!sum->any)
		return SIFT_TBL_VER_ALL;
	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_MVCC || sum->frozen_rows || sum->xmin_lo != sum->xmin_hi)
		return SIFT_TBL_VER_ROWS;
	if (TransactionIdIsCurrentTransactionId(sum->xmin_lo))
		return sum->cmin_hi < snapshot->curcid ? SIFT_TBL_VER_ALL : SIFT_TBL_VER_ROWS;
	if (sift_tbl_inserted_visible(snapshot, &rv, &dead))
		return SIFT_TBL_VER_ALL;
	if (!dead)
		CheckForSerializableConflictOut(rel, sum->xmin_lo, snapshot);
	return SIFT_TBL_VER_NONE;
}

/* The tombstone that deletes key for the snapshot, NULL when none does; the writers of the entries before it are a serializable conflict out */
static const SiftTomb *
ended_by(Relation rel, const SiftTblRel *st, Snapshot snapshot, uint64 key)
{
	const SiftTomb *e;
	int			n;

	if ((e = sift_tomb_find(&st->tomb, key, &n)) == NULL)
		return NULL;
	for (int i = 0; i < n; i++)
	{
		if (e[i].kind == SIFT_TOMB_LOCK)
			continue;
		if (sift_tomb_deletes(snapshot, &e[i]))
			return &e[i];
		if (snapshot->snapshot_type == SNAPSHOT_MVCC && TransactionIdIsValid(e[i].xid) && !TransactionIdDidAbort(e[i].xid))
			CheckForSerializableConflictOut(rel, e[i].xid, snapshot);
	}
	return NULL;
}

bool
sift_tbl_visible(Relation rel, const SiftTblRel *st, Snapshot snapshot, const SiftTblRowVer *rv)
{
	bool		dead;

	if (!sift_tbl_inserted_visible(snapshot, rv, &dead))
	{
		if (!dead && snapshot->snapshot_type == SNAPSHOT_MVCC)
			CheckForSerializableConflictOut(rel, rv->xmin, snapshot);
		return false;
	}
	return ended_by(rel, st, snapshot, rv->key) == NULL;
}

/* The hops of the log st reads now: none are kept across a rewrite of the log, which may free the versions they lead to */
static HTAB *
hops_of(SiftTblRel *st)
{
	if (st->hops != NULL && st->hops_gen != st->tomb.gen)
	{
		hash_destroy(st->hops);
		st->hops = NULL;
	}
	if (st->hops == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(TblHop),.hcxt = st->cxt};

		/* MEM[MEM-TBL-18]: v1 table relcache ctx (at most SIFT_TBL_HOPS_MAX entries) */
		st->hops = hash_create("sift_table chain hops", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		st->hops_gen = st->tomb.gen;
	}
	return st->hops;
}

/*
 * The version of the HOT chain from key on that the snapshot sees, or the last one; *dead: every update it went past is one no snapshot can miss. An MVCC snapshot starts from where
 * the walks before it found the chain settled: the members ended by such updates are passed by every snapshot, so the walk of a hot row costs the updates since, not its whole chain
 */
uint64
sift_tbl_chain_skip(Relation rel, SiftTblRel *st, Snapshot snapshot, uint64 key, bool *dead)
{
	HTAB	   *hops = snapshot->snapshot_type == SNAPSHOT_MVCC ? hops_of(st) : NULL;
	const uint64 from = key;
	const TblHop *hop = hops != NULL ? hash_search(hops, &from, HASH_FIND, NULL) : NULL;
	uint64		settled = hop != NULL ? hop->to : from;
	const SiftTomb *e;

	key = settled;
	while ((e = ended_by(rel, st, snapshot, key)) != NULL && e->kind == SIFT_TOMB_UPDATE && (e->flags & SIFT_TOMB_HOT))
	{
		const bool	gone = sift_tbl_xid_removable(rel, e->xid);

		if (dead)
			*dead &= gone;
		key = e->succ;
		if (gone && settled == e->key)
			settled = key;
	}
	if (hops != NULL && settled != from && (hop != NULL || hash_get_num_entries(hops) < SIFT_TBL_HOPS_MAX))
		((TblHop *) hash_search(hops, &from, HASH_ENTER, NULL))->to = settled;
	return key;
}

/*
 * Does a statement of this transaction delete the row for the snapshot? Then the row is no version of the snapshot whatever its insertion says
 * (it was inserted before): the repeated fetches of the old versions of a key updated many times in one transaction skip reading them
 */
bool
sift_tbl_deleted_by_self(const SiftTblRel *st, Snapshot snapshot, uint64 key)
{
	const SiftTomb *e;
	int			n;

	if ((e = sift_tomb_find(&st->tomb, key, &n)) == NULL)
		return false;
	for (int i = 0; i < n; i++)
		if (e[i].kind != SIFT_TOMB_LOCK && TransactionIdIsCurrentTransactionId(e[i].xid) && sift_tomb_deletes(snapshot, &e[i]))
			return true;
	return false;
}

void
sift_tbl_del_state(const SiftTblRel *st, uint64 key, SiftTblDel *del)
{
	const SiftTomb *e;
	int			n;

	memset(del, 0, sizeof(SiftTblDel));
	del->state = SIFT_TBL_DEL_NONE;
	if ((e = sift_tomb_find(&st->tomb, key, &n)) == NULL)
		return;
	for (int i = 0; i < n; i++)
	{
		SiftTblDelState state;

		if (TransactionIdIsCurrentTransactionId(e[i].xid))
		{
			if (e[i].kind == SIFT_TOMB_LOCK)
			{
				del->self_locked = true;
				continue;
			}
			state = SIFT_TBL_DEL_SELF;
		}
		else if (TransactionIdIsInProgress(e[i].xid))
		{
			if (e[i].kind == SIFT_TOMB_LOCK)
			{
				del->lockxid = e[i].xid;
				continue;
			}
			state = SIFT_TBL_DEL_INPROGRESS;
		}
		else if (TransactionIdDidCommit(e[i].xid))
		{
			if (e[i].kind == SIFT_TOMB_LOCK)
				continue;
			state = e[i].kind == SIFT_TOMB_UPDATE ? SIFT_TBL_DEL_UPDATED : SIFT_TBL_DEL_DELETED;
		}
		else
			continue;			/* aborted */
		del->state = state;
		del->xid = e[i].xid;
		del->cid = e[i].cid;
		del->succ = e[i].succ;
		del->hot = e[i].kind == SIFT_TOMB_UPDATE && (e[i].flags & SIFT_TOMB_HOT);
	}
}
