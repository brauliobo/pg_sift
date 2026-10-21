/*
 * store.c - core/store.h over SQLite. A "relation" is the page file of one virtual table: the 8 KiB engine
 * blocks are laid end to end in the shadow table <name>_chunks, chunk_bytes per row. A unit of the cache is one chunk
 * (or one block when the rows are smaller than a block): a bounded set of 64-byte aligned slots filled straight from the
 * row with sqlite3_blob_read (one copy), evicted by CLOCK, never while pinned. Pointers into pages are handed out
 * pinned (like PostgreSQL's shared buffers) and stay valid until released. Segments are immutable runs of blocks that
 * start on a unit boundary, so a retired segment owns whole rows; the manifest (<name>_segs, <name>_meta) is the
 * directory and commits with the SQLite transaction, so a rollback drops segments and block allocations with it. The
 * LIVE pages of a segment are edited in place in the cache and written back with sqlite3_blob_write.
 */
#include "store_int.h"

#include <sys/mman.h>
#include <unistd.h>

SQLITE_EXTENSION_INIT3

#define SQ_MIN_SLOTS	64				/* the cache holds at least this many units, whatever its size */

static uint32 store_serial;

/* ---------- SQL helpers ---------- */

char *
tbl(const SqStore *st, const char *suffix)
{
	return sqlite3_mprintf("\"%w\".\"%w_%s\"", st->schema, st->name, suffix);
}

/* fmt gets the quoted shadow table suffix as %s, and optionally one more string */
static void
exec(SqStore *st, const char *fmt, const char *suffix, const char *extra)
{
	char	   *t = tbl(st, suffix),
			   *sql = sqlite3_mprintf(fmt, t, extra);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
}

sqlite3_stmt *
prep(SqStore *st, sqlite3_stmt **slot, const char *fmt, const char *suffix)
{
	if (*slot == NULL)
	{
		char	   *t = tbl(st, suffix),
				   *sql = sqlite3_mprintf(fmt, t);
		int			rc = sqlite3_prepare_v3(st->db, sql, -1, SQLITE_PREPARE_PERSISTENT, slot, NULL);

		sqlite3_free(sql);
		sqlite3_free(t);
		CHECK(st, rc);
	}
	return *slot;
}

static void
finalize_all(SqStore *st)
{
	sqlite3_finalize(st->put);
	sqlite3_finalize(st->ver);
	st->put = st->ver = NULL;
	close_blobs(st);
}

/* ---------- geometry ---------- */

void
set_geometry(SqStore *st, int chunk_bytes)
{
	st->chunk_bytes = chunk_bytes;
	st->unit = Max((uint32) chunk_bytes, SQ_PAGE);
	st->P = st->unit / SQ_PAGE;
	st->R = st->unit / chunk_bytes;
}

bool
chunk_size_ok(int c)
{
	return c >= 4096 && c <= 65536 && (c & (c - 1)) == 0;
}

/* ---------- cache ---------- */

static SqCache *
cache_new(SqStore *st)
{
	SqCache    *c = calloc(1, sizeof(SqCache));
	size_t		bytes = (size_t) st->cache_kib * 1024;

	c->soft = Max((uint32) (bytes / st->unit), SQ_MIN_SLOTS);
	c->hard = c->soft * 2 + 512;
	c->map_len = (size_t) c->hard * st->unit + st->unit;
	c->map = mmap(NULL, c->map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (c->map == MAP_FAILED)
	{
		free(c);
		elog(ERROR, "sift: cannot reserve %zu bytes of cache", bytes);
	}
	c->base = (char *) TYPEALIGN(st->unit, c->map);
	c->slot = calloc(c->hard, sizeof(SqSlot));
	c->nbucket = pg_nextpower2_32(c->hard * 2);
	c->bucket = calloc(c->nbucket, sizeof(uint32));
	c->freelist = malloc(c->hard * sizeof(uint32));
	for (uint32 i = 0; i < c->hard; i++)
		c->freelist[i] = c->hard - 1 - i;
	c->nfree = c->hard;
	return c;
}

void
cache_free(SqCache *c)
{
	if (c == NULL)
		return;
	munmap(c->map, c->map_len);
	free(c->slot);
	free(c->bucket);
	free(c->freelist);
	free(c);
}

static inline uint32
chunk_hash(const SqCache *c, uint32 chunk)
{
	return (chunk * 2654435761u) & (c->nbucket - 1);
}

static inline char *
slot_data(const SqStore *st, uint32 s)
{
	return st->cache->base + (size_t) s * st->unit;
}

static int64_t
cache_find(const SqCache *c, uint32 chunk)
{
	for (uint32 s = c->bucket[chunk_hash(c, chunk)]; s; s = c->slot[s - 1].next)
		if (c->slot[s - 1].chunk == chunk)
			return s - 1;
	return -1;
}

static void
cache_unlink(SqCache *c, uint32 s)
{
	uint32	   *pp = &c->bucket[chunk_hash(c, c->slot[s].chunk)];

	while (*pp != s + 1)
		pp = &c->slot[*pp - 1].next;
	*pp = c->slot[s].next;
	c->slot[s].valid = 0;
	c->freelist[c->nfree++] = s;
	c->nvalid--;
}

/* CLOCK: a unit survives one sweep after its last use; pinned and edited ones are skipped */
static bool
cache_evict_one(SqCache *c)
{
	for (uint32 steps = 0; steps < 2 * c->hard; steps++)
	{
		uint32		s = c->hand;
		SqSlot	   *sl = &c->slot[s];

		c->hand = (c->hand + 1) % c->hard;
		if (!sl->valid || __atomic_load_n(&sl->pins, __ATOMIC_ACQUIRE) > 0 || sl->dirty)
			continue;
		if (sl->ref)
		{
			sl->ref = 0;
			continue;
		}
		cache_unlink(c, s);
		c->evictions++;
		return true;
	}
	return false;
}

static void
read_unit(SqStore *st, uint32 unit, char *dst)
{
	char	   *last = NULL;

	for (uint32 j = 0; j < st->R; j++)
	{
		int64_t		row = (int64_t) unit * st->R + j;
		int			rc,
					len;

		if (st->blob)
			rc = sqlite3_blob_reopen(st->blob, row);
		else
			rc = sqlite3_blob_open(st->db, st->schema, st->chunks_name, "data", row, 0, &st->blob);
		if (rc != SQLITE_OK)
		{
			sqlite3_blob_close(st->blob);
			st->blob = NULL;
			if (j > 0)
				break;				/* the last unit of a segment may stop after a row */
			elog(ERROR, "sift: chunk %u of \"%s\" is missing", unit, st->name);
		}
		len = sqlite3_blob_bytes(st->blob);
		if (len > st->chunk_bytes || sqlite3_blob_read(st->blob, dst + (size_t) j * st->chunk_bytes, len, 0) != SQLITE_OK)
			elog(ERROR, "sift: cannot read chunk %u of \"%s\": %s", unit, st->name, sqlite3_errmsg(st->db));
		last = dst + (size_t) j * st->chunk_bytes + len;
	}
	if (last && last < dst + st->unit)
		memset(last, 0, dst + st->unit - last);
}

/* The slot holding the unit, loaded if need be */
static uint32
cache_get(SqStore *st, uint32 unit)
{
	SqCache    *c = st->cache;
	int64_t		f;
	uint32		s;

	if (c == NULL)
		c = st->cache = cache_new(st);
	f = cache_find(c, unit);
	if (f >= 0)
	{
		__atomic_fetch_add(&c->hits, 1, __ATOMIC_RELAXED);
		if (!st->shared)
			c->slot[f].ref = 1;
		return f;
	}
	if (st->shared)
		elog(ERROR, "sift: chunk %u of \"%s\" is not cached in a parallel scan", unit, st->name);
	while (c->nvalid >= c->soft && cache_evict_one(c))
		;
	if (c->nfree == 0)
		elog(ERROR, "sift: the cache of \"%s\" is full of pinned chunks (%u)", st->name, c->hard);
	s = c->freelist[--c->nfree];
	c->misses++;
	read_unit(st, unit, c->base + (size_t) s * st->unit);
	c->slot[s] = (SqSlot) {.chunk = unit, .valid = 1, .ref = 1, .next = c->bucket[chunk_hash(c, unit)]};
	c->bucket[chunk_hash(c, unit)] = s + 1;
	c->nvalid++;
	return s;
}

/* All units go; the caller holds no pin */
void
cache_invalidate(SqStore *st)
{
	SqCache    *c = st->cache;

	for (uint32 i = 0; c && i < c->hard; i++)
		if (c->slot[i].valid && c->slot[i].pins <= 0)
		{
			c->slot[i].dirty = 0;
			c->slot[i].queued = 0;
			cache_unlink(c, i);
		}
	st->ndirty = 0;
	st->epoch++;
	st->rel.rd_locator.relNumber = (st->serial << 12) + (st->epoch & 0xFFF);
}

void
sq_store_invalidate(SqStore *st)
{
	cache_invalidate(st);
}

void
cache_drop_unit(SqStore *st, uint32 unit)
{
	int64_t		f = st->cache ? cache_find(st->cache, unit) : -1;

	if (f >= 0 && st->cache->slot[f].pins <= 0 && !st->cache->slot[f].dirty)
		cache_unlink(st->cache, f);
}

void
sq_store_set_cache(SqStore *st, int cache_kib)
{
	cache_free(st->cache);
	st->cache = NULL;
	st->ndirty = 0;
	st->cache_kib = cache_kib;
}

void
sq_store_recover(SqStore *st)
{
	SqCache    *c = st->cache;

	for (uint32 i = 0; c && i < c->hard; i++)
		c->slot[i].pins = 0;
	__atomic_store_n(&st->npinned, 0, __ATOMIC_RELAXED);
	close_blobs(st);
}

bool
sq_store_preload(SqStore *st)
{
	const SiftDirSnapshot *d = &st->dir;
	uint64		missing = 0;

	if (st->cache == NULL)
		st->cache = cache_new(st);
	if (st->resident_epoch == st->epoch + 1 && st->resident_version == d->version &&
		st->resident_evictions == st->cache->evictions)
		return true;
	for (int i = 0; i < d->nsegs; i++)
		for (uint32 u = d->segs[i].reserved / st->P; u <= d->segs[i].header_blk / st->P; u++)
			missing += cache_find(st->cache, u) < 0;
	if (st->cache->nvalid + missing > st->cache->soft)
		return false;
	for (int i = 0; i < d->nsegs; i++)
		for (uint32 u = d->segs[i].reserved / st->P; u <= d->segs[i].header_blk / st->P; u++)
			cache_get(st, u);
	st->resident_epoch = st->epoch + 1;
	st->resident_version = d->version;
	st->resident_evictions = st->cache->evictions;
	return true;
}

void
sq_store_set_shared(SqStore *st, bool on)
{
	st->shared = on;
}

/* ---------- lifecycle ---------- */

SqStore *
sq_store_new(sqlite3 *db, const char *schema, const char *name, const SqOptions *opts, int chunk_bytes, int cache_kib)
{
	SqStore    *st = calloc(1, sizeof(SqStore));

	st->db = db;
	st->schema = strdup(schema);
	st->name = strdup(name);
	st->chunks_name = sqlite3_mprintf("%s_chunks", name);
	st->opts = *opts;
	st->cache_kib = cache_kib;
	st->serial = __atomic_add_fetch(&store_serial, 1, __ATOMIC_RELAXED);
	set_geometry(st, chunk_bytes);
	st->hw = 2;								/* blocks 0 and 1 stay unused, as the metapage and directory of an index */
	st->next_gen = 1;
	snprintf(st->rel.relname, sizeof(st->rel.relname), "%s", name);
	st->rel.store = st;
	st->rel.rd_locator.relNumber = st->serial << 12;
	return st;
}

void
sq_store_free(SqStore *st)
{
	finalize_all(st);
	cache_free(st->cache);
	free(st->dirty);
	free(st->schema);
	free(st->name);
	sqlite3_free(st->chunks_name);
	free(st);
}

Relation
sq_store_rel(SqStore *st)
{
	return &st->rel;
}

const SqOptions *
sq_store_options(const SqStore *st)
{
	return &st->opts;
}

int
sq_store_chunk_bytes(const SqStore *st)
{
	return st->chunk_bytes;
}

void
sq_store_create_tables(SqStore *st)
{
	char	   *chunk = sqlite3_mprintf("%d", st->chunk_bytes);

	if (!chunk_size_ok(st->chunk_bytes))
		elog(ERROR, "sift: chunk must be a power of two from 4096 to 65536");
	exec(st, "CREATE TABLE %s(id INTEGER PRIMARY KEY, data BLOB)", "chunks", NULL);
	exec(st, "CREATE TABLE %s(gen INTEGER PRIMARY KEY, header_blk, first_blk, sb_lo, sb_hi, ndocs, sum_dl, bytes, flags)", "segs", NULL);
	exec(st, "CREATE TABLE %s(id INTEGER PRIMARY KEY, version, lversion, hw, next_gen)", "meta", NULL);
	exec(st, "CREATE TABLE %s(k PRIMARY KEY, v) WITHOUT ROWID", "config", NULL);
	exec(st, "INSERT INTO %s VALUES(1, 1, 1, 2, 1)", "meta", NULL);		/* version, lversion, hw, next_gen */
	sq_store_config_set(st, "format", SQ_STORE_FORMAT);
	sq_store_config_set(st, "chunk", st->chunk_bytes);
	sqlite3_free(chunk);
}

void
sq_store_drop_tables(SqStore *st)
{
	finalize_all(st);
	exec(st, "DROP TABLE IF EXISTS %s", "chunks", NULL);
	exec(st, "DROP TABLE IF EXISTS %s", "segs", NULL);
	exec(st, "DROP TABLE IF EXISTS %s", "meta", NULL);
	exec(st, "DROP TABLE IF EXISTS %s", "config", NULL);
}

void
sq_store_rename(SqStore *st, const char *newname)
{
	finalize_all(st);
	free(st->name);
	st->name = strdup(newname);
	sqlite3_free(st->chunks_name);
	st->chunks_name = sqlite3_mprintf("%s_chunks", newname);
	snprintf(st->rel.relname, sizeof(st->rel.relname), "%s", newname);
}

int64_t
sq_store_config_get(SqStore *st, const char *key, int64_t dflt)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "config"),
			   *sql = sqlite3_mprintf("SELECT v FROM %s WHERE k = ?1", t);
	int			rc = sqlite3_prepare_v2(st->db, sql, -1, &q, NULL);
	int64_t		v = dflt;

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(q) == SQLITE_ROW)
		v = sqlite3_column_int64(q, 0);
	sqlite3_finalize(q);
	return v;
}

void
sq_store_config_set(SqStore *st, const char *key, int64_t value)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "config"),
			   *sql = sqlite3_mprintf("INSERT OR REPLACE INTO %s VALUES(?1, ?2)", t);
	int			rc = sqlite3_prepare_v2(st->db, sql, -1, &q, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_int64(q, 2, value);
	rc = sqlite3_step(q);
	sqlite3_finalize(q);
	CHECK(st, rc);
	st->changed = true;
}

/* ---------- chunk rows: statistics and the integrity check ---------- */

bool
sq_store_rows_between(SqStore *st, int64_t lo, int64_t hi, int64_t *count, int64_t *bytes)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "chunks"),
			   *sql = sqlite3_mprintf("SELECT count(*), coalesce(sum(length(data)), 0) FROM %s WHERE id BETWEEN %lld AND %lld", t,
									  (long long) lo, (long long) hi);
	bool		ok = sqlite3_prepare_v2(st->db, sql, -1, &q, NULL) == SQLITE_OK;

	if (ok && sqlite3_step(q) == SQLITE_ROW)
	{
		*count = sqlite3_column_int64(q, 0);
		*bytes = sqlite3_column_int64(q, 1);
	}
	else
		ok = false;
	if (q)
		sqlite3_finalize(q);
	sqlite3_free(sql);
	sqlite3_free(t);
	return ok;
}

int64_t
sq_store_rows_total(SqStore *st)
{
	int64_t		n = 0,
				b;

	sq_store_rows_between(st, INT64_MIN, INT64_MAX, &n, &b);
	return n;
}

void
sq_store_stats(SqStore *st, SqStoreStats *out)
{
	SqCache    *c = st->cache;

	memset(out, 0, sizeof(*out));
	out->pin_leaks = st->pin_leaks;
	sq_store_rows_between(st, INT64_MIN, INT64_MAX, (int64_t *) &out->chunks_total, (int64_t *) &out->bytes_total);
	if (c)
	{
		out->cached_bytes = (uint64) c->nvalid * st->unit;
		out->hits = c->hits;
		out->misses = c->misses;
		out->evictions = c->evictions;
		out->slots = c->soft;
		for (uint32 i = 0; i < c->hard; i++)
			out->pinned += c->slot[i].valid && c->slot[i].pins > 0;
	}
}

/* ---------- pages ---------- */

/* The store the calling thread is working for: ReleaseBuffer has no relation argument */
static _Thread_local SqStore *cur_store;

SqStore *
sq_store_enter(SqStore *st)
{
	SqStore    *old = cur_store;

	cur_store = st;
	return old;
}

static void release_locks(SqStore *st);

void
sq_store_leave(SqStore *old)
{
	SqStore    *st = cur_store;

	cur_store = old;
	if (__atomic_load_n(&st->npinned, __ATOMIC_ACQUIRE) != 0 && !st->shared)
	{
		st->pin_leaks++;
		sq_store_recover(st);
	}
	if (old == NULL)
		release_locks(st);
}

/* An open blob handle keeps SQLite's read lock after its statement ends; hold it only while a statement or transaction needs it */
static void
release_locks(SqStore *st)
{
	if (st->cursors == 0 && !st->shared && sqlite3_get_autocommit(st->db))
		close_blobs(st);
}

void
sq_store_hold(SqStore *st, int delta)
{
	st->cursors += delta;
	release_locks(st);
}

static inline uint32
slot_index(const SqStore *st, Buffer buf)
{
	return (uint32) ((buf - st->cache->base) / st->unit);
}

Buffer
ReadBuffer(Relation rel, BlockNumber blkno)
{
	SqStore    *st = rel->store;
	uint32		s = cache_get(st, blkno / st->P);

	__atomic_fetch_add(&st->cache->slot[s].pins, 1, __ATOMIC_ACQUIRE);
	__atomic_fetch_add(&st->npinned, 1, __ATOMIC_RELAXED);
	return slot_data(st, s) + (size_t) (blkno % st->P) * SQ_PAGE;
}

void
sq_unpin(Buffer buf)
{
	SqStore    *st = cur_store;
	SqSlot	   *sl;

	if (buf == NULL)
		return;
	sl = &st->cache->slot[slot_index(st, buf)];
	if (__atomic_load_n(&sl->pins, __ATOMIC_RELAXED) > 0)
	{
		__atomic_fetch_sub(&sl->pins, 1, __ATOMIC_RELEASE);
		__atomic_fetch_sub(&st->npinned, 1, __ATOMIC_RELAXED);
	}
}

BlockNumber
BufferGetBlockNumber(Buffer buf)
{
	SqStore    *st = cur_store;
	uint32		s = slot_index(st, buf);

	return st->cache->slot[s].chunk * st->P + (uint32) ((buf - slot_data(st, s)) / SQ_PAGE);
}

GenericXLogState *
GenericXLogStart(Relation rel)
{
	return (GenericXLogState *) rel;
}

Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buf, int flags)
{
	SqStore    *st = ((Relation) state)->store;
	uint32		s = slot_index(st, buf);
	SqSlot	   *sl = &st->cache->slot[s];

	sl->dirty |= 1 << ((buf - slot_data(st, s)) / SQ_PAGE);
	st->changed = true;				/* a rollback must drop the edit */
	if (!sl->queued)
	{
		sl->queued = 1;
		if (st->ndirty == st->dirty_cap)
		{
			st->dirty_cap = st->dirty_cap ? st->dirty_cap * 2 : 64;
			st->dirty = realloc(st->dirty, st->dirty_cap * sizeof(uint32));
		}
		st->dirty[st->ndirty++] = s;
	}
	return buf;
}

void
GenericXLogFinish(GenericXLogState *state)
{
}

void
GenericXLogAbort(GenericXLogState *state)
{
}

/* Writes the byte range [off, off + len) of a unit back into its rows, in place */
static void
write_range(SqStore *st, uint32 unit, uint32 off, uint32 len, const char *src)
{
	for (uint32 j = off / st->chunk_bytes; j <= (off + len - 1) / st->chunk_bytes; j++)
	{
		uint32		lo = Max(off, j * (uint32) st->chunk_bytes),
					hi = Min(off + len, (j + 1) * (uint32) st->chunk_bytes);
		int64_t		row = (int64_t) unit * st->R + j;
		int			rc;

		if (st->wblob)
			rc = sqlite3_blob_reopen(st->wblob, row);
		else
			rc = sqlite3_blob_open(st->db, st->schema, st->chunks_name, "data", row, 1, &st->wblob);
		if (rc == SQLITE_OK)
			rc = sqlite3_blob_write(st->wblob, src + (lo - off), hi - lo, lo - j * st->chunk_bytes);
		if (rc != SQLITE_OK)
		{
			sqlite3_blob_close(st->wblob);
			st->wblob = NULL;
			elog(ERROR, "sift: cannot write chunk %u of \"%s\": %s", unit, st->name, sqlite3_errmsg(st->db));
		}
	}
}

void
sq_store_flush_pages(SqStore *st)
{
	bool		wrote = false;

	close_blobs(st);
	for (int i = 0; i < st->ndirty; i++)
	{
		SqSlot	   *sl = &st->cache->slot[st->dirty[i]];

		for (uint32 pg = 0; pg < st->P; pg++)
			if (sl->dirty >> pg & 1)
			{
				write_range(st, sl->chunk, pg * SQ_PAGE, SQ_PAGE, slot_data(st, st->dirty[i]) + (size_t) pg * SQ_PAGE);
				wrote = true;
			}
		sl->dirty = 0;
		sl->queued = 0;
	}
	st->ndirty = 0;
	sqlite3_blob_close(st->wblob);
	st->wblob = NULL;
	if (wrote)
		persist_meta(st, true);
}
