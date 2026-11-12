/*
 * col/xstamp.h - who inserted the entries of a segment: the xid stamp of a heap sift_col index with track_changes
 */
#ifndef SIFT_COL_XSTAMP_H
#define SIFT_COL_XSTAMP_H

#include "access/transam.h"

#include "core/page.h"

/*
 * Every transaction that inserted an entry committed before the xid next, but those the bits of open name: bit i is the transaction i steps before base, which had
 * not finished when the stamp was made. A stamp with no bit set is SIFT_SEG_XSTAMP of the directory entry (reserved = next), the one a read may take for every inserter
 * committed and finished once its snapshot's xmin is not below next. One with bits set is SIFT_SEG_XPENDING (reserved = next, pad = base, sum_dl = open), which no read uses:
 * sift_xstamp_settle drops the bits of the transactions that committed, so that the stamp is a plain one when they all did, and none when one did not.
 */
typedef struct SiftXStamp
{
	TransactionId next;			/* InvalidTransactionId: the segment does not say */
	TransactionId base;
	uint64		open;
} SiftXStamp;

#define SIFT_XSTAMP_OPEN_MAX 64	/* the transactions a stamp may wait for, within that many xids of each other */

/* The xid before which every transaction that inserted an entry of the segment committed; false when the segment does not say, or has transactions still to commit */
static inline bool
sift_seg_xstamp(const SiftSegEntry *e, TransactionId *xid)
{
	if (!(e->flags & SIFT_SEG_XSTAMP))
		return false;
	*xid = e->reserved;
	return true;
}

static inline SiftXStamp
sift_seg_xstamp_of(const SiftSegEntry *e)
{
	if (e->flags & SIFT_SEG_XSTAMP)
		return (SiftXStamp) {e->reserved, InvalidTransactionId, 0};
	if (e->flags & SIFT_SEG_XPENDING)
		return (SiftXStamp) {e->reserved, e->pad, e->sum_dl};
	return (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0};
}

/* The pad word is a scope segment's ordinal: a segment of a scope never waits for a transaction (nothing stamps it) */
static inline void
sift_seg_set_xstamp(SiftSegEntry *e, SiftXStamp s)
{
	if (e->flags & SIFT_SEG_XPENDING)
		e->pad = 0;
	if (e->flags & (SIFT_SEG_XSTAMP | SIFT_SEG_XPENDING))
		e->reserved = e->sum_dl = 0;
	e->flags &= ~(SIFT_SEG_XSTAMP | SIFT_SEG_XPENDING);
	if (!TransactionIdIsNormal(s.next))
		return;
	e->reserved = s.next;
	if (s.open == 0)
		e->flags |= SIFT_SEG_XSTAMP;
	else
	{
		Assert(!(e->flags & SIFT_SEG_SCOPE));
		e->flags |= SIFT_SEG_XPENDING;
		e->pad = s.base;
		e->sum_dl = s.open;
	}
}

/* Who inserted a set of rows: top the latest xid among them (InvalidTransactionId: no stamp), xid the distinct ones that had not finished when their records were read */
typedef struct SiftXWait
{
	TransactionId top;
	int			n;
	TransactionId xid[SIFT_XSTAMP_OPEN_MAX];
} SiftXWait;

/* Notes a transaction that had not finished: false (and no stamp) when it is one too many */
extern bool sift_xwait_note(SiftXWait *w, TransactionId x);

/* The stamp of rows whose latest inserter is top and of which the transactions xid[n] had not finished (none when top is invalid, or they are more than SIFT_XSTAMP_OPEN_MAX xids apart); no commit log is asked */
extern SiftXStamp sift_xstamp_pack(TransactionId top, const TransactionId *xid, int n);

/* The stamp of the rows now: next one past top, the transactions of w that have finished dropped (all of them committed, or none), those that have not within SIFT_XSTAMP_OPEN_MAX xids of each other */
extern SiftXStamp sift_xstamp_make(SiftXWait *w);

/* Asks the commit log about the transactions the stamp waits for: the bits of those that committed go; false (and no stamp) when one did not commit or the log does not say */
extern bool sift_xstamp_settle(SiftXStamp *s);

/* The stamp of the rows of both: false when either has none */
extern bool sift_xstamp_union(SiftXStamp *a, const SiftXStamp *b);

/* The stamp of the rows of the segments, each settled first: none when one does not say */
extern SiftXStamp sift_xstamp_inputs(const SiftSegEntry *in, int n);

#endif
