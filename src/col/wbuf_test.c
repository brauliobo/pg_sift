/*
 * col/wbuf_test.c - test-only SQL functions sift._test_col_wbuf_* for the write buffer lanes of the column engine (col/wbuf.c),
 * compiled only with SIFT_TESTING and registered by pg_sift_test. They run on an index of the BM25 access method created on an empty
 * table, which has the metapage, directory and free list the store layer needs and no segments; sift._test_col_wbuf_prepare gives it
 * the schema hash of the test schema, then rows are appended, sealed, read back, deleted from and verified without the index AM.
 *
 * The test schema is an order with its line items (leaf, parent group):
 *   0 order_id INT   1 store_id INT   2 status STR   3 total F8   4 weight F4   5 items GROUP
 *   6 items.category STR   7 items.sku STR   8 items.qty INT   9 items.price F8
 * Row `id` is a function of (seed, id): random values, NULLs, overflow flags, strings up to 255 bytes, every 1000th row with thousands of
 * items (a record of many pages). Its TID is a bijection of id, so rows reach the lanes out of key order and spread over many superblocks.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "catalog/pg_type.h"
#include "common/pg_prng.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/wbuf.h"
#include "core/lane.h"

PG_FUNCTION_INFO_V1(sift_test_col_wbuf_prepare);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_append);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_torn);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_long);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_rows);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_batches);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_delete);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_seal);
PG_FUNCTION_INFO_V1(sift_test_col_wbuf_segments);

#define NCOLS		10
#define ID_BITS		24
#define ID_MASK		((UINT64CONST(1) << ID_BITS) - 1)
#define ID_MUL		UINT64CONST(0x9E3779B1)		/* odd: id -> position is a bijection of ID_BITS bits */
#define ULL(x)		((unsigned long long) (x))

static const SiftColSchema *
test_schema(void)
{
	static SiftColSchema *schema = NULL;
	static const struct
	{
		const char *path;
		int16		parent;
		uint8		kind;
		uint8		store;
		uint8		type;
	}			leaves[NCOLS] = {
		{"order_id", -1, SIFT_COLK_SCALAR, SIFT_COLS_INT, SIFT_COLT_INT8},
		{"store_id", -1, SIFT_COLK_SCALAR, SIFT_COLS_INT, SIFT_COLT_INT4},
		{"status", -1, SIFT_COLK_SCALAR, SIFT_COLS_STR, SIFT_COLT_TEXT},
		{"total", -1, SIFT_COLK_SCALAR, SIFT_COLS_F8, SIFT_COLT_FLOAT8},
		{"weight", -1, SIFT_COLK_SCALAR, SIFT_COLS_F4, SIFT_COLT_FLOAT4},
		{"items", -1, SIFT_COLK_GROUP, SIFT_COLS_INT, SIFT_COLT_INT4},
		{"items[].category", 5, SIFT_COLK_SCALAR, SIFT_COLS_STR, SIFT_COLT_TEXT},
		{"items[].sku", 5, SIFT_COLK_SCALAR, SIFT_COLS_STR, SIFT_COLT_TEXT},
		{"items[].qty", 5, SIFT_COLK_SCALAR, SIFT_COLS_INT, SIFT_COLT_INT4},
		{"items[].price", 5, SIFT_COLK_SCALAR, SIFT_COLS_F8, SIFT_COLT_FLOAT8},
	};

	if (schema == NULL)
	{
		SiftColDef	defs[NCOLS] = {0};

		for (int i = 0; i < NCOLS; i++)
			defs[i] = (SiftColDef) {i, leaves[i].parent, leaves[i].kind, leaves[i].store, 0, i + 1, leaves[i].type, leaves[i].path};
		schema = sift_cschema_create(TopMemoryContext, defs, NCOLS);
	}
	return schema;
}

static ItemPointerData
tid_of(uint64 id)
{
	uint64		pos = (id * ID_MUL) & ID_MASK;
	ItemPointerData tid;

	ItemPointerSet(&tid, pos >> 7, (pos & 127) + 1);
	return tid;
}

/* The inverse of tid_of: multiplication by the inverse of ID_MUL modulo 2^ID_BITS (Newton's iteration) */
static uint64
id_of(const ItemPointerData *tid)
{
	uint64		pos = ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 7) | (ItemPointerGetOffsetNumberNoCheck(tid) - 1),
				inv = ID_MUL;

	for (int i = 0; i < 5; i++)
		inv *= 2 - ID_MUL * inv;
	return (pos * inv) & ID_MASK;
}

typedef struct TestRow
{
	SiftColRow	row;
	SiftColValue val[NCOLS];
	SiftColValue *elem[NCOLS];
} TestRow;

static void
gen_str(pg_prng_state *st, SiftColValue *v, bool vocab)
{
	static const char *const words[] = {"open", "paid", "shipped", "returned", "grocery", "toys", "garden"};
	uint64		r = pg_prng_uint64_range(st, 0, 99);

	if (vocab && r < 80)
	{
		const char *w = words[pg_prng_uint64_range(st, 0, lengthof(words) - 1)];

		v->v.s = w;
		v->len = strlen(w);
		return;
	}
	v->len = r < 2 ? SIFT_COL_DICT_MAX_ENTRY : r < 4 ? pg_prng_uint64_range(st, 256, 20000) : r < 10 ? 0 : pg_prng_uint64_range(st, 1, 24);
	v->v.s = palloc(v->len + 1);
	for (uint32 i = 0; i < v->len; i++)
		((char *) v->v.s)[i] = pg_prng_uint64_range(st, 0, 255);
}

static void
gen_double(pg_prng_state *st, double *f)
{
	switch (pg_prng_uint64_range(st, 0, 19))
	{
		case 0:
			*f = get_float8_nan();
			break;
		case 1:
			*f = -0.0;
			break;
		case 2:
			*f = pg_prng_bool(st) ? get_float8_infinity() : -get_float8_infinity();
			break;
		default:
			*f = (pg_prng_double(st) - 0.5) * 1e6;
	}
}

static void
gen_int(pg_prng_state *st, int64 *i)
{
	switch (pg_prng_uint64_range(st, 0, 19))
	{
		case 0:
			*i = PG_INT64_MIN;
			break;
		case 1:
			*i = PG_INT64_MAX;
			break;
		case 2:
		case 3:
			*i = (int64) pg_prng_uint64(st);
			break;
		default:
			*i = (int64) pg_prng_uint64_range(st, 0, 2000) - 1000;
	}
}

/* A value of a leaf: NULL 8%, an overflow 2% */
static void
gen_value(pg_prng_state *st, const SiftColDef *def, SiftColValue *v)
{
	uint64		r = pg_prng_uint64_range(st, 0, 99);

	*v = (SiftColValue) {0};
	if (r < 10)
	{
		v->isnull = true;
		v->overflow = r < 2;
		return;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			gen_int(st, &v->v.i);
			break;
		case SIFT_COLS_F4:
			{
				double		f;

				gen_double(st, &f);
				v->v.f = (float4) f;
				break;
			}
		case SIFT_COLS_F8:
			gen_double(st, &v->v.f);
			break;
		case SIFT_COLS_STR:
			gen_str(st, v, def->id == 2 || def->id == 6);
			break;
	}
}

/* Row id of the seed, in CurrentMemoryContext */
static void
gen_row(uint64 seed, uint64 id, TestRow *t)
{
	const SiftColSchema *s = test_schema();
	pg_prng_state st;
	SiftColValue *items = &t->val[5];

	pg_prng_seed(&st, seed * UINT64CONST(0x9E3779B97F4A7C15) + id);
	memset(t, 0, sizeof(*t));
	t->row = (SiftColRow) {tid_of(id), NCOLS, t->val, t->elem};
	for (int c = 1; c < NCOLS; c++)
		if (s->cols[c].parent < 0 && c != 5)
			gen_value(&st, &s->cols[c], &t->val[c]);
	t->val[0].v.i = id;
	*items = (SiftColValue) {0};
	if (id % 1000 == 999)
		items->v.i = 600 + pg_prng_uint64_range(&st, 0, 2400);
	else if (pg_prng_uint64_range(&st, 0, 9) == 0)
	{
		items->isnull = true;
		items->overflow = pg_prng_bool(&st);
	}
	else
		items->v.i = pg_prng_uint64_range(&st, 0, 5);
	for (int c = 6; c < NCOLS && !items->isnull; c++)
	{
		t->elem[c] = palloc(Max(items->v.i, 1) * sizeof(SiftColValue));
		for (int64 j = 0; j < items->v.i; j++)
			gen_value(&st, &s->cols[c], &t->elem[c][j]);
	}
}

static void
differs(const char *what, uint64 id, int leaf, int64 j)
{
	elog(ERROR, "row %llu differs from its generator: %s of leaf %d element %lld", ULL(id), what, leaf, (long long) j);
}

static void
same_value(const SiftColDef *def, const SiftColValue *got, const SiftColValue *want, uint64 id, int64 j, bool exact)
{
	if (got->isnull != want->isnull || got->overflow != want->overflow)
		differs("null or overflow flag", id, def->id, j);
	if (want->isnull)
		return;
	if (def->kind == SIFT_COLK_GROUP || def->store == SIFT_COLS_INT)
	{
		if (got->v.i != want->v.i)
			differs("integer", id, def->id, j);
	}
	else if (def->store == SIFT_COLS_STR)
	{
		if (got->len != want->len || memcmp(got->v.s, want->v.s, want->len) != 0)
			differs("string", id, def->id, j);
	}
	else if (exact ? memcmp(&got->v.f, &want->v.f, sizeof(double)) != 0 : sift_col_f8_key(got->v.f) != sift_col_f8_key(want->v.f))
		differs("float", id, def->id, j);
}

/*
 * A row read back against its generator: the TID, every value, every element; returns its id. A float read from a batch is the one of its
 * order key (-0 is +0, every NaN one NaN), a record keeps its bits.
 */
static uint64
check_row(uint64 seed, const SiftColRow *got, bool exact)
{
	static MemoryContext check_cxt = NULL;
	const SiftColSchema *s = test_schema();
	uint64		id = got->val[0].v.i;
	MemoryContext old;
	TestRow    *want;

	if (check_cxt == NULL)
		check_cxt = AllocSetContextCreate(TopMemoryContext, "test row check", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(check_cxt);
	old = MemoryContextSwitchTo(check_cxt);
	want = palloc(sizeof(TestRow));
	gen_row(seed, id, want);
	if (got->ncols != NCOLS || !ItemPointerEquals((ItemPointer) &got->tid, &want->row.tid))
		differs("tid", id, 0, 0);
	for (int c = 0; c < NCOLS; c++)
	{
		const SiftColDef *def = &s->cols[c];
		const SiftColValue *parent = def->parent < 0 ? NULL : &want->val[def->parent];

		if (parent == NULL)
			same_value(def, &got->val[c], &want->val[c], id, 0, exact);
		else
			for (int64 j = 0; !parent->isnull && j < parent->v.i; j++)
				same_value(def, &got->elem[c][j], &want->elem[c][j], id, j, exact);
	}
	MemoryContextSwitchTo(old);
	return id;
}

static const char *
kind_name(SiftSegKind kind)
{
	return kind == SIFT_SEG_WBUF_OPEN ? "active" : kind == SIFT_SEG_WBUF_SEALED ? "sealed" : "immutable";
}

/* The write buffers of the directory now, in directory order */
static SiftDirSnapshot *
read_dir(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	sift_dir_read(index, snap);
	return snap;
}

static bool
is_wbuf(const SiftSegEntry *e)
{
	return e->kind == SIFT_SEG_WBUF_OPEN || e->kind == SIFT_SEG_WBUF_SEALED;
}

static void
set_hash(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->analyzer_hash = *(uint32 *) arg;
}

/* (index regclass, hash_xor int4 DEFAULT 0): the index now carries the test schema's hash (xor hash_xor: a stale definition) */
Datum
sift_test_col_wbuf_prepare(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint32		hash = test_schema()->hash ^ (uint32) PG_GETARG_INT32(1);

	sift_dir_update(index, &(SiftDirChange) {.fn = set_hash,.arg = &hash,.extra = InvalidBuffer});
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(test_schema()->hash);
}

/* (kb int4): the write buffer limit the appends use */
/*
 * Locks the tails of up to six lanes from this backend's own, so that the next append has to take another lane; refreshes the hint's tails.
 * Sessions doing this at once would wait for each other's tails.
 */
static int
lock_busy(Relation index, SiftCWbufHint *hint, uint64 rnd, Buffer *bufs)
{
	SiftDirSnapshot *snap = read_dir(index);
	int			n = 0;

	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftWbufHeaderData hdr;

		if (snap->segs[i].kind != SIFT_SEG_WBUF_OPEN)
			continue;
		sift_lane_read_header(index, &snap->segs[i], &hdr);
		if (hint->gen != snap->segs[i].gen)
			hint->gen = 0;
		for (int l = 0; l < SIFT_LANES; l++)
			hint->tail[l] = hdr.lanes[l].tail;
		for (int k = 0; hint->gen != 0 && k < rnd % 7; k++)
		{
			bufs[n] = sift_lane_read_locked(index, hdr.lanes[((uint32) MyProcNumber + k) % SIFT_LANES].tail, BUFFER_LOCK_EXCLUSIVE);
			n++;
		}
	}
	pfree(snap);
	return n;
}

/*
 * (index regclass, seed int8, first int4, n int4, busy bool DEFAULT false): appends rows first .. first + n - 1, with the backend's hint kept
 * across them; busy makes most appends meet locked lane tails. Returns how many appends reported the segment as full.
 */
Datum
sift_test_col_wbuf_append(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint64		seed = PG_GETARG_INT64(1);
	int64		first = PG_GETARG_INT32(2),
				n = PG_GETARG_INT32(3);
	bool		busy = PG_GETARG_BOOL(4);
	SiftCWbufHint hint = {0};
	MemoryContext old = CurrentMemoryContext,
				rowcxt = AllocSetContextCreate(CurrentMemoryContext, "test row", ALLOCSET_DEFAULT_SIZES);
	int64		crossed = 0;

	for (int64 id = first; id < first + n; id++)
	{
		Buffer		bufs[7];
		int			nbusy = 0;
		TestRow    *t;

		MemoryContextReset(rowcxt);
		MemoryContextSwitchTo(rowcxt);
		t = palloc(sizeof(TestRow));
		gen_row(seed, id, t);
		if (busy)
			nbusy = lock_busy(index, &hint, pg_prng_uint64(&pg_global_prng_state), bufs);
		crossed += sift_cwbuf_append(index, &hint, test_schema(), &t->row);
		for (int k = 0; k < nbusy; k++)
			UnlockReleaseBuffer(bufs[k]);
		MemoryContextSwitchTo(old);
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextDelete(rowcxt);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(crossed);
}

/* (index regclass, seed int8, id int4, fail_after int4): a big row whose append fails after fail_after page links, leaving it torn */
Datum
sift_test_col_wbuf_torn(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	SiftCWbufHint hint = {0};
	TestRow    *t = palloc(sizeof(TestRow));
	int64		id = PG_GETARG_INT32(2);

	if (id % 1000 != 999)
		elog(ERROR, "only the rows with id %% 1000 = 999 are big enough to be torn");
	gen_row(PG_GETARG_INT64(1), id, t);
	sift_cwbuf_fail_grow = PG_GETARG_INT32(3);
	sift_cwbuf_append(index, &hint, test_schema(), &t->row);
	sift_cwbuf_fail_grow = 0;
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

/* (index regclass): appends a row with a 300 000 byte string: its record spans several pages */
Datum
sift_test_col_wbuf_long(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	SiftCWbufHint hint = {0};
	TestRow    *t = palloc(sizeof(TestRow));

	gen_row(1, 1, t);
	t->val[2] = (SiftColValue) {.v.s = palloc0(300000),.len = 300000};
	sift_cwbuf_append(index, &hint, test_schema(), &t->row);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

static void
put3(ReturnSetInfo *rsinfo, Datum a, Datum b, Datum c)
{
	Datum		values[3] = {a, b, c};
	bool		nulls[3] = {0};

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* (index regclass, seed int8) -> (gen int8, id int8, deleted bool): every record of the write buffers through the reader, each checked against its generator */
Datum
sift_test_col_wbuf_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	uint64		seed = PG_GETARG_INT64(1);
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = read_dir(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftCWbufReader *r;
		SiftColRow	row;
		bool		deleted;

		if (!is_wbuf(&snap->segs[i]))
			continue;
		r = sift_cwbuf_open(index, &snap->segs[i], test_schema(), CurrentMemoryContext);
		while (sift_cwbuf_next(r, &row, &deleted))
			put3(rsinfo, Int64GetDatum(snap->segs[i].gen), Int64GetDatum(check_row(seed, &row, true)), BoolGetDatum(deleted));
		sift_cwbuf_close(r);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

/*
 * (index regclass, seed int8) -> (gen int8, sb int8, id int8): the live rows of every write buffer as batches. The batches ascend by
 * superblock, their keys ascend and belong to it, and every row is checked against its generator.
 */
Datum
sift_test_col_wbuf_batches(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	uint64		seed = PG_GETARG_INT64(1);
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = read_dir(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		int			nb;
		SiftColBatch **batches;
		SiftColRow	row = {0};

		if (!is_wbuf(&snap->segs[i]))
			continue;
		batches = sift_cwbuf_batches(index, &snap->segs[i], test_schema(), CurrentMemoryContext, &nb);
		for (int k = 0; k < nb; k++)
		{
			SiftColBatch *b = batches[k];

			if (k > 0 && b->sb <= batches[k - 1]->sb)
				elog(ERROR, "batches of segment %llu do not ascend by superblock", ULL(snap->segs[i].gen));
			for (uint32 x = 0; x < b->nrows; x++)
			{
				if (x > 0 && b->keys[x] <= b->keys[x - 1])
					elog(ERROR, "keys of a batch of segment %llu do not ascend", ULL(snap->segs[i].gen));
				if (sift_block_sb(sift_key_block(b->keys[x])) != b->sb)
					elog(ERROR, "a key of superblock %u in a batch of superblock %u", sift_block_sb(sift_key_block(b->keys[x])), b->sb);
				sift_cbatch_row(b, x, &row);
				put3(rsinfo, Int64GetDatum(snap->segs[i].gen), Int64GetDatum(b->sb), Int64GetDatum(check_row(seed, &row, false)));
			}
		}
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

typedef struct DeleteState
{
	int32		modulus;
} DeleteState;

static bool
dead_every(ItemPointer tid, void *state)
{
	return id_of(tid) % ((DeleteState *) state)->modulus == 0;
}

/* (index regclass, modulus int4): VACUUM's pass over the write buffers; the rows with id % modulus = 0 are dead. Returns the rows flagged. */
Datum
sift_test_col_wbuf_delete(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	DeleteState state = {PG_GETARG_INT32(1)};
	IndexBulkDeleteResult stats = {0};
	SiftDirSnapshot *snap;
	int64		removed = 0;

	LockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
	snap = read_dir(index);
	for (int i = 0; i < snap->nsegs; i++)
		if (is_wbuf(&snap->segs[i]))
			removed += sift_cwbuf_bulkdelete(index, &snap->segs[i], test_schema(), dead_every, &state, &stats);
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(removed);
}

/* (index regclass, force bool): sift_cwbuf_seal; the sealed generation or 0 */
Datum
sift_test_col_wbuf_seal(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint64		gen = sift_cwbuf_seal(index, PG_GETARG_BOOL(1));

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(gen);
}

/* The lanes of the segment that hold at least one record */
static int
lanes_used(Relation index, const SiftSegEntry *e)
{
	SiftWbufHeaderData hdr;
	int			used = 0;

	sift_lane_read_header(index, e, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		bool		any = false;

		for (BlockNumber blk = hdr.lanes[l].head; BlockNumberIsValid(blk);)
		{
			Buffer		buf = sift_lane_read_locked(index, blk, BUFFER_LOCK_SHARE);

			any |= SiftLaneHead(BufferGetPage(buf))->nrecs > 0;
			blk = SiftLaneHead(BufferGetPage(buf))->next;
			UnlockReleaseBuffer(buf);
		}
		used += any;
	}
	return used;
}

/*
 * (index regclass) -> (gen, kind, nrows, npages, live, extents, lanes): the write buffers; live is sift_cwbuf_verify's, extents the blocks
 * sift_cwbuf_pages reports, lanes the lanes holding records
 */
Datum
sift_test_col_wbuf_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = read_dir(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		uint64		nrows,
					npages;
		int			next;
		SiftExtent *ext;
		int64		blocks = 0;
		Datum		values[7];
		bool		nulls[7] = {0};

		if (!is_wbuf(e))
			continue;
		sift_cwbuf_summary(index, e, &nrows, &npages);
		ext = sift_cwbuf_pages(index, e, &next);
		for (int k = 0; k < next; k++)
			blocks += ext[k].nblocks;
		values[0] = Int64GetDatum(e->gen);
		values[1] = CStringGetTextDatum(kind_name(e->kind));
		values[2] = Int64GetDatum(nrows);
		values[3] = Int64GetDatum(npages);
		values[4] = Int64GetDatum(sift_cwbuf_verify(index, e, test_schema()));
		values[5] = Int64GetDatum(blocks);
		values[6] = Int32GetDatum(lanes_used(index, e));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
