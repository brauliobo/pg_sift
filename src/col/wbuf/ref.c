/*
 * col/wbuf/ref.c - the values of the REF records of a write buffer: a record keeps the TID and the xid of the inserter (SIFT_COLR_REF), its values are in the arena of the
 * buffer (col/wbuf/arena.c), kept by the backend that inserted the row. They are taken from there when wbuf_next_rec reaches the first REF record of a page copy, for all of its
 * records from there, and handed out as the encoded payload the insert would have written: every reader of the buffer decodes them as it does any other record.
 * The heap is read only for the records the arena has no entry for (after a restart, on a standby, or when the arena dropped the buffer), in block order, and the values go to the arena.
 * The tuple at the TID is read with SnapshotAny, and
 *   - has the xmin of the record: its values, whatever became of the tuple since (a visibility check follows later, as for any buffered row);
 *   - is frozen: its values when the xid of the record precedes the removal horizon of the heap (a tuple is frozen once its inserter is older than every snapshot); else it is
 *     another tuple's. A tuple that carries the xid of the record and was frozen since is accepted by this rule, and a TID reused by a later tuple that is frozen too is not told
 *     apart: the TID of an index entry is not reused before the VACUUM that removes the entry has flagged the record dead, which the reader skips;
 *   - is the member of a HOT chain whose root was pruned (the line pointer of the TID redirects): its values, a HOT update changes no column of the index;
 *   - is another tuple (its xmin differs), or the line pointer is dead or unused, or the page is gone: no row, no one can see it.
 */
#include "postgres.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "utils/snapmgr.h"
#include "col/am.h"
#include "core/compat.h"
#include "col/wbuf/wbuf_int.h"

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare(&(*(RefRow *const *) a)->tid, &(*(RefRow *const *) b)->tid);
}

/* The REF records of the page copy that are not flagged dead, from byte from on, in page order */
static void
ref_collect(SiftCWbufReader *r, Size from)
{
	PageCursor	c = {.index = r->cur.index};

	wbuf_cursor_set(&c, r->page);
	c.pos = from;
	r->nref = r->refnext = 0;
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

		if ((h->flags & SIFT_COLR_REF) && !(h->flags & SIFT_COLR_DELETED))
		{
			RefRow	   *x;

			if (!(h->flags & SIFT_COLR_XMIN) || h->len != sizeof(TransactionId))
				SIFT_CORRUPT(c.index, "A reference record of segment %llu has flags %x and %u payload bytes.", (unsigned long long) r->entry.gen, h->flags, (unsigned) h->len);
			if (r->nref == r->refcap)
			{
				r->refcap = Max(64, 2 * r->refcap);
				SIFT_REGROW_IN(r->mcxt, r->ref, r->refcap * sizeof(RefRow));
			}
			x = &r->ref[r->nref++];
			*x = (RefRow) {.pos = c.pos,.tid = h->tid};
			memcpy(&x->xmin, h + 1, sizeof(TransactionId));
		}
		c.pos += size;
	}
}

/* Is the tuple in the slot the one of the record? root: it is at the TID of the record, not the chain member the line pointer redirects to (the member after the pruned root) */
static bool
ref_is_ours(Relation heap, TupleTableSlot *slot, TransactionId xmin, bool root)
{
	bool		shouldFree;
	HeapTupleHeader th = ExecFetchSlotHeapTuple(slot, false, &shouldFree)->t_data;

	if (!root)
		return true;
	return HeapTupleHeaderXminFrozen(th) ? GlobalVisCheckRemovableXid(heap, xmin) : TransactionIdEquals(HeapTupleHeaderGetRawXmin(th), xmin);
}

/* Fetches the values of the n records ord names (ascending TIDs) and appends them, encoded, to refpay and to the arena of the buffer */
static void
ref_fetch(SiftCWbufReader *r, RefRow **ord, int n)
{
	Relation	index = r->cur.index;
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer refs", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	MemoryContext rowcxt = AllocSetContextCreate(tmp, "sift col write buffer ref row", ALLOCSET_DEFAULT_SIZES);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	/* a private copy: the detoasting of a value locks the toast table, and a reload of the relcache entry frees the cached state */
	SiftColRel *rel = sift_col_rel_private(index, tmp);
	IndexInfo  *info = BuildIndexInfo(index);
	EState	   *estate = CreateExecutorState();
	ExprContext *ec = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	IndexFetchTableData *fetch = sift_table_index_fetch_begin(heap);
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

	for (int i = 0; i < n; i++)
	{
		RefRow	   *x = ord[i];
		ItemPointerData t = x->tid;
		bool		again = false,
					dead = false;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		SiftColRow	row;
		uint32		jnull;

		if (ItemPointerGetBlockNumber(&t) >= nblocks || !table_index_fetch_tuple(fetch, &t, SnapshotAny, slot, &again, &dead) ||
			!ref_is_ours(heap, slot, x->xmin, ItemPointerGetOffsetNumber(&t) == ItemPointerGetOffsetNumber(&x->tid)))
			continue;
		ec->ecxt_scantuple = slot;
		FormIndexDatum(info, slot, estate, values, isnull);
		sift_col_make_row(rel, &x->tid, values, isnull, &row, rowcxt);
		x->off = r->refpay.len;
		wbuf_encode_row(&r->refpay, rel->schema, &row, &jnull);
		x->len = r->refpay.len - x->off;
		x->live = true;
		(void) wbuf_arena_put(index, r->entry.gen, &x->tid, x->xmin, r->refpay.data + x->off, x->len, PG_UINT64_MAX);
		ResetPerTupleExprContext(estate);
		MemoryContextReset(rowcxt);
	}
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(heap, AccessShareLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

static void
ref_page(SiftCWbufReader *r, Size from)
{
	RefRow	  **ord;
	int			n = 0;

	if (r->refpay.data == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(r->mcxt);

		initStringInfo(&r->refpay);
		MemoryContextSwitchTo(old);
	}
	resetStringInfo(&r->refpay);
	ref_collect(r, from);
	wbuf_arena_read(r->cur.index, r->entry.gen, r->ref, r->nref, &r->refpay);
	ord = palloc(r->nref * sizeof(RefRow *));
	for (int i = 0; i < r->nref; i++)
		if (!r->ref[i].live)
			ord[n++] = &r->ref[i];
	if (n > 0)
	{
		qsort(ord, n, sizeof(RefRow *), tid_cmp);
		ref_fetch(r, ord, n);
	}
	pfree(ord);
	r->refready = true;
}

/*
 * The values of the REF record at byte pos of the page copy, as the payload of rec (valid until the page copy goes); false when the heap has no tuple for it. The records are asked
 * for in page order, the first of them fetching those of the rest of the page.
 */
bool
wbuf_ref_values(SiftCWbufReader *r, Size pos, RawRec *rec)
{
	RefRow	   *x;

	if (!r->refready)
		ref_page(r, pos);
	while (r->ref[r->refnext].pos < pos)
		r->refnext++;
	x = &r->ref[r->refnext++];
	Assert(x->pos == pos && r->refnext <= r->nref);
	rec->payload = (const uint8 *) r->refpay.data + x->off;
	rec->len = x->len;
	return x->live;
}
