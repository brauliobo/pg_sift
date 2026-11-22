/*
 * col/l0_stage.h - primitive 1 of format major 2: what a transaction writes is staged in memory, columnar, routed to the partition of each row, and reaches the index as sorted
 * L0 parts (64 rows or more of a partition) or as one batch of the partition's slab (fewer), never as rows.
 *
 * A stage belongs to one relation of the transaction. It is flushed when it holds work_mem, when the transaction is about to commit, and when the backend is going to read
 * the relation (sift_stage_flush). A subtransaction that aborts drops its own rows and deletes from memory: nothing of it was ever written; a row the transaction staged and deleted again is dropped with its delete at the commit. A flush finds the manifest changed since the rows were
 * routed (a split) and routes them again by their keys.
 */
#ifndef SIFT_L0_STAGE_H
#define SIFT_L0_STAGE_H

#include "core/host.h"

#include "col/f2/l0_slab.h"

typedef struct SiftStagePart
{
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	SiftRowBuf *rb;
} SiftStagePart;

/* A row of the index the transaction deleted: the key its entry was made with (it routes the delete to its partition), its address and the delete's own transaction and command */
typedef struct SiftStageDel
{
	uint8		key[SIFT_BOUND_BYTES];
	uint64		tid;
	uint64		succ_tid;		/* is_update: the address of the version that replaced the row */
	TransactionId xmax;
	CommandId	cmax;
	SubTransactionId subxid;
	bool		is_update;
} SiftStageDel;

/* What the stage tells the host of its relation: rows are written (a read of the relation, the end of the transaction), a subtransaction ends, the stage goes. rel is NULL but for the writes. */
typedef enum SiftStageEvent
{
	SIFT_STAGE_FLUSH,
	SIFT_STAGE_FLUSH_AT_END,	/* the transaction is about to commit or prepare */
	SIFT_STAGE_SUB_ABORT,
	SIFT_STAGE_SUB_COMMIT,
	SIFT_STAGE_FREE
} SiftStageEvent;

struct SiftTxnStage;
typedef void (*SiftStageHostEvent) (Relation rel, struct SiftTxnStage *st, SiftStageEvent ev, SubTransactionId sub, SubTransactionId parent);

typedef struct SiftTxnStage
{
	Oid			relid;
	RelFileLocator locator;
	SiftPartSpec spec;
	SiftManifestRead routing;	/* the partitions as of the version the rows were routed by */
	SiftStagePart *parts;
	int			nparts;
	uint64		mem_bytes;
	SiftStageDel *dels;
	int			ndels;
	int			capdels;
	uint32	   *del_htab;		/* the address of a delete to its place in dels, plus one (open addressing); NULL before the first delete */
	uint32		del_htab_cap;
	void	   *host_stage;		/* what the host of the relation stages besides rows and deletes (a table: its column delta records), NULL for none */
	SiftStageHostEvent host_event;	/* the host's answer to the events above, set with host_stage */
	uint32		bulk_rows;		/* a bulk insert (sift_stage_set_bulk): the rows at which the buffer of a partition is written; 0: the stage flushes at work_mem */
	uint64		bulk_budget;	/* the bytes the buffers of all partitions may hold */
	uint32		host_pending;	/* the records the host staged that the next read of the relation must see written */
	bool		by_value;		/* the rows are not tuples of the heap (a test stages them), so no flush may hold them as references */
} SiftTxnStage;

extern void sift_stage_define_gucs(void);

/* The stage of the relation in this transaction (made when there is none); the spec is copied */
extern SiftTxnStage *sift_stage_get(Relation rel, const SiftPartSpec *spec);

/*
 * A bulk insert (COPY, CTAS, INSERT ... SELECT, col/f2/f2_batch.h): the buffer of a partition is written, as one part of the cold tier written once and whole (SIFT_TIER_COLD_L2, its pages through
 * smgr_bulk_write), when it holds flush_rows rows, and the largest buffer when all of them hold budget_bytes; a buffer of fewer rows than a granule keeps the rule of a small write. Set for the
 * rest of the transaction.
 */
extern void sift_stage_set_bulk(SiftTxnStage *st, uint32 flush_rows, uint64 budget_bytes);

/* Stages a row: routed to its partition by key (the 40 bytes of core/range.h); flushes the stage when it is over work_mem (a bulk stage: see above) */
extern void sift_stage_add(Relation rel, SiftTxnStage *st, const uint8 *key, uint64 tid, uint32 xmin, uint32 cmin, const SiftCell *cells, const SiftCell *const *elem);

/*
 * Stages the delete of a row of the index (track_changes): held in memory until the transaction is about to commit, when the deletes are written for the partitions after the rows
 * (col/delete.h) under the transaction's own xid: a delete the transaction rolls back, or one of an aborted subtransaction, was never written. A transaction that stages more than
 * half of work_mem of them writes them as it goes, each under the xid of the (sub)transaction that deleted: the rows of a statement are not held in memory.
 */
extern void sift_stage_delete(SiftTxnStage *st, const uint8 *key, uint64 tid);

/* The delete of a row whose speculative insertion was undone after the stage wrote it: dead to every snapshot at once, even a dirty one (xmax FrozenTransactionId) */
extern void sift_stage_delete_spec_abort(SiftTxnStage *st, const uint8 *key, uint64 tid);

/* Takes a row that is still in memory out of the stage, with nothing written; false when it is not there (a flush wrote it) */
extern bool sift_stage_cancel_insert(SiftTxnStage *st, uint64 tid);

/* The delete of the version an UPDATE replaced: written as the pair of SIFT_PD_TID_UPDATED and SIFT_PD_TID_SUCC (col/pend_del.h) that tells a transaction waiting for the row where it went */
extern void sift_stage_delete_update(SiftTxnStage *st, const uint8 *key, uint64 old_tid, uint64 new_tid);

/* The delete of the address the transaction still holds in memory (not one it has written): true with who deleted it, when, and the version that replaced it */
extern bool sift_stage_find_delete(Relation rel, uint64 tid, TransactionId *xmax, CommandId *cmax, bool *has_succ, uint64 *succ);

/* The row staged at the address is dropped, as if it had never been staged: false when it is not in memory (the stage was written since). Only a row nothing has read yet may go: the new version of an UPDATE the same statement made */
extern bool sift_stage_cancel_tid(SiftTxnStage *st, uint64 tid);

/* The addresses the snapshot sees deleted by what the transaction staged for the relation, ascending (NULL: none) */
extern uint64 *sift_stage_dead(Relation rel, Snapshot snap, int *n);

/* Does the transaction hold in memory, not yet written, a row at an address of lo to hi */
extern bool sift_stage_holds_in(Relation rel, uint64 lo, uint64 hi);

/* held[i] is set for each tids[i] that is a row the transaction staged: its insert is not in the relation yet */
extern void sift_stage_rows_among(Relation rel, const uint64 *tids, int n, bool *held);

/* What the transaction staged for the relation so far (NULL: nothing) */
extern const SiftTxnStage *sift_stage_peek(Relation rel);

/* Writes everything the transaction staged for the relation; the rows staged afterwards start a new stage */
extern void sift_stage_flush(Relation rel);
#ifdef SIFT_TESTING
extern uint64 sift_stage_flushes;
#endif

/* Writes everything the transaction staged, for every relation: before a worker, which has no stage of its own, reads the indexes */
extern void sift_stage_flush_all(void);

/* The stage of the relation goes with what it holds (the index was built again: what was staged is in the heap the build read); nothing when there is none */
extern void sift_stage_discard(Oid relid);

/* A number that changes when a stage is made or goes: what keeps a pointer to a stage knows it is still the one of the transaction */
extern uint64 sift_stage_epoch(void);

/* The rows staged and not yet written for the relation */
extern uint64 sift_stage_rows(Relation rel);

/* Rows or deletes of the relation are staged: a read that finds none has nothing to flush */
extern bool sift_stage_has_pending(Relation rel);

#endif							/* SIFT_L0_STAGE_H */
