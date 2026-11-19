/*
 * col/verify.c - sift.col_verify(index, heapallindexed): the structural check of the store, every segment and write buffer, the
 * totals of the metapage, that no live row is in the index twice and, with heapallindexed, that every heap tuple visible to a snapshot is
 * in the index with the very values the index tuple forms.
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "col/am.h"
#include "col/order_bound.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/check.h"
#include "core/cmp.h"
#include "core/dirchain.h"
#include "core/errctx.h"
#include "core/ops.h"
#include "core/membudget.h"

#define corrupt SIFT_CORRUPT

/*
 * A row's values (hash), and the types of its overflow values apart (tags): a value stored under minor 3 has none, and then the row's types are
 * not compared (known is false). A string the unit's byte budget turned into an overflow has no type either, and the heap still holds the string:
 * a row with one (strovf) is compared by the hash of its other leaves (rest).
 */
typedef struct RowHash
{
	SiftKey		key;
	uint64		hash;
	uint64		rest;
	uint64		tags;
	bool		known;
	bool		strovf;
	uint64		khash[SIFT_SCOPE_MAX];	/* a row of the entities: hash and rest over the leaves the scope segments of each scope with cols(..) keep, and strovf of them as bits */
	uint64		krest[SIFT_SCOPE_MAX];
	uint8		kovf;
	int			sleaf;			/* an entry of a scope segment: the derived key leaf that holds its scope key, and the key (a row of the entities: -1) */
	int64		skey;
} RowHash;

/*
 * One verification, in steps (core/check.h): the store and the directory, each segment, then what needs all of them. Memory: the keys of the live rows go to a
 * sort that spills (maintenance_work_mem), and so do the hashes of the rows, which are kept only when the index has scope segments or heapallindexed compares the
 * rows with the heap: one sort in the order of (address, scope leaf, scope key), where the rows of the entities (scope leaf -1) come before their copies in the scope
 * segments, and the heap scan, which goes by block, walks it with the rows of one block at a time.
 */
typedef struct ColVerify
{
	SiftErrCtx	errctx;
	Snapshot	snapshot;		/* taken before the directory was read: the heap rows it shows were inserted before the rows of the index were read */
	ResourceOwner owner;		/* of the call: the temporary files of the key sort belong to it, not to the subtransaction of the step that spilled */
	Relation	index;
	SiftColRel *r;
	MemoryContext cxt;
	SiftDirSnapshot *snap;
	SiftDirPart *parts;			/* the partitions of the directory's chain: their entries are those of snap from first on */
	int			nparts;
	SiftColBatch *batch;
	Tuplesortstate *keys;
	Tuplesortstate *rows;		/* the RowHash of every live entry, entities and scope segments */
	TupleTableSlot *put;
	TupleTableSlot *get;
	bytea	   *img;			/* the RowHash being put */
	uint64		nscope;			/* the live entries of the scope segments */
	const SiftSegEntry *seg;	/* the segment of the step */
	uint64		ndocs;
	bool		hashes;
	bool		scoped;			/* the index has scope segments: the sorted rows are read twice with heapallindexed */
	bool		heapallindexed;
	bool		sorted;
} ColVerify;

static int
rowhash_cmp(const void *a, const void *b)
{
	SiftKey		x = ((const RowHash *) a)->key,
				y = ((const RowHash *) b)->key;

	return SIFT_CMP3(x, y);
}

static uint64
value_hash(uint64 h, const SiftColDef *def, const SiftColValue *v)
{
	uint64		x;

	if (v->overflow)
		x = 2;
	else if (v->isnull)
		x = 1;
	else if (def->store == SIFT_COLS_STR)
		x = hash_bytes_extended((const unsigned char *) v->v.s, v->len, 3);
	else if (def->store == SIFT_COLS_INT)
		x = (uint64) v->v.i;
	else
		x = (uint64) sift_col_f8_key(v->v.f);
	return hash_combine64(h, murmurhash64(x));
}

static void
value_hashes(RowHash *rh, const SiftColDef *def, const SiftColValue *v)
{
	rh->hash = value_hash(rh->hash, def, v);
	if (def->store != SIFT_COLS_STR && def->store != SIFT_COLS_VAR)
		rh->rest = value_hash(rh->rest, def, v);
	else if (v->overflow && v->tag == SIFT_COLTAG_UNKNOWN)
		rh->strovf = true;
	if (v->overflow)
	{
		rh->tags = hash_combine64(rh->tags, murmurhash64(v->tag));
		rh->known &= v->tag != SIFT_COLTAG_UNKNOWN;
	}
}

/* The hashes of a row, whether it comes from the heap (the index tuple formed again) or from the index (decoded) */
static RowHash
row_hash(const SiftColSchema *sc, const SiftColRow *row, SiftKey key, const bool *kept)
{
	RowHash		rh = {.key = key,.known = true,.sleaf = -1};

	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if ((def->flags & SIFT_COLD_DERIVED) || (kept != NULL && !kept[c]))	/* derived: as of the stamp of the segment, not of the heap row */
			continue;
		if (def->parent < 0)
			value_hashes(&rh, def, &row->val[c]);
		else
		{
			int64		n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i;

			for (int64 i = 0; i < n; i++)
				value_hashes(&rh, def, &row->elem[c][i]);
		}
	}
	return rh;
}

/*
 * The lanes of a wide value agree: one NULL and overflow flag for all, a numeric image is a canonical image and its
 * display scale one PostgreSQL has, an interval's total is the one its time, day and month give
 */
static void
check_wide(Relation index, const SiftColSchema *sc, const SiftColRow *row)
{
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		const SiftColValue *v = &row->val[c];

		if (def->lanes < 2 || sift_col_def_arrlen_head(def))	/* the lengths of an array are checked with its value: array_length of an empty array is NULL */
			continue;
		if (sift_col_def_shape(def))
		{
			SiftColValue lane[SIFT_COL_MAX_LANES] = {{{0}}};

			/* the lanes are what the image gives: the rows of a NULL head have NULL lanes, any other row the lanes of its datum (an overflow where a bound does not fit) */
			if (!v->isnull)
			{
				sift_col_shape_values(def, PointerGetDatum(memcpy(palloc(v->len), v->v.s, v->len)), lane);
				for (int k = 1; k < def->lanes; k++)
					if (row->val[c + k].isnull != lane[k].isnull || row->val[c + k].overflow != lane[k].overflow || (!lane[k].isnull && row->val[c + k].v.i != lane[k].v.i))
						corrupt(index, "Lane %d of leaf %d is not what the value gives.", k, c);
			}
			else
				for (int k = 1; k < def->lanes; k++)
					if (!row->val[c + k].isnull)
						corrupt(index, "Lane %d of leaf %d holds a value for a NULL.", k, c);
			continue;
		}
		for (int k = 1; k < def->lanes; k++)
			if (row->val[c + k].isnull != v->isnull || row->val[c + k].overflow != v->overflow)
				corrupt(index, "The lanes of leaf %d disagree on whether a value is NULL.", c);
		if (v->isnull)
			continue;
		if (def->store == SIFT_COLS_STR)
		{
			if (!sift_col_numimg_valid((const uint8 *) v->v.s, v->len))
				corrupt(index, "Leaf %d holds a numeric image that is not canonical.", c);
			if (row->val[c + 1].v.i < 0 || row->val[c + 1].v.i > SIFT_NIMG_DSCALE_MAX)
				corrupt(index, "Leaf %d holds a display scale of %lld.", c, (long long) row->val[c + 1].v.i);
		}
		else if (def->type == SIFT_COLT_INTERVAL)
		{
			sift_w128	total = sift_col_wide_join(v->v.i, row->val[c + 1].v.i);
			int64		time;
			int32		day,
						month;

			sift_col_interval_raw(total, row->val[c + 2].v.i, &time, &day, &month);
			if (sift_col_interval_total(time, day, month) != total)
				corrupt(index, "Leaf %d holds an interval whose total is not that of its month, day and time.", c);
		}
	}
}

/* The derived key leaf a row of a scope segment holds a key in: exactly one of them (the scope the segment is of) */
static int
scope_leaf_of(Relation index, const SiftColSchema *sc, const SiftColBatch *b, uint32 r)
{
	int			leaf = -1;

	for (int c = 0; c < sc->ncols; c++)
		if ((sc->cols[c].flags & SIFT_COLD_SCOPE) && !sift_col_bit_test(b->col[c].nulls, r))
		{
			if (leaf >= 0)
				corrupt(index, "Row (%u,%u) of a scope segment holds the keys of two scopes.", sift_key_block(b->keys[r]), (uint32) (b->keys[r] & 0xFFFF));
			leaf = c;
		}
	if (leaf < 0)
		corrupt(index, "Row (%u,%u) of a scope segment holds no scope key.", sift_key_block(b->keys[r]), (uint32) (b->keys[r] & 0xFFFF));
	return leaf;
}

/* Does the scope s keep values of some of the leaves only (cols(..))? */
static bool
scope_partial(const SiftColRel *r, int s)
{
	for (int c = 0; c < r->schema->ncols; c++)
		if (!r->scope_cols[s][c])
			return true;
	return false;
}

/* The hashes of a row of the entities, and of its copy in each scope with cols(..): the leaves that scope keeps */
static RowHash
entity_hash(const SiftColRel *r, const SiftColRow *row, SiftKey key)
{
	RowHash		rh = row_hash(r->schema, row, key, NULL);

	for (int s = 0; s < r->nscope; s++)
		if (scope_partial(r, s))
		{
			RowHash		k = row_hash(r->schema, row, key, r->scope_cols[s]);

			rh.khash[s] = k.hash;
			rh.krest[s] = k.rest;
			rh.kovf |= k.strovf << s;
		}
	return rh;
}

/* Does the entry of a scope segment hold the values its row of the entities has, in the leaves the scope keeps */
static bool
scope_same(const SiftColRel *r, const RowHash *e, const RowHash *s)
{
	int			so = 0;

	while (r->scope_leaf[so] != s->sleaf)
		so++;
	if (!scope_partial(r, so))
		return e->strovf ? e->rest == s->rest : e->hash == s->hash;
	return (e->kovf >> so & 1) ? e->krest[so] == s->rest : e->khash[so] == s->hash;
}

static void
put_key(ColVerify *cv, SiftKey key)
{
	ResourceOwner step = CurrentResourceOwner;

	CurrentResourceOwner = cv->owner;
	tuplesort_putdatum(cv->keys, Int64GetDatum((int64) key), false);
	CurrentResourceOwner = step;
}

static TupleDesc
rows_desc(void)
{
	TupleDesc	desc = CreateTemplateTupleDesc(4);

	TupleDescInitEntry(desc, 1, "key", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 2, "sleaf", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 3, "skey", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 4, "hashes", BYTEAOID, -1, 0);
	sift_tupdesc_finalize(desc);
	return desc;
}

static Tuplesortstate *
rows_begin(ColVerify *cv)
{
	AttrNumber	att[] = {1, 2, 3};
	Oid			ops[] = {Int8LessOperator, Int4LessOperator, Int8LessOperator};
	Oid			coll[] = {InvalidOid, InvalidOid, InvalidOid};
	bool		nullsfirst[] = {false, false, false};
	TupleDesc	desc = rows_desc();

	cv->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	cv->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	cv->img = palloc(VARHDRSZ + sizeof(RowHash));
	SET_VARSIZE(cv->img, VARHDRSZ + sizeof(RowHash));
	return tuplesort_begin_heap(desc, 3, att, ops, coll, nullsfirst, maintenance_work_mem, NULL,
								cv->heapallindexed && cv->scoped ? TUPLESORT_RANDOMACCESS : TUPLESORT_NONE);
}

static void
put_row(ColVerify *cv, const RowHash *rh)
{
	ResourceOwner step = CurrentResourceOwner;

	memcpy(VARDATA(cv->img), rh, sizeof(RowHash));
	ExecClearTuple(cv->put);
	cv->put->tts_values[0] = Int64GetDatum((int64) rh->key);
	cv->put->tts_values[1] = Int32GetDatum(rh->sleaf);
	cv->put->tts_values[2] = Int64GetDatum(rh->skey);
	cv->put->tts_values[3] = PointerGetDatum(cv->img);
	memset(cv->put->tts_isnull, false, 4);
	ExecStoreVirtualTuple(cv->put);
	CurrentResourceOwner = cv->owner;
	tuplesort_puttupleslot(cv->rows, cv->put);
	CurrentResourceOwner = step;
}

static bool
next_row(ColVerify *cv, RowHash *rh)
{
	ResourceOwner step = CurrentResourceOwner;
	bool		more,
				isnull;

	CurrentResourceOwner = cv->owner;
	more = tuplesort_gettupleslot(cv->rows, true, false, cv->get, NULL);
	CurrentResourceOwner = step;
	if (more)
		memcpy(rh, VARDATA_ANY(DatumGetByteaPP(slot_getattr(cv->get, 4, &isnull))), sizeof(RowHash));
	return more;
}

static void
sort_rows(ColVerify *cv)
{
	ResourceOwner step = CurrentResourceOwner;

	if (cv->sorted)
		return;
	CurrentResourceOwner = cv->owner;
	tuplesort_performsort(cv->rows);
	CurrentResourceOwner = step;
	cv->sorted = true;
}

/* The rows of a decoded unit: each is checked on its own, its key goes to the duplicate sort, and its hashes are sorted when a scope segment or the heap is compared with them */
static void
collect_batch(ColVerify *cv, const SiftColBatch *b, bool scope)
{
	const SiftColRel *rel = cv->r;
	const SiftColSchema *sc = rel->schema;
	SiftColRow	row = {0};

	for (uint32 r_ = 0; r_ < b->nrows; r_++)
	{
		sift_cbatch_row(b, r_, &row);
		check_wide(cv->index, sc, &row);
		if (!scope)
			put_key(cv, b->keys[r_]);
		if (!scope && !cv->hashes)
			continue;
		if (scope)
		{
			int			leaf = scope_leaf_of(cv->index, sc, b, r_);
			int			s = 0;
			RowHash		rh;

			while (rel->scope_leaf[s] != leaf)
				s++;
			rh = row_hash(sc, &row, b->keys[r_], rel->scope_cols[s]);
			rh.sleaf = leaf;
			rh.skey = b->col[leaf].key[r_];
			put_row(cv, &rh);
			cv->nscope++;
		}
		else
		{
			RowHash		rh = entity_hash(rel, &row, b->keys[r_]);

			put_row(cv, &rh);
		}
	}
}

/* The entries of the scope segments in the order of (address, scope leaf, scope key) */
static int
scope_cmp(const void *a, const void *b)
{
	const RowHash *x = a,
			   *y = b;

	if (x->key != y->key)
		return SIFT_CMP3(x->key, y->key);
	if (x->sleaf != y->sleaf)
		return SIFT_CMP3(x->sleaf, y->sleaf);
	return SIFT_CMP3(x->skey, y->skey);
}

#ifdef SIFT_TESTING
bool		sift_test_col_verify_inval = false;
#endif

typedef struct HeapCheck
{
	Relation	index;
	SiftColRel *r;				/* a private copy: the cached state of the index is freed whenever its relcache entry is reloaded */
	ColVerify  *cv;
	MemoryContext tmp;
	int64		checked;
	BlockNumber winblk;			/* the block the rows of win are of: the heap scan goes by block, the sorted rows by address */
	RowHash    *win;
	int			nwin;
	int			capwin;
	RowHash		ahead;			/* the sorted row read past the window */
	bool		have_ahead;
	bool		eof;
} HeapCheck;

/* The rows of the entities of block blk: the sorted rows below it are not heap tuples the scan can still reach, those of scope segments are not compared with the heap */
static void
heap_window(HeapCheck *hc, BlockNumber blk)
{
	hc->nwin = 0;
	hc->winblk = blk;
	while (!hc->eof)
	{
		if (!hc->have_ahead)
		{
			hc->eof = !next_row(hc->cv, &hc->ahead);
			if (hc->eof)
				break;
			hc->have_ahead = true;
		}
		if (sift_key_block(hc->ahead.key) > blk)
			break;
		hc->have_ahead = false;
		if (sift_key_block(hc->ahead.key) < blk || hc->ahead.sleaf >= 0)
			continue;
		if (hc->nwin == hc->capwin)
		{
			hc->capwin = Max(64, hc->capwin * 2);
			hc->win = hc->win ? repalloc(hc->win, hc->capwin * sizeof(RowHash)) : MemoryContextAlloc(hc->cv->cxt, hc->capwin * sizeof(RowHash));
		}
		hc->win[hc->nwin++] = hc->ahead;
	}
}

static void
heap_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	HeapCheck  *hc = state;
	SiftColRow	row;
	RowHash		key = {sift_key_from_tid(tid)},
				heap;
	const RowHash *found;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

	if (!alive)
		return;
#ifdef SIFT_TESTING
	if (sift_test_col_verify_inval)
	{
		/* a session that updates the index's statistics sends this invalidation, which frees the cached state mid-scan */
		memset(sift_col_rel(hc->index), 0x7f, sizeof(SiftColRel));
		RelationCacheInvalidateEntry(RelationGetRelid(hc->index));
	}
#endif
	if (blk != hc->winblk)
	{
		SIFT_INVARIANT(!BlockNumberIsValid(hc->winblk) || blk > hc->winblk, "sift_col: the heap scan of a verification went back from block %u to %u", hc->winblk, blk);
		heap_window(hc, blk);
	}
	found = bsearch(&key, hc->win, hc->nwin, sizeof(RowHash), rowhash_cmp);
	if (found == NULL)
		corrupt(hc->index, "Heap tuple (%u,%u) is not in the index.", blk, ItemPointerGetOffsetNumber(tid));
	sift_col_make_row(hc->r, tid, values, isnull, &row, hc->tmp);
	heap = row_hash(hc->r->schema, &row, key.key, NULL);
	if (found->strovf ? found->rest != heap.rest : found->hash != heap.hash)
		corrupt(hc->index, "The values of heap tuple (%u,%u) differ from the index's.", blk, ItemPointerGetOffsetNumber(tid));
	if (found->known && found->tags != heap.tags)
		corrupt(hc->index, "The types of the overflow values of heap tuple (%u,%u) differ from the index's.", blk, ItemPointerGetOffsetNumber(tid));
	hc->checked++;
	MemoryContextReset(hc->tmp);
}


/* The lower bounds of the partitions are bounds and ascend strictly: the first need not be the empty one, a partition emptied by deletes may have been the first */
static void
step_part_bounds(void *arg)
{
	ColVerify  *cv = arg;
	SiftBound	prev,
				lo;

	for (int k = 0; k < cv->nparts; k++)
	{
		if (!sift_bound_unpack(cv->parts[k].lo, &lo))
			corrupt(cv->index, "The lower bound of the partition " UINT64_FORMAT " is no bound.", cv->parts[k].lineage);
		if (k > 0 && sift_bound_cmp(&prev, &lo) >= 0)
			corrupt(cv->index, "The bounds of partitions %d and %d do not ascend.", k - 1, k);
		prev = lo;
	}
}

static void
step_store(void *arg)
{
	ColVerify  *cv = arg;

	cv->r = sift_col_rel_private(cv->index, cv->cxt);
	cv->batch = sift_cbatch_create(cv->cxt, cv->r->schema);
	sift_store_verify(cv->index);
	cv->snap = sift_dir_read(cv->index);
	if (!(cv->snap->meta.flags & SIFT_COL_META_FLAG) || sift_dir_layout_differs(cv->snap, cv->r->schema->hash))
		corrupt(cv->index, "The metapage does not describe this column layout.");
	if (cv->snap->nchain > 0)
	{
		SiftSegEntry *segs;
		SiftDirChain chain;

		(void) sift_dirchain_read(cv->index, cv->snap->chain[0], cv->snap->version, 0, &segs, &chain);
		cv->nparts = sift_dirchain_parts(&chain, &cv->parts);
	}
}

/* The rows of a segment of a partition lie from its lower bound up to below the bound of the next partition: a segment that straddles two is read by neither's bound */
static void
verify_partition(ColVerify *cv, const SiftSegEntry *e, SiftColSegment *seg)
{
	int			at = (int) (e - cv->snap->segs);

	for (int k = 0; k < cv->nparts; k++)
	{
		const SiftDirPart *p = &cv->parts[k];
		SiftBound	lo,
					next;

		if (at < (int) p->first || at >= (int) (p->first + p->n))
			continue;
		if (!sift_bound_unpack(p->lo, &lo) || (k + 1 < cv->nparts && !sift_bound_unpack(cv->parts[k + 1].lo, &next)))
			corrupt(cv->index, "The lower bound of the partition " UINT64_FORMAT " is no bound.", p->lineage);
		if (!sift_colk_rows_within(seg, &lo, k + 1 < cv->nparts ? &next : NULL))
			corrupt(cv->index, "Segment %llu holds rows outside its partition " UINT64_FORMAT " of the key.", (unsigned long long) e->gen, p->lineage);
		return;
	}
}

static void
verify_immutable(ColVerify *cv, const SiftSegEntry *e)
{
	MemoryContext segcx = AllocSetContextCreate(cv->cxt, "sift_col verify segment", ALLOCSET_DEFAULT_SIZES);
	SiftColSegment *seg = sift_cseg_open(cv->index, e, segcx);
	uint64		live = sift_cseg_verify(seg);
	bool		scope = (e->flags & SIFT_SEG_SCOPE) != 0;

	if (live != sift_cseg_live_rows(seg))
		corrupt(cv->index, "Segment %llu holds %llu live rows, its liveness says %llu.", (unsigned long long) e->gen,
				(unsigned long long) live, (unsigned long long) sift_cseg_live_rows(seg));
	if (scope && !sift_cseg_keyed(e))
		corrupt(cv->index, "Scope segment %llu is not cut by key.", (unsigned long long) e->gen);
	cv->ndocs += scope ? 0 : e->ndocs;
	verify_partition(cv, e, seg);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		sift_cseg_decode_unit(seg, u, cv->batch);
		collect_batch(cv, cv->batch, scope);
	}
	sift_cseg_close(seg);
	MemoryContextDelete(segcx);
}

static void
verify_wbuf(ColVerify *cv, const SiftSegEntry *e)
{
	int			n;
	SiftColBatch **bs;
	MemoryContext segcx = AllocSetContextCreate(cv->cxt, "sift_col verify write buffer", ALLOCSET_DEFAULT_SIZES);

	sift_cwbuf_verify(cv->index, e, cv->r->schema);
	bs = sift_cwbuf_batches(cv->index, e, cv->r->schema, segcx, &n, NULL);
	for (int j = 0; j < n; j++)
		collect_batch(cv, bs[j], false);
	MemoryContextDelete(segcx);
}

/*
 * One segment. The maintenance lock is held for this segment alone, so that a promotion or merge waits for the length of one and not of the index: the
 * segments that go while the others are verified stay readable, the registered snapshot keeps their blocks from being reused (P4 of core/store.h), as for a scan.
 */
static void
step_segment(void *arg)
{
	ColVerify  *cv = arg;
	const SiftSegEntry *e = cv->seg;

	LockPage(cv->index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	if (e->kind == SIFT_SEG_IMMUTABLE)
		verify_immutable(cv, e);
	else if (e->kind != SIFT_SEG_TOMB)
		verify_wbuf(cv, e);
	UnlockPage(cv->index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
}

static void
step_totals(void *arg)
{
	ColVerify  *cv = arg;

	if (cv->ndocs != cv->snap->meta.ndocs)
		corrupt(cv->index, "Metapage totals %llu rows, segments %llu.", (unsigned long long) cv->snap->meta.ndocs, (unsigned long long) cv->ndocs);
}

static void
step_duplicates(void *arg)
{
	ColVerify  *cv = arg;
	Datum		key,
				prev = 0;
	bool		isnull,
				first = true;

	CurrentResourceOwner = cv->owner;
	tuplesort_performsort(cv->keys);
	while (tuplesort_getdatum(cv->keys, true, false, &key, &isnull, NULL))
	{
		if (!first && key == prev)
			corrupt(cv->index, "Row (%u,%u) is live in the index twice.", sift_key_block(DatumGetInt64(key)), (uint32) (DatumGetInt64(key) & 0xFFFF));
		prev = key;
		first = false;
	}
}

static void
step_scope(void *arg)
{
	ColVerify  *cv = arg;
	RowHash		e = {0},
				s,
				prev = {0};

	sort_rows(cv);
	while (next_row(cv, &s))
	{
		if (s.sleaf < 0)
		{
			e = s;
			continue;
		}
		if (prev.sleaf >= 0 && scope_cmp(&s, &prev) == 0)
			corrupt(cv->index, "Row (%u,%u) is live twice in the scope segments under the scope key " INT64_FORMAT ".", sift_key_block(s.key), (uint32) (s.key & 0xFFFF), s.skey);
		if (e.sleaf >= 0 || e.key != s.key)
			corrupt(cv->index, "Row (%u,%u) of the scope segments is not in the segments of the entities.", sift_key_block(s.key), (uint32) (s.key & 0xFFFF));
		if (!scope_same(cv->r, &e, &s))
			corrupt(cv->index, "The values of row (%u,%u) in the scope segments differ from those of the segments of the entities.", sift_key_block(s.key), (uint32) (s.key & 0xFFFF));
		prev = s;
	}
}

static void
step_heap(void *arg)
{
	ColVerify  *cv = arg;
	Relation	heap = table_open(cv->index->rd_index->indrelid, AccessShareLock);
	IndexInfo  *info = BuildIndexInfo(cv->index);
	HeapCheck	hc = {.index = cv->index,.r = cv->r,.cv = cv,.winblk = InvalidBlockNumber,
					  .tmp = AllocSetContextCreate(cv->cxt, "sift_col verify tuple", ALLOCSET_DEFAULT_SIZES)};

	sort_rows(cv);
	if (cv->nscope > 0)
	{
		ResourceOwner step = CurrentResourceOwner;

		CurrentResourceOwner = cv->owner;
		tuplesort_rescan(cv->rows);
		CurrentResourceOwner = step;
	}
	info->ii_Concurrent = true;
	table_index_build_scan(heap, cv->index, info, true, false, heap_cb, &hc, table_beginscan_strat(heap, cv->snapshot, 0, NULL, true, false));
	table_close(heap, AccessShareLock);
}

/* Whether the index has a scope segment: its entries are compared with the rows of the entities, which then keep their hashes */
static bool
has_scope_segments(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (sift_seg_is_scope(&snap->segs[i]))
			return true;
	return false;
}

static void
col_verify(SiftCheck *ck, Relation index, bool heapallindexed)
{
	ColVerify  *cv = palloc0(sizeof(ColVerify));
	MemoryContext old = CurrentMemoryContext;
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	bool		listed,
				sound;

	cv->owner = CurrentResourceOwner;
	cv->snapshot = snapshot;
	cv->index = index;
	cv->heapallindexed = heapallindexed;
	cv->cxt = AllocSetContextCreate(old, "sift_col verify", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(cv->cxt);
	SIFT_MEM_SCOPE(col_verify, cv->cxt);
	sift_errctx_push_diagnostic(&cv->errctx, RelationGetRelid(index));
	/* the snapshot is registered before the directory is read, and kept: the segments of the copy stay readable while they are verified one at a time */
	sound = listed = sift_check_step(ck, SIFT_CHECK_WHOLE, step_store, cv);
	if (listed)
	{
		cv->scoped = has_scope_segments(cv->snap);
		cv->hashes = heapallindexed || cv->scoped;
		cv->keys = tuplesort_begin_datum(INT8OID, Int8LessOperator, InvalidOid, false, maintenance_work_mem, NULL, TUPLESORT_NONE);
		if (cv->hashes)
			cv->rows = rows_begin(cv);
	}
	for (int i = 0; listed && i < cv->snap->nsegs; i++)
	{
		cv->seg = &cv->snap->segs[i];
		sift_errctx_segment(&cv->errctx, cv->seg->gen);
		sound &= sift_check_step(ck, cv->seg->gen, step_segment, cv);
		SIFT_MEM_CHECKPOINT();
	}
	sift_errctx_whole(&cv->errctx);
	if (sound)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_totals, cv);
	if (sound)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_part_bounds, cv);
	if (sound)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_duplicates, cv);
	if (sound && cv->nscope > 0)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_scope, cv);
	if (sound && heapallindexed)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_heap, cv);
	SIFT_MEM_CHECKPOINT();
	sift_errctx_pop(&cv->errctx);
	if (cv->keys)
		tuplesort_end(cv->keys);
	if (cv->rows)
		tuplesort_end(cv->rows);
	UnregisterSnapshot(snapshot);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cv->cxt);
}

Datum
sift_col_verify(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_inspect(PG_GETARG_OID(0), NULL);

	sift_check_maintain(index);
	col_verify(NULL, index, PG_GETARG_BOOL(1));
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}

void
sift_col_check(SiftCheck *ck, Oid indexoid, bool heapallindexed)
{
	Relation	index = sift_col_open_inspect(indexoid, NULL);

	sift_check_maintain(index);
	col_verify(ck, index, heapallindexed);
	index_close(index, AccessShareLock);
}
