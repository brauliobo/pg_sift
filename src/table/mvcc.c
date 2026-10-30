/*
 * table/mvcc.c - visibility of a row version of a sift_table under every snapshot type (docs/COL-TAM.md section 3).
 *
 * A version is visible when its insertion is (write buffer record: xmin/cmin; column group: frozen, or the xmin/cmin its unit keeps for the row) and no
 * DELETE or UPDATE tombstone of it counts as a deletion for the snapshot. LOCK tombstones never delete.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "table/table.h"

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

/* Does the tombstone e (not a LOCK) delete its row for the snapshot? Dirty snapshots also learn of an in-progress deleter */
static bool
tomb_deletes(Snapshot snapshot, const SiftTblTomb *e)
{
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
			return false;
		case SNAPSHOT_MVCC:
			if (TransactionIdIsCurrentTransactionId(e->xid))
				return e->cid < snapshot->curcid;
			if (XidInMVCCSnapshot(e->xid, snapshot))
				return false;
			return TransactionIdDidCommit(e->xid);
		case SNAPSHOT_SELF:
			return TransactionIdIsCurrentTransactionId(e->xid) || TransactionIdDidCommit(e->xid);
		case SNAPSHOT_DIRTY:
			if (TransactionIdIsCurrentTransactionId(e->xid) || TransactionIdDidCommit(e->xid))
				return true;
			if (TransactionIdIsInProgress(e->xid))
				snapshot->xmax = e->xid;
			return false;
		case SNAPSHOT_NON_VACUUMABLE:
			return TransactionIdDidCommit(e->xid) && (!TransactionIdIsNormal(e->xid) || sift_vis_removable_xid(snapshot->vistest, e->xid));
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

bool
sift_tbl_visible(Relation rel, const SiftTblRel *st, Snapshot snapshot, const SiftTblRowVer *rv)
{
	bool		dead;
	const SiftTblTomb *e;
	int			n;

	if (!sift_tbl_inserted_visible(snapshot, rv, &dead))
	{
		if (!dead && snapshot->snapshot_type == SNAPSHOT_MVCC)
			CheckForSerializableConflictOut(rel, rv->xmin, snapshot);
		return false;
	}
	if ((e = sift_tbl_tomb_find(st, rv->key, &n)) == NULL)
		return true;
	for (int i = 0; i < n; i++)
	{
		if (e[i].kind == SIFT_TBL_TOMB_LOCK)
			continue;
		if (tomb_deletes(snapshot, &e[i]))
			return false;
		if (snapshot->snapshot_type == SNAPSHOT_MVCC && TransactionIdIsValid(e[i].xid) && !TransactionIdDidAbort(e[i].xid))
			CheckForSerializableConflictOut(rel, e[i].xid, snapshot);
	}
	return true;
}

/*
 * Does a statement of this transaction delete the row for the snapshot? Then the row is no version of the snapshot whatever its insertion says
 * (it was inserted before): the repeated fetches of the old versions of a key updated many times in one transaction skip reading them
 */
bool
sift_tbl_deleted_by_self(const SiftTblRel *st, Snapshot snapshot, uint64 key)
{
	const SiftTblTomb *e;
	int			n;

	if ((e = sift_tbl_tomb_find(st, key, &n)) == NULL)
		return false;
	for (int i = 0; i < n; i++)
		if (e[i].kind != SIFT_TBL_TOMB_LOCK && TransactionIdIsCurrentTransactionId(e[i].xid) && tomb_deletes(snapshot, &e[i]))
			return true;
	return false;
}

void
sift_tbl_del_state(const SiftTblRel *st, uint64 key, SiftTblDel *del)
{
	const SiftTblTomb *e;
	int			n;

	memset(del, 0, sizeof(SiftTblDel));
	del->state = SIFT_TBL_DEL_NONE;
	if ((e = sift_tbl_tomb_find(st, key, &n)) == NULL)
		return;
	for (int i = 0; i < n; i++)
	{
		SiftTblDelState state;

		if (TransactionIdIsCurrentTransactionId(e[i].xid))
		{
			if (e[i].kind == SIFT_TBL_TOMB_LOCK)
			{
				del->self_locked = true;
				continue;
			}
			state = SIFT_TBL_DEL_SELF;
		}
		else if (TransactionIdIsInProgress(e[i].xid))
		{
			if (e[i].kind == SIFT_TBL_TOMB_LOCK)
			{
				del->lockxid = e[i].xid;
				continue;
			}
			state = SIFT_TBL_DEL_INPROGRESS;
		}
		else if (TransactionIdDidCommit(e[i].xid))
		{
			if (e[i].kind == SIFT_TBL_TOMB_LOCK)
				continue;
			state = e[i].kind == SIFT_TBL_TOMB_UPDATE ? SIFT_TBL_DEL_UPDATED : SIFT_TBL_DEL_DELETED;
		}
		else
			continue;			/* aborted */
		del->state = state;
		del->xid = e[i].xid;
		del->cid = e[i].cid;
		del->succ = e[i].succ;
		del->hot = e[i].kind == SIFT_TBL_TOMB_UPDATE && (e[i].flags & SIFT_TBL_TOMB_HOT);
	}
}
