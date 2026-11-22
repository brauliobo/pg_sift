/*
 * col/f2/f2_store.c - the Format 2.0 relation descriptor of an index or a table: one cache entry per relation, made by the host's loader, and the one conversion of a row's datums
 * to cells and key.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_store_schema.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

typedef struct StoreEntry
{
	Oid			relid;
	SiftF2Store *store;
	uint64		tick;
} StoreEntry;

static HTAB *store_cache = NULL;
static bool store_cache_registered = false;

static void
drop_entry(StoreEntry *e)
{
	MemoryContextDelete(e->store->cx);
	hash_search(store_cache, &e->relid, HASH_REMOVE, NULL);
}

static void
store_invalidate(Datum arg pg_attribute_unused(), int cacheid pg_attribute_unused(), uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	StoreEntry *e;

	if (store_cache == NULL)
		return;
	hash_seq_init(&seq, store_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (hashvalue == 0 || GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(e->relid)) == hashvalue)
			drop_entry(e);
}

/* the entry used longest ago of those no statement of the transaction may hold */
static StoreEntry *
store_victim(void)
{
	HASH_SEQ_STATUS seq;
	StoreEntry *e,
			   *victim = NULL;

	if (store_cache == NULL)
		return NULL;
	hash_seq_init(&seq, store_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (!sift_cache_used_in_xact(e->tick) && (victim == NULL || e->tick < victim->tick))
			victim = e;
	return victim;
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

	store_cache = hash_create("sift cache f2 stores", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	if (!store_cache_registered)
	{
		CacheRegisterSyscacheCallback(RELOID, store_invalidate, (Datum) 0);
		sift_cache_register(&store_cache_kind);
		store_cache_registered = true;
	}
}

static const SiftF2HostOps *
host_of(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX ? &sift_col_host_ops : &sift_tbl_host_ops;
}

static SiftF2Store *
store_build(Relation rel)
{
	MemoryContext cx = sift_cache_create_context("sift cache f2 store");
	SiftF2Store *s = MemoryContextAllocZero(cx, sizeof(SiftF2Store));

	s->relid = RelationGetRelid(rel);
	s->locator = rel->rd_locator;
	s->host = host_of(rel);
	s->cx = cx;
	s->row_cx = AllocSetContextCreateInternal(cx, "sift cache f2 row", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	s->natts = RelationGetDescr(rel)->natts;
	s->host->load_options(rel, s, cx);
	return s;
}

SiftF2Store *
sift_f2_store_open(Relation rel)
{
	StoreEntry *e;
	bool		found;
	Oid			relid = RelationGetRelid(rel);

	if (store_cache == NULL)
		store_cache_init();
	e = hash_search(store_cache, &relid, HASH_ENTER, &found);
	if (!found)
	{
		e->store = NULL;
		PG_TRY();
		{
			e->store = store_build(rel);
		}
		PG_CATCH();
		{
			hash_search(store_cache, &relid, HASH_REMOVE, NULL);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	e->tick = sift_cache_tick();
	return e->store;
}

SiftF2Store *
sift_f2_store_from_oid(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	SiftF2Store *s = sift_f2_store_open(rel);

	relation_close(rel, NoLock);
	return s;
}

void
sift_f2_cell_of_datum(Form_pg_attribute att, Datum d, MemoryContext row_cx, bool spill, SiftCell *c)
{
	MemoryContext old;
	uint8		scale;

	if (sift_f2_attr_num64(att, &scale))
	{
		sift_f2_num64_cell(d, scale, row_cx, c);
		return;
	}
	if (att->attbyval)
	{
		if (att->atttypid == FLOAT4OID)
			c->f = DatumGetFloat4(d);
		else if (att->atttypid == FLOAT8OID)
			c->f = DatumGetFloat8(d);
		else
			c->i = att->attlen == 8 ? DatumGetInt64(d) : att->attlen == 4 ? DatumGetInt32(d) : att->attlen == 2 ? DatumGetInt16(d) : (int8) DatumGetChar(d);
		return;
	}
	if (att->attlen > 0)
	{
		c->s = DatumGetPointer(d);
		c->slen = att->attlen;
		return;
	}
	if (att->attlen == -2)
	{
		c->s = DatumGetCString(d);
		c->slen = strlen(c->s) + 1;
		return;
	}
	old = MemoryContextSwitchTo(row_cx);
	c->s = (const char *) PG_DETOAST_DATUM(d);
	MemoryContextSwitchTo(old);
	c->slen = VARSIZE_ANY(c->s);
	if (spill && c->slen - VARHDRSZ > SIFT_F2_WIDE_THRESHOLD)
	{
		c->isnull = true;
		c->tag = SIFT_F2_TAG_WIDE_RAW;
		c->xp = c->s;
		c->xplen = c->slen;
		c->s = NULL;
		c->slen = 0;
	}
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

void
sift_f2_store_pack_row(Relation rel, SiftF2Store *store, MemoryContext row_cx, const ItemPointerData *tid, const Datum *values, const bool *isnull,
					   bool spill_wide_blobs, SiftCell *cells, SiftCell **elem, uint8 *key)
{
	TupleDesc	desc = RelationGetDescr(rel);

	if (store->host->host_kind == SIFT_F2_HOST_INDEX)
	{
		sift_f2_row(rel, ((SiftColRel *) store->host_data)->f2, row_cx, tid, values, isnull, cells, elem, key);
		return;
	}
	memset(cells, 0, sizeof(SiftCell) * store->ncols);
	for (int a = 1; a <= store->natts; a++)
	{
		int			leaf = store->att_first[a];
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		if (leaf < 0)
			continue;
		Assert(store->kinds[leaf] == kind_of(att));
		if (isnull[a - 1])
			cells[leaf].isnull = true;
		else
			sift_f2_cell_of_datum(att, values[a - 1], row_cx, spill_wide_blobs, &cells[leaf]);
	}
	if (elem != NULL)
		memset(elem, 0, sizeof(SiftCell *) * store->ncols);
	sift_row_key(&store->spec, cells, ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid), key);
}

SiftCellKind
sift_f2_kind_of_attribute(Form_pg_attribute att)
{
	return kind_of(att);
}
