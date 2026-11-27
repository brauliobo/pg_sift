/*
 * col/v1/conn/found.c - the found flags of a connected index of Format 1 as the aggregate reads them (SiftConnOps, core/store/ops.h; agg/join/found.h)
 */
#include "postgres.h"

#include "access/transam.h"

#include "agg/vclass.h"
#include "col/am.h"
#include "col/v1/conn/conn.h"
#include "col/v1/seg/seg.h"
#include "col/v1/store_ops.h"
#include "col/v1/wbuf.h"

static int
found_conn(Relation heap, Relation index, AttrNumber fk, Oid parent, AttrNumber key, Oid *parent_index)
{
	int			leaf;

	return sift_conn_find(heap, index, fk, parent, key, &leaf, parent_index);
}

/* the write buffers hold no row */
static bool
wbufs_empty(Relation index, const SiftDirSnapshot *dir)
{
	for (int i = 0; i < dir->nsegs; i++)
	{
		uint64		rows,
					pages;

		if (!sift_seg_is_wbuf(&dir->segs[i]))
			continue;
		sift_cwbuf_summary(index, &dir->segs[i], &rows, &pages);
		if (rows > 0)
			return false;
	}
	return true;
}

/* the oldest of the stamps the segments were filled under for connection c, which are all of one journal */
static const char *
oldest_stamp(Relation index, const SiftDirSnapshot *dir, int c, SiftConnStamp *oldest, bool *clean)
{
	const char *why = NULL;

	for (int i = 0; why == NULL && i < dir->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftConnStamp st;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		st = c < seg->nconn ? seg->conn[c] : (SiftConnStamp) {0};
		sift_cseg_close(seg);
		*clean &= (st.reserved & SIFT_CONN_STAMP_CLEAN) != 0;
		if (st.lineage == 0)
			why = "segment without stamp";
		else if (oldest->lineage != 0 && oldest->lineage != st.lineage)
			why = "stamps of two journals";
		else if (oldest->lineage == 0 || TransactionIdPrecedes(st.xmin, oldest->xmin))
			*oldest = st;
	}
	return why != NULL ? why : oldest->lineage == 0 ? "no segment" : NULL;
}

static const char *
found_proof(Relation index, int c, SiftConnStamp *oldest, bool *clean)
{
	SiftDirSnapshot *dir = sift_col_dir_read(index, false);

	*oldest = (SiftConnStamp) {0};
	*clean = true;
	return wbufs_empty(index, dir) ? oldest_stamp(index, dir, c, oldest, clean) : "write buffer";
}

const SiftConnOps sift_v1_conn_ops = {
	.conn = found_conn,
	.proof = found_proof
};

const SiftSrcLeaf *
sift_v1_found_leaf(Relation index, const SiftSrcLayout *lay, int attno)
{
	const int	c = sift_v6_attno_found(attno);

	SIFT_INVARIANT(c >= 0 && c < sift_col_rel(index)->nconn, "index %u has no connection %d to read the found flag of", RelationGetRelid(index), c);
	return &lay->leaf[sift_col_rel(index)->conn_found[c]];
}
