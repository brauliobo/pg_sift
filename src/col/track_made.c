#include "postgres.h"

#include "access/xact.h"
#include "utils/snapshot.h"

#include "col/am.h"

/*
 * The journal indexes that CREATE INDEX of a scope index made in this transaction (col/scope_make.c), with the command that made them: the writers of their relation are held out until
 * the transaction ends, so the log is complete for a snapshot taken after that command, and the scope segments built under such a snapshot may carry its stamp (the log is no one's baseline to
 * wait for: the transaction that made it is the one that reads). Entries of an earlier transaction never match: they need the log to be this transaction's (sift_col_track_own).
 */
#define MADE_N 8
static struct
{
	Oid			index;
	CommandId	cid;
}			made[MADE_N];
static int	made_next;

void
sift_col_track_made(Oid index)
{
	made[made_next++ % MADE_N] = (typeof(made[0])) {index, GetCurrentCommandId(false)};
}

bool
sift_col_track_made_before(Oid index, Snapshot snap)
{
	for (int i = 0; i < MADE_N; i++)
		if (made[i].index == index)
			return snap->curcid >= made[i].cid;
	return false;
}
