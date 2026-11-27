/*
 * table/f2/lock/int.h - what the row locks of a transaction touched (f2_lock_touched.c), for f2_lock.c: the rows it holds in tier 1, to give back at its end, the ones of a
 * subtransaction in a list of their own so that its end costs what it holds.
 */
#ifndef SIFT_TABLE_F2_LOCK_INT_H
#define SIFT_TABLE_F2_LOCK_INT_H

#include "postgres.h"

#include "access/xact.h"

#include "table/f2/lock/esc.h"

typedef struct SiftTouched
{
	SiftRowLockKey key;
	uint64		part_id;		/* 0 for the entries of speculative inserts, whose partition is not known */
	TransactionId xid;
	SubTransactionId subxid;
	int32		sub_prev;		/* the neighbours in the list of the subtransaction, -1 at its ends */
	int32		sub_next;
} SiftTouched;

/* True when the (sub)transaction did not hold the row already: a row is one entry for each xid that holds it, however many statements lock or update it */
extern bool sift_touched_add(const SiftRowLockKey *k, uint64 part_id);

/* The row a speculative insert gave up leaves the table of the transaction */
extern void sift_touched_forget(const SiftRowLockKey *k);

extern uint32 sift_touched_count(void);
extern const SiftTouched *sift_touched_at(uint32 i);

/* The first entry of the list of a subtransaction, -1 with none; sift_touched_next is the one after i */
extern int32 sift_touched_sub_first(SubTransactionId sub);
extern int32 sift_touched_next(uint32 i);

/* order[0..n): the positions in the window touched[at[0..n)] (at NULL: touched[base, base + n)) sorted by the stripe of their rows, for one lock of each stripe */
extern void sift_touched_by_stripe(const uint32 *at, uint32 base, uint32 n, uint16 *order);

/* The positions of the rows of the relation (of the partition, unless SIFT_ESC_PART_ALL) that the subtransaction holds, highest first, so that removing them in that order moves none of
 * the others; NULL with none, else palloc'd */
extern uint32 *sift_touched_sub_of(const RelFileLocator *loc, uint64 part_id, SubTransactionId sub, uint32 *n);

/* Removes entry i: the last entry takes its place, and the return is the position that one had (-1 when i was the last), for a walk that holds a position */
extern int32 sift_touched_remove(uint32 i);

/* A subtransaction released: its entries are its parent's, and retag runs on each of them once */
extern void sift_touched_promote(SubTransactionId sub, SubTransactionId parent, void (*retag) (const SiftTouched *t, SubTransactionId parent));

/* A subtransaction rolled back, and its entries are removed: it has no list left */
extern void sift_touched_sub_close(SubTransactionId sub);

extern void sift_touched_reset(void);

/* The entry of the row with its stripe locked in the mode, or NULL with nothing locked; sift_tbl_rowlock_unlock lets the stripe go */
extern SiftRowLockEntry *sift_tbl_rowlock_find_locked(Relation rel, uint64 tid, LWLockMode mode, int *si);
extern void sift_tbl_rowlock_unlock(int si);

/* The entry goes where it holds nothing (the stripe locked), and the row is no longer one this transaction touched */
extern void sift_tbl_rowlock_drop_idle(int si, SiftRowLockEntry *e);

#endif
