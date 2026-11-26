/*
 * col/f2/store/store.c - the Format 2.0 relation descriptor of an index or a table: one cache entry per relation, made by the host's loader, and the one conversion of a row's datums
 * to cells and key.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "storage/sinval.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/store/store.h"
#include "col/f2/store/schema.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

typedef struct StoreEntry
{
	Oid			relid;
	SiftF2Store *store;
	uint64		tick;
	dlist_node	lru_node;		/* in store_lru, the entry used longest ago first: the ticks of the list ascend */
	uint32		relhash;		/* RELOID hash value of the relation and ATTNUM hash value of each attribute (atthash[a - 1]), taken with the store: a callback runs outside a transaction */
	uint32	   *atthash;
} StoreEntry;

static HTAB *store_cache = NULL;
static dlist_head store_lru = DLIST_STATIC_INIT(store_lru);
static bool store_cache_registered = false;
static uint64 store_invalidations = 0;	/* what a build that reads the catalogs finds out has changed under it */

#define MAX_INVALIDATORS	5

static SiftF2Invalidator invalidators[MAX_INVALIDATORS];
static int	ninvalidators = 0;

static void
relation_invalidated(Datum arg pg_attribute_unused(), int cacheid pg_attribute_unused(), uint32 hashvalue)
{
	for (int i = 0; i < ninvalidators; i++)
		invalidators[i] (hashvalue);
}

void
sift_f2_invalidator_register(SiftF2Invalidator fn)
{
	for (int i = 0; i < ninvalidators; i++)
		if (invalidators[i] == fn)
			return;
	SIFT_INVARIANT(ninvalidators < MAX_INVALIDATORS, "more than %d Format 2.0 cache invalidators", MAX_INVALIDATORS);
	if (ninvalidators == 0)
		CacheRegisterSyscacheCallback(RELOID, relation_invalidated, (Datum) 0);
	invalidators[ninvalidators++] = fn;
}

uint32
sift_f2_rel_hash(Relation rel)
{
	return GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
}

void
sift_f2_invalidate_now(Relation rel)
{
	SharedInvalidationMessage msg = {.cc = {.id = RELOID,.dbId = MyDatabaseId,.hashValue = sift_f2_rel_hash(rel)}};

	relation_invalidated((Datum) 0, RELOID, msg.cc.hashValue);
	SendSharedInvalidMessages(&msg, 1);
}

bool
sift_f2_inval_hits(uint32 relhash, uint32 hashvalue)
{
	return hashvalue == 0 || relhash == hashvalue;
}

/* A store a statement of the transaction may hold leaves the cache and lives to the end of the transaction; any other is deleted */
static void
drop_entry(StoreEntry *e)
{
	dlist_delete(&e->lru_node);
	if (sift_cache_used_in_xact(e->tick))
		MemoryContextSetParent(e->store->cx, TopTransactionContext);
	else
		MemoryContextDelete(e->store->cx);
	hash_search(store_cache, &e->relid, HASH_REMOVE, NULL);
}

static void
store_invalidate(uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	StoreEntry *e;

	store_invalidations++;
	if (store_cache == NULL)
		return;
	hash_seq_init(&seq, store_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (sift_f2_inval_hits(e->relhash, hashvalue))
			drop_entry(e);
}

/* DROP COLUMN and ALTER COLUMN TYPE change pg_attribute rows and leave the pg_class row alone: the store follows the descriptor, so any attribute row of its relation drops it */
static void
store_attr_invalidate(Datum arg pg_attribute_unused(), int cacheid pg_attribute_unused(), uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	StoreEntry *e;

	store_invalidations++;
	if (store_cache == NULL)
		return;
	hash_seq_init(&seq, store_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		bool		hit = hashvalue == 0;

		for (int a = 0; !hit && a < e->store->natts; a++)
			hit = e->atthash[a] == hashvalue;
		if (hit)
			drop_entry(e);
	}
}

/* CREATE INDEX and DROP INDEX on a table that has indexes already change no pg_class row and no pg_attribute row of it, but the indexes that block a column-delta update are part of the store */
static void
store_index_invalidate(Datum arg pg_attribute_unused(), int cacheid pg_attribute_unused(), uint32 hashvalue pg_attribute_unused())
{
	HASH_SEQ_STATUS seq;
	StoreEntry *e;

	store_invalidations++;
	if (store_cache == NULL)
		return;
	hash_seq_init(&seq, store_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		drop_entry(e);
}

/* the entry used longest ago, when no statement of the transaction may hold it: the ticks of the list ascend, so that is the first of it or none */
static StoreEntry *
store_victim(void)
{
	StoreEntry *first;

	if (store_cache == NULL || dlist_is_empty(&store_lru))
		return NULL;
	first = dlist_container(StoreEntry, lru_node, dlist_head_node(&store_lru));
	return sift_cache_used_in_xact(first->tick) ? NULL : first;
}

static uint64
store_oldest(void)
{
	StoreEntry *victim = store_victim();

	return victim != NULL ? victim->tick : UINT64_MAX;
}

static bool
store_evict_one(void)
{
	StoreEntry *victim = store_victim();

	if (victim == NULL)
		return false;
	drop_entry(victim);
	return true;
}

static void
store_evict_all(void)
{
	while (store_evict_one())
		;
}

static const SiftCacheKind store_cache_kind = {
	.name = "f2_store_cache",
	.oldest = store_oldest,
	.evict_one = store_evict_one,
	.evict_all = store_evict_all,
};

static void
store_cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(StoreEntry),.hcxt = sift_cache_create_context("sift cache f2 stores")};

	/* MEM[MEM-IDLE-12]: cachebudget f2 stores HTAB (16 init entries; evictable LRU) */
	store_cache = hash_create("sift cache f2 stores", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	if (!store_cache_registered)
	{
		sift_f2_invalidator_register(store_invalidate);
		CacheRegisterSyscacheCallback(ATTNUM, store_attr_invalidate, (Datum) 0);
		CacheRegisterSyscacheCallback(INDEXRELID, store_index_invalidate, (Datum) 0);
		sift_cache_register(&store_cache_kind);
		store_cache_registered = true;
	}
}

static const SiftF2HostOps *
host_of(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX ? &sift_col_host_ops : &sift_tbl_host_ops;
}

/* In the context of the caller until the store is whole and adopted: a build that fails leaves nothing in the cache budget */
static SiftF2Store *
store_build(Relation rel)
{
	/* MEM[MEM-IDLE-03]: cachebudget f2 store per-rel ctx (struct + shared-nothing option copies; evictable LRU; committed by sift_f2_store_open callers) */
	MemoryContext cx = sift_cache_create_context_in(CurrentMemoryContext, "sift cache f2 store");
	SiftF2Store *s = MemoryContextAllocZero(cx, sizeof(SiftF2Store));

	s->relid = RelationGetRelid(rel);
	s->locator = rel->rd_locator;
	s->host = host_of(rel);
	s->cx = cx;
	/* MEM[MEM-IDLE-04]: cachebudget f2 store row_cx (table DML only: an index store has none, 0 B idle; 1 KB min for a table) */
	if (s->host->host_kind != SIFT_F2_HOST_INDEX)
		s->row_cx = AllocSetContextCreateInternal(cx, "sift cache f2 row", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	s->natts = RelationGetDescr(rel)->natts;
	s->host->load_options(rel, s, cx);
	return s;
}

/* The catalogs the build reads may change under it: a store made across an invalidation is made again */
static SiftF2Store *
store_build_current(Relation rel)
{
	for (;;)
	{
		uint64		seen = store_invalidations;
		SiftF2Store *s = store_build(rel);

		if (seen == store_invalidations)
			return s;
		MemoryContextDelete(s->cx);
	}
}

static uint32 *
store_att_hashes(const SiftF2Store *s)
{
	uint32	   *h = MemoryContextAlloc(s->cx, sizeof(uint32) * Max(s->natts, 1));

	for (int a = 0; a < s->natts; a++)
		h[a] = GetSysCacheHashValue2(ATTNUM, ObjectIdGetDatum(s->relid), Int16GetDatum(a + 1));
	return h;
}

SiftF2Store *
sift_f2_store_open(Relation rel)
{
	StoreEntry *e;
	Oid			relid = RelationGetRelid(rel);

	if (store_cache == NULL)
		store_cache_init();
	e = hash_search(store_cache, &relid, HASH_FIND, NULL);
	if (e == NULL)
	{
		SiftF2Store *s = store_build_current(rel);

		e = hash_search(store_cache, &relid, HASH_ENTER, NULL);
		e->store = s;
		e->relhash = sift_f2_rel_hash(rel);
		e->atthash = store_att_hashes(s);
		sift_cache_adopt(s->cx);
		dlist_push_tail(&store_lru, &e->lru_node);
	}
	else
		dlist_move_tail(&store_lru, &e->lru_node);
	e->tick = sift_cache_tick();
	return e->store;
}

void
sift_f2_store_invalidate(Relation rel)
{
	Relation	classrel = table_open(RelationRelationId, AccessShareLock);
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));

	if (HeapTupleIsValid(tup))
	{
		CacheInvalidateHeapTuple(classrel, tup, NULL);
		ReleaseSysCache(tup);
	}
	table_close(classrel, AccessShareLock);
}

SiftF2Store *
sift_f2_store_cached(Oid relid)
{
	StoreEntry *e = store_cache == NULL ? NULL : hash_search(store_cache, &relid, HASH_FIND, NULL);

	return e == NULL ? NULL : e->store;
}

SiftF2Store *
sift_f2_store_from_oid(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	SiftF2Store *s = sift_f2_store_open(rel);

	relation_close(rel, NoLock);
	return s;
}

/* A value over the wide threshold leaves the granules: its cell is null and tagged, and the varlena image goes to the extents of the table */
static void
wide_cell(SiftCell *c, const char *image, uint32 image_len)
{
	c->isnull = true;
	c->tag = SIFT_F2_TAG_WIDE_RAW;
	c->xp = image;
	c->xplen = image_len;
	c->s = NULL;
	c->slen = 0;
}

Datum
sift_f2_bare_datum_of(Oid typid, const char *bytes, uint32 len, MemoryContext cx)
{
	struct varlena *v;

	if (typid == NAMEOID)
		return NameGetDatum(memcpy(MemoryContextAllocZero(cx, NAMEDATALEN), bytes, len));
	if (len < VARATT_SHORT_MAX)
	{
		v = MemoryContextAlloc(cx, len + 1);
		SET_VARSIZE_SHORT(v, len + 1);
		memcpy(VARDATA_SHORT(v), bytes, len);
	}
	else
	{
		v = MemoryContextAlloc(cx, len + VARHDRSZ);
		SET_VARSIZE(v, len + VARHDRSZ);
		memcpy(VARDATA(v), bytes, len);
	}
	return PointerGetDatum(v);
}

Datum
sift_f2_bare_datum(Form_pg_attribute att, const char *bytes, uint32 len, MemoryContext cx)
{
	return sift_f2_bare_datum_of(att->atttypid, bytes, len, cx);
}

/* How the datum of an attribute becomes its cell, decided once from the attribute and the store and not for each value */
typedef enum PackKind
{
	PACK_NUM64,
	PACK_BARE_NAME,
	PACK_BARE_VARLENA,
	PACK_FLOAT4,
	PACK_FLOAT8,
	PACK_INT64,
	PACK_INT32,
	PACK_INT16,
	PACK_INT8,
	PACK_FIXED,
	PACK_CSTRING,
	PACK_VARLENA
} PackKind;

struct SiftF2Packer
{
	PackKind	kind;
	uint8		scale;
	int16		len;
	int			attr;			/* the index of the attribute in the values of a row */
	int			leaf;
};

static PackKind
pack_kind(const SiftF2Store *store, Form_pg_attribute att, uint8 *scale)
{
	if (sift_f2_attr_num64(att, scale))
		return PACK_NUM64;
	if (store->headerless && sift_f2_attr_bare_bytes(att))
		return att->atttypid == NAMEOID ? PACK_BARE_NAME : PACK_BARE_VARLENA;
	if (att->attbyval)
		return att->atttypid == FLOAT4OID ? PACK_FLOAT4 : att->atttypid == FLOAT8OID ? PACK_FLOAT8 :
			att->attlen == 8 ? PACK_INT64 : att->attlen == 4 ? PACK_INT32 : att->attlen == 2 ? PACK_INT16 : PACK_INT8;
	return att->attlen > 0 ? PACK_FIXED : att->attlen == -2 ? PACK_CSTRING : PACK_VARLENA;
}

static void
pack_cell(const struct SiftF2Packer *k, Datum d, MemoryContext row_cx, bool spill, SiftCell *c)
{
	MemoryContext old;
	struct varlena *v;

	switch (k->kind)
	{
		case PACK_NUM64:
			sift_f2_num64_cell(d, k->scale, row_cx, c);
			return;
		case PACK_BARE_NAME:
			c->s = NameStr(*DatumGetName(d));
			c->slen = strnlen(c->s, NAMEDATALEN);
			return;
		case PACK_BARE_VARLENA:
			old = MemoryContextSwitchTo(row_cx);
			v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
			MemoryContextSwitchTo(old);
			c->s = VARDATA_ANY(v);
			c->slen = VARSIZE_ANY_EXHDR(v);
			if (spill && c->slen > SIFT_F2_WIDE_THRESHOLD)
				wide_cell(c, (const char *) v, VARSIZE_ANY(v));
			return;
		case PACK_FLOAT4:
			c->f = DatumGetFloat4(d);
			return;
		case PACK_FLOAT8:
			c->f = DatumGetFloat8(d);
			return;
		case PACK_INT64:
			c->i = DatumGetInt64(d);
			return;
		case PACK_INT32:
			c->i = DatumGetInt32(d);
			return;
		case PACK_INT16:
			c->i = DatumGetInt16(d);
			return;
		case PACK_INT8:
			c->i = (int8) DatumGetChar(d);
			return;
		case PACK_FIXED:
			c->s = DatumGetPointer(d);
			c->slen = k->len;
			return;
		case PACK_CSTRING:
			c->s = DatumGetCString(d);
			c->slen = strlen(c->s) + 1;
			return;
		case PACK_VARLENA:
			old = MemoryContextSwitchTo(row_cx);
			c->s = (const char *) PG_DETOAST_DATUM(d);
			MemoryContextSwitchTo(old);
			c->slen = VARSIZE_ANY(c->s);
			if (spill && c->slen - VARHDRSZ > SIFT_F2_WIDE_THRESHOLD)
				wide_cell(c, c->s, c->slen);
			return;
	}
}

void
sift_f2_cell_of_datum(const SiftF2Store *store, Form_pg_attribute att, Datum d, MemoryContext row_cx, bool spill, SiftCell *c)
{
	struct SiftF2Packer k = {.len = att->attlen};

	k.kind = pack_kind(store, att, &k.scale);
	pack_cell(&k, d, row_cx, spill, c);
}

/* The packer of each attribute that has a leaf, by attribute: made with the first row packed, in the context of the store, which goes with the descriptor it was made from */
static void
packers_build(SiftF2Store *store, TupleDesc desc)
{
	struct SiftF2Packer *k = MemoryContextAlloc(store->cx, sizeof(*k) * Max(store->natts, 1));
	int			n = 0;

	for (int a = 1; a <= store->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		if (store->att_first[a] < 0)
			continue;
		k[n] = (struct SiftF2Packer) {.len = att->attlen,.attr = a - 1,.leaf = store->att_first[a]};
		k[n].kind = pack_kind(store, att, &k[n].scale);
		n++;
	}
	store->packers = k;
	store->npackers = n;
}

static SiftCellKind
kind_of(Form_pg_attribute att)
{
	uint8		scale;

	if (sift_f2_attr_num64(att, &scale))
		return SIFT_CELL_INT;
	if (att->attbyval)
		return att->atttypid == FLOAT4OID || att->atttypid == FLOAT8OID ? SIFT_CELL_F8 : SIFT_CELL_INT;
	return SIFT_CELL_STR;
}

static void
pack_columns(Relation rel, SiftF2Store *store, MemoryContext row_cx, const Datum *values, const bool *isnull, bool spill_wide_blobs, SiftCell *cells)
{
	TupleDesc	desc = RelationGetDescr(rel);

	if (store->packers == NULL)
		packers_build(store, desc);
	memset(cells, 0, sizeof(SiftCell) * store->ncols);
	for (int i = 0; i < store->npackers; i++)
	{
		const struct SiftF2Packer *k = &store->packers[i];

		if (isnull[k->attr])
			cells[k->leaf].isnull = true;
		else
			pack_cell(k, values[k->attr], row_cx, spill_wide_blobs, &cells[k->leaf]);
	}
}

/* The leaves of the declared paths: what the shredder makes of the documents, a cell for each leaf of a row and for each element of a list */
void
sift_f2_store_pack_paths(const SiftF2Store *store, MemoryContext row_cx, const Datum *values, const bool *isnull, SiftCell *cells, SiftCell **elem)
{
	MemoryContext old = MemoryContextSwitchTo(row_cx);
	SiftColRow	row = {.ncols = store->ncols,.val = palloc0(store->ncols * sizeof(SiftColValue)),.elem = palloc0(store->ncols * sizeof(SiftColValue *))};

	SIFT_INVARIANT(elem != NULL || !store->has_groups, "the row of table %u has lists of declared paths and no room for their elements", store->relid);
	sift_col_shred_row(store->paths, values, isnull, &row, store->path_first, row_cx);
	for (int c = 0; c < store->ncols; c++)
		if (sift_f2_leaf_is_path(store, c) && store->parent[c] < 0)
			cells[c] = sift_f2_value_cell(store->kinds[c], &row.val[c]);
	for (int c = 0; c < store->ncols; c++)
	{
		const int	g = store->parent[c];
		int64		n = g < 0 || cells[g].isnull ? 0 : cells[g].i;

		if (g < 0)
			continue;
		elem[c] = n > 0 ? palloc(n * sizeof(SiftCell)) : NULL;
		for (int64 j = 0; j < n; j++)
			elem[c][j] = sift_f2_value_cell(store->kinds[c], &row.elem[c][j]);
	}
	MemoryContextSwitchTo(old);
}

void
sift_f2_store_pack_row(Relation rel, SiftF2Store *store, MemoryContext row_cx, const ItemPointerData *tid, const Datum *values, const bool *isnull,
					   bool spill_wide_blobs, SiftCell *cells, SiftCell **elem, uint8 *key)
{
	if (store->host->host_kind == SIFT_F2_HOST_INDEX)
	{
		sift_f2_row(rel, ((SiftColRel *) store->host_data)->f2, row_cx, tid, values, isnull, cells, elem, key);
		return;
	}
	pack_columns(rel, store, row_cx, values, isnull, spill_wide_blobs, cells);
	if (elem != NULL)
		memset(elem, 0, sizeof(SiftCell *) * store->ncols);
	if (store->paths != NULL)
		sift_f2_store_pack_paths(store, row_cx, values, isnull, cells, elem);
	sift_row_key(&store->spec, cells, ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid), key);
}

void
sift_f2_store_pack_key(Relation rel, SiftF2Store *store, MemoryContext row_cx, const ItemPointerData *tid, const Datum *values, const bool *isnull, SiftCell *cells, uint8 *key)
{
	pack_columns(rel, store, row_cx, values, isnull, false, cells);
	sift_row_key(&store->spec, cells, ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid), key);
}

SiftCellKind
sift_f2_kind_of_attribute(Form_pg_attribute att)
{
	return kind_of(att);
}

bool
sift_f2_leaf_is_path(const SiftF2Store *s, int leaf)
{
	return s->path_def != NULL && s->path_def[leaf].path != NULL;
}

const SiftPartColIdentity *
sift_f2_unnamed_path_ids(const SiftF2Store *s)
{
	SiftPartColIdentity *ids = memcpy(palloc(s->spec.ncol_ids * sizeof(SiftPartColIdentity)), s->spec.col_ids, s->spec.ncol_ids * sizeof(SiftPartColIdentity));

	for (int c = 0; c < s->spec.ncol_ids; c++)
		if (sift_f2_leaf_is_path(s, c))
			ids[c] = (SiftPartColIdentity) {.phys_kind = ids[c].phys_kind,.phys_hint = ids[c].phys_hint,.phys_scale = ids[c].phys_scale};
	return ids;
}
