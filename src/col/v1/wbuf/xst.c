/*
 * col/v1/wbuf/xst.c - what is known of the inserter of a write buffer record when the record is read
 */
#include "postgres.h"
#include "core/v1_col/tomb.h"
#include "col/v1/wbuf/wbuf_int.h"

static TransactionId xst_last = InvalidTransactionId;
static uint8 xst_last_st;

/* Committed and aborted are final, and a read of the log does not repeat for the next record of the same transaction; the memory is for one pass over records, since an xid comes round */
uint8
wbuf_xst_of(TransactionId x)
{
	uint8		st;

	if (x == xst_last && TransactionIdIsValid(x))
		return xst_last_st;
	switch (sift_xid_state(x))
	{
		case SIFT_XID_COMMITTED:
			st = XST_COMMITTED;
			break;
		case SIFT_XID_OPEN:
			return XST_OPEN;
		case SIFT_XID_ABORTED:
			st = XST_ABORTED;
			break;
		default:
			return XST_NONE;
	}
	xst_last = x;
	xst_last_st = st;
	return st;
}

void
wbuf_xst_forget(void)
{
	xst_last = InvalidTransactionId;
}
