/*
 * col/xstamp.c - the xid stamp of a segment and the transactions it still waits for (col/xstamp.h)
 */
#include "postgres.h"

#include "col/xstamp.h"
#include "core/tomb.h"

/* The steps from lo up to hi, -1 when they are SIFT_XSTAMP_OPEN_MAX or more */
static int
xid_gap(TransactionId hi, TransactionId lo)
{
	for (int d = 0; d < SIFT_XSTAMP_OPEN_MAX; d++)
	{
		if (lo == hi)
			return d;
		TransactionIdAdvance(lo);
	}
	return -1;
}

bool
sift_xwait_note(SiftXWait *w, TransactionId x)
{
	for (int k = 0; k < w->n; k++)
		if (w->xid[k] == x)
			return true;
	if (w->n == SIFT_XSTAMP_OPEN_MAX)
		return false;
	w->xid[w->n++] = x;
	return true;
}

SiftXStamp
sift_xstamp_pack(TransactionId top, const TransactionId *xid, int n)
{
	SiftXStamp	s = {InvalidTransactionId, InvalidTransactionId, 0};

	if (!TransactionIdIsNormal(top))
		return s;
	for (int i = 0; i < n; i++)
		if (!TransactionIdIsValid(s.base) || TransactionIdFollows(xid[i], s.base))
			s.base = xid[i];
	for (int i = 0; i < n; i++)
	{
		int			d = xid_gap(s.base, xid[i]);

		if (d < 0)
			return (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0};
		s.open |= UINT64CONST(1) << d;
	}
	TransactionIdAdvance(top);
	s.next = top;
	return s;
}

SiftXStamp
sift_xstamp_make(SiftXWait *w)
{
	int			kept = 0;

	/* the transactions that finished since their records were read leave no bit: a promotion takes long enough for most of them to */
	for (int i = 0; i < w->n; i++)
		switch (sift_xid_state(w->xid[i]))
		{
			case SIFT_XID_COMMITTED:
				break;
			case SIFT_XID_OPEN:
				w->xid[kept++] = w->xid[i];
				break;
			default:
				return sift_xstamp_pack(InvalidTransactionId, NULL, 0);
		}
	w->n = kept;
	return sift_xstamp_pack(w->top, w->xid, kept);
}

bool
sift_xstamp_settle(SiftXStamp *s)
{
	TransactionId x = s->base;

	if (!TransactionIdIsNormal(s->next))
		return false;
	for (int i = 0; i < SIFT_XSTAMP_OPEN_MAX && s->open >> i; i++)
	{
		if (s->open >> i & 1)
			switch (sift_xid_state(x))
			{
				case SIFT_XID_COMMITTED:
					s->open &= ~(UINT64CONST(1) << i);
					break;
				case SIFT_XID_OPEN:
					break;
				default:
					s->next = InvalidTransactionId;
					return false;
			}
		TransactionIdRetreat(x);
	}
	return true;
}

/* The bits of a with its base moved up to base: false when one is pushed out of the word */
static bool
open_rebase(uint64 *open, TransactionId from, TransactionId base)
{
	int			d = *open == 0 ? 0 : xid_gap(base, from);

	if (d < 0 || (*open << d) >> d != *open)
		return false;
	*open <<= d;
	return true;
}

bool
sift_xstamp_union(SiftXStamp *a, const SiftXStamp *b)
{
	uint64		bo = b->open;
	TransactionId base;

	if (!TransactionIdIsNormal(a->next) || !TransactionIdIsNormal(b->next))
		return false;
	base = bo == 0 ? a->base : a->open == 0 || TransactionIdFollows(b->base, a->base) ? b->base : a->base;
	if (!open_rebase(&a->open, a->base, base) || !open_rebase(&bo, b->base, base))
	{
		a->next = InvalidTransactionId;
		return false;
	}
	a->open |= bo;
	a->base = a->open == 0 ? InvalidTransactionId : base;
	if (TransactionIdFollows(b->next, a->next))
		a->next = b->next;
	return true;
}

SiftXStamp
sift_xstamp_inputs(const SiftSegEntry *in, int n)
{
	SiftXStamp	all = {.next = InvalidTransactionId};

	for (int i = 0; i < n; i++)
	{
		SiftXStamp	s = sift_seg_xstamp_of(&in[i]);

		if (!sift_xstamp_settle(&s) || (i > 0 && !sift_xstamp_union(&all, &s)))
			return (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0};
		if (i == 0)
			all = s;
	}
	return all;
}
