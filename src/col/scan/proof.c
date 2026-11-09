/*
 * col/scan/proof.c - what the stamps of a connected index say about the rows a source reads (docs/COL-CONNECT.md, "Found")
 */
#include "postgres.h"

#include "access/transam.h"
#include "col/conn.h"
#include "col/scan_int.h"
#include "col/wbuf.h"

/* the number of the connection whose flag that the parent row was found is the leaf, -1 for another leaf */
static int
found_conn(const SiftColSchema *schema, int leaf)
{
	int			conn = 0;

	for (int i = 0; i <= leaf; i++)
	{
		const SiftColDef *d = &schema->cols[i];

		if (!(d->flags & SIFT_COLD_FOUND))
			continue;
		if (i == leaf)
			return conn;
		conn++;
	}
	return -1;
}

/*
 * NULL when the flag of the connection is the whole story of the rows the source reads: they are all in stored segments, each filled under a stamp of one
 * lineage, and the write buffers hold no row. Then *oldest is the oldest of the stamps (the keys that changed since it are the keys any row may be wrong
 * for) and *clean says no row of any segment had a key the parent lacked (a parent row inserted since then changes no flag). Else the reason, as a word for EXPLAIN.
 */
const char *
sift_col_source_proof(SiftColSource *src, int found_leaf, SiftConnStamp *oldest, bool *clean)
{
	int			conn = found_conn(src->schema, found_leaf);

	SIFT_INVARIANT(conn >= 0, "sift: leaf %d is not the flag of a connection", found_leaf);
	*oldest = (SiftConnStamp) {0};
	*clean = true;
	for (int i = 0; i < src->dir.nsegs; i++)
	{
		uint64		rows,
					pages;

		if (!sift_seg_is_wbuf(&src->dir.segs[i]))
			continue;
		sift_cwbuf_summary(src->index, &src->dir.segs[i], &rows, &pages);
		if (rows > 0)
			return "write buffer";
	}
	for (int i = 0; i < src->nseg; i++)
	{
		const SiftConnStamp *st = &src->seg[i].seg->conn[conn];

		if (st->reserved == SIFT_CONN_STAMP_REFUSED)
			return "parent too large to resolve";
		if (st->lineage == 0)
			return "segment without stamp";
		*clean &= (st->reserved & SIFT_CONN_STAMP_CLEAN) != 0;
		if (oldest->lineage == 0)
			*oldest = *st;
		else if (oldest->lineage != st->lineage)
			return "stamps of two journals";
		else if (TransactionIdPrecedes(st->xmin, oldest->xmin))
			oldest->xmin = st->xmin;
	}
	return oldest->lineage == 0 ? "no segment" : NULL;
}
