/*
 * table/f2/row/state.h - what the disk and the stage of the transaction say of a row before it is locked: its inserter, its deleter and the version that replaced it, and the column
 * delta chain of its partition. The row locks (f2_lock.c) judge a request by this, once before they take the lock and again after every wait.
 */
#ifndef SIFT_TBL_F2_ROW_STATE_H
#define SIFT_TBL_F2_ROW_STATE_H

#include "postgres.h"

#include "access/tableam.h"
#include "utils/snapshot.h"

typedef struct SiftRowState
{
	bool		found;
	TransactionId xmin;
	CommandId	cmin;
	TransactionId xmax;
	CommandId	cmax;
	bool		has_succ;
	uint64		succ;
	uint64		part_id;		/* the partition the row is in */
} SiftRowState;

extern SiftRowState sift_row_state_read(Relation rel, uint64 tid);

/* The same for a row that is not in the stage of the transaction, without writing the stage */
extern SiftRowState sift_row_state_read_settled(Relation rel, uint64 tid);

/* TM_Ok for a row the statement may lock, else why not (the deleter and the successor in tmfd) */
extern TM_Result sift_row_state_result(const SiftRowState *rs, CommandId cid, TM_FailureData *tmfd, uint64 tid);

/* The row of a partition with a column delta chain, as its latest delta makes it for this statement; a row that no delta touched is TM_Ok */
extern TM_Result sift_row_delta_result(Relation rel, uint64 tid, CommandId cid, Snapshot snap, TM_FailureData *tmfd);
extern void sift_row_delta_ack(Relation rel, uint64 tid, CommandId cid);

/* The successor an update that the snapshot sees left on disk; false for a row that was not updated or whose update the snapshot does not see */
extern bool sift_row_visible_successor(Relation rel, uint64 tid, Snapshot snap, uint64 *succ);
extern uint32 sift_row_part_count(Relation rel);

#endif							/* SIFT_TBL_F2_ROW_STATE_H */
