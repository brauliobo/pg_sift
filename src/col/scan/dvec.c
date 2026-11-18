/*
 * col/scan/dvec.c - the deletion records of the tombstone log (core/tomb_dv.h) as the liveness a scan reads: the rows of a unit that a record ends for the snapshot are cleared in what the scan
 * sees of the unit, so a unit with records is one dirty unit (one record to find and one vector to take away) where marks make it leave the clean path row by row (docs/COL-HEAPFREE.md)
 */
#include "postgres.h"

#include "access/xact.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "col/scan_int.h"
#include "core/damage.h"
#include "core/membudget.h"
#include "core/popcount.h"
#include "core/tomb_dv.h"

typedef struct DvKey
{
	uint64		gen;
	uint32		unit;
	uint32		pad;
} DvKey;

typedef struct DvEnt
{
	DvKey		key;
	SiftColDvUnit unit;
	bool		has;
} DvEnt;

typedef struct ColDvOv
{
	SiftColDv	base;
	SiftColSource *src;
	MemoryContext cx;
	HTAB	   *tab;
	SiftDvRec  *rec;
	uint64	   *scratch;			/* the rows of the unit in hand: a unit that no record touches keeps nothing */
	TransactionId memo_xid;			/* the verdict of the last transaction asked about: a statement's records are of one */
	bool		memo_deletes;
} ColDvOv;

/* Does the record's transaction end its rows for the snapshot */
static bool
dv_deletes(ColDvOv *o, const SiftDvRec *r)
{
	SiftTomb	head = {.xid = r->xid,.cid = r->cid,.kind = SIFT_TOMB_DELETE};
	SiftTombVerdict verdict;

	if (r->xid == o->memo_xid && !TransactionIdIsCurrentTransactionId(r->xid))
		return o->memo_deletes;
	verdict = sift_tomb_verdict(o->src->snap, &head);
	if (verdict == SIFT_TOMB_UNKNOWN)
		SIFT_DAMAGE_INDEX(o->src->index, errmsg("a deletion record of index \"%s\" names a transaction that the commit log no longer holds", RelationGetRelationName(o->src->index)),
						  errhint("VACUUM the table to fold it, or REINDEX."));
	o->memo_xid = r->xid;
	o->memo_deletes = verdict == SIFT_TOMB_DELETES;
	return o->memo_deletes;
}

/* The rows of the unit in hand (o->scratch) kept for the scan: a unit that no record touches keeps nothing */
static void
dv_keep(ColDvOv *o, DvEnt *e, uint32 nw)
{
	SIFT_MEM_SCOPE(col_dv_overlay, o->cx);

	e->unit.bits = memcpy(MemoryContextAlloc(o->cx, nw * sizeof(uint64)), o->scratch, nw * sizeof(uint64));
	SIFT_MEM_CHECKPOINT();
}

static DvEnt *
dv_make(ColDvOv *o, const SiftColSegment *seg, uint32 u, const DvKey *key)
{
	SiftTombLog *log = o->src->hflog;
	uint32		n = seg->sbdir[u].nrows,
				nw = SIFT_COL_NWORDS(n);
	uint64	   *bits = o->scratch;
	SiftTombKeyWalk w;
	DvEnt	   *e = hash_search(o->tab, key, HASH_ENTER, NULL);
	uint32		ndead = 0;

	memset(bits, 0, nw * sizeof(uint64));
	/* the log was replaced since the scan opened it: what it says of this unit is not known, and the heap decides the rows from here on, as for a mark */
	if (log->gen != o->src->hfgen)
		o->src->hf = -1;
	else
	{
		sift_tomb_key_begin(&w, log, SIFT_DV_KEY(seg->entry.gen, u), o->cx);
		while (sift_dv_next(&w, o->rec))
		{
			if (o->rec->gen != seg->entry.gen || o->rec->desc.part != u || !dv_deletes(o, o->rec))
				continue;
			if (o->rec->desc.row_lo + o->rec->desc.span > n)
				SIFT_DAMAGE_DATA(seg->index, errmsg("sift_col: a deletion record of index \"%s\" names rows past unit %u of segment %llu", RelationGetRelationName(seg->index), u, (unsigned long long) seg->entry.gen));
			sift_dv_or_bits(o->rec, bits);
		}
		sift_tomb_key_end(&w);
	}
	for (uint32 i = 0; i < nw; i++)
		ndead += sift_pc64(bits[i]);
	e->has = ndead > 0;
	e->unit = (SiftColDvUnit) {ndead, NULL};
	if (e->has)
		dv_keep(o, e, nw);
	return e;
}

static const SiftColDvUnit *
dv_unit(SiftColDv *base, const SiftColSegment *seg, uint32 u)
{
	ColDvOv    *o = (ColDvOv *) base;
	DvKey		key = {seg->entry.gen, u, 0};
	DvEnt	   *e = hash_search(o->tab, &key, HASH_FIND, NULL);

	if (e == NULL)
		e = dv_make(o, seg, u, &key);
	return e->has ? &e->unit : NULL;
}

/*
 * The records of the log the source holds, for the segments it opened: none of them when the log has no record (the common case costs nothing), and the heap for every row when the oldest
 * record names a transaction the commit log may no longer hold (VACUUM freezes the records it keeps, so that does not come to be)
 */
void
col_dv_open(SiftColSource *src)
{
	ColDvOv    *o;
	HASHCTL		ctl = {.keysize = sizeof(DvKey),.entrysize = sizeof(DvEnt)};

	if (src->hflog == NULL || src->hflog->dvecs == 0)
		return;
#ifdef SIFT_TESTING
	if (sift_test_hf_ignore_tombs)
		return;
#endif
	if (TransactionIdIsNormal(src->hflog->dv_oldest) && sift_xid_state(src->hflog->dv_oldest) == SIFT_XID_UNKNOWN)
	{
		src->hf = -1;
		return;
	}
	o = MemoryContextAllocZero(src->mcxt, sizeof(ColDvOv));
	o->cx = AllocSetContextCreate(src->mcxt, "sift_col deletion records", ALLOCSET_DEFAULT_SIZES);
	ctl.hcxt = o->cx;
	o->tab = hash_create("sift_col deletion records", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	o->rec = MemoryContextAlloc(o->cx, sizeof(SiftDvRec));
	o->scratch = MemoryContextAlloc(o->cx, SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	o->src = src;
	o->base.unit = dv_unit;
	for (int i = 0; i < src->nseg; i++)
		src->seg[i].seg->dv = &o->base;
	src->dvov = o;
}
