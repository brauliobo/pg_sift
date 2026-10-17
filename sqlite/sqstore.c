/*
 * sqstore.c - sift_store.h over SQLite. A "relation" is the page file of one virtual table: the 8 KiB engine
 * blocks are laid end to end in the shadow table <name>_chunks, chunk_bytes per row. A unit of the cache is one chunk
 * (or one block when the rows are smaller than a block): a bounded set of 64-byte aligned slots filled straight from the
 * row with sqlite3_blob_read (one copy), evicted by CLOCK, never while pinned. Pointers into pages are handed out
 * pinned (like PostgreSQL's shared buffers) and stay valid until released. Segments are immutable runs of blocks that
 * start on a unit boundary, so a retired segment owns whole rows; the manifest (<name>_segs, <name>_meta) is the
 * directory and commits with the SQLite transaction, so a rollback drops segments and block allocations with it. The
 * LIVE pages of a segment are edited in place in the cache and written back with sqlite3_blob_write.
 */
#include "sqapi.h"

#include <sys/mman.h>
#include <unistd.h>

SQLITE_EXTENSION_INIT3

#define SQ_PAGE			BLCKSZ
#define SQ_MIN_SLOTS	64				/* the cache holds at least this many units, whatever its size */

typedef struct SqSlot
{
	uint32		chunk;					/* unit held */
	uint32		next;					/* hash chain: slot + 1 */
	int32		pins;
	uint8		ref;
	uint8		valid;
	uint8		dirty;					/* bit per block of the unit edited since the last write-back */
	uint8		queued;					/* in the dirty list */
} SqSlot;

typedef struct SqCache
{
	char	   *map;
	size_t		map_len;
	char	   *base;					/* unit-aligned */
	SqSlot	   *slot;
	uint32	   *bucket;
	uint32		nbucket;
	uint32	   *freelist;
	uint32		nfree;
	uint32		soft,					/* units the cache keeps */
				hard;					/* units it may hold while the rest is pinned */
	uint32		nvalid;
	uint32		hand;
	uint64		hits,
				misses,
				evictions;
} SqCache;

struct SqStore
{
	sqlite3    *db;
	char	   *schema;
	char	   *name;
	char	   *chunks_name;			/* <name>_chunks, for sqlite3_blob_open */
	SqOptions	opts;
	RelationData rel;
	int			chunk_bytes;
	uint32		unit;					/* bytes of a cache unit */
	uint32		P;						/* blocks per unit */
	uint32		R;						/* rows per unit */
	int			cache_kib;
	SqCache    *cache;
	bool		shared;					/* parallel readers: lookups only */
	uint32	   *dirty;					/* units with edited blocks */
	int			ndirty,
				dirty_cap;
	/* directory */
	SiftDirSnapshot dir;
	uint32		lversion;
	BlockNumber hw;
	uint64		next_gen;
	uint32		epoch;
	uint32		serial;
	bool		changed;				/* this transaction wrote something */
	int32		npinned;				/* pins held on any unit */
	uint64		pin_leaks;				/* calls that ended holding pins (a bug in the engine paths, counted by the tests) */
	/* statements */
	sqlite3_stmt *put,
			   *ver;
	sqlite3_blob *blob,
			   *wblob;
	/* fault injection */
	int			fault_mode;
	int64_t		fault_after;
};

static uint32 store_serial;

/* ---------- SQL helpers ---------- */

#define CHECK(st, rc) \
	do { \
		int rc_ = (rc); \
		if (rc_ != SQLITE_OK && rc_ != SQLITE_ROW && rc_ != SQLITE_DONE) \
			elog(ERROR, "sift: %s", sqlite3_errmsg((st)->db)); \
	} while (0)

static char *
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

static sqlite3_stmt *
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
close_blobs(SqStore *st)
{
	sqlite3_blob_close(st->blob);
	sqlite3_blob_close(st->wblob);
	st->blob = st->wblob = NULL;
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

static void
set_geometry(SqStore *st, int chunk_bytes)
{
	st->chunk_bytes = chunk_bytes;
	st->unit = Max((uint32) chunk_bytes, SQ_PAGE);
	st->P = st->unit / SQ_PAGE;
	st->R = st->unit / chunk_bytes;
}

static bool
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

static void
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
		c->hits++;
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
static void
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

static void
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
	for (int i = 0; i < d->nsegs; i++)
		for (uint32 u = d->segs[i].reserved / st->P; u <= d->segs[i].header_blk / st->P; u++)
			missing += cache_find(st->cache, u) < 0;
	if (st->cache->nvalid + missing > st->cache->soft)
		return false;
	for (int i = 0; i < d->nsegs; i++)
		for (uint32 u = d->segs[i].reserved / st->P; u <= d->segs[i].header_blk / st->P; u++)
			cache_get(st, u);
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

/* ---------- directory ---------- */

BlockNumber
sq_store_hw(const SqStore *st)
{
	return st->hw;
}

void
sq_store_set_hw(SqStore *st, BlockNumber hw)
{
	st->hw = hw;
}

const SiftDirSnapshot *
sq_store_dir(const SqStore *st)
{
	return &st->dir;
}

uint64
sq_store_new_gen(SqStore *st)
{
	return st->next_gen++;
}

/* The analyzer hash the segments were written under: the metapage keeps it, eval compares it with the options' */
static uint32
analyzer_hash(const SqStore *st)
{
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_from_options(&st->opts.base, &cfg);
	return cfg.hash;
}

static void
load_config(SqStore *st)
{
	int64_t		format = sq_store_config_get(st, "format", 0),
				chunk = sq_store_config_get(st, "chunk", 0);

	if (format != SQ_STORE_FORMAT)
		elog(ERROR, "sift: table \"%s\" has storage format %lld, this build reads %d", st->name, (long long) format,
			 SQ_STORE_FORMAT);
	if (!chunk_size_ok(chunk))
		elog(ERROR, "sift: table \"%s\" has an invalid chunk size %lld", st->name, (long long) chunk);
	if (chunk != st->chunk_bytes)
	{
		cache_free(st->cache);
		st->cache = NULL;
		set_geometry(st, chunk);
	}
}

static void
load_meta(SqStore *st, uint32 *version)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "meta"),
			   *sql = sqlite3_mprintf("SELECT version, lversion, hw, next_gen FROM %s", t);
	int			rc = sqlite3_prepare_v2(st->db, sql, -1, &q, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	if (sqlite3_step(q) != SQLITE_ROW)
	{
		sqlite3_finalize(q);
		elog(ERROR, "sift: table \"%s\" has no metadata row", st->name);
	}
	*version = sqlite3_column_int(q, 0);
	st->lversion = sqlite3_column_int(q, 1);
	st->hw = (BlockNumber) sqlite3_column_int64(q, 2);
	st->next_gen = sqlite3_column_int64(q, 3);
	sqlite3_finalize(q);
}

static void
load_segs(SqStore *st)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "segs"),
			   *sql = sqlite3_mprintf("SELECT gen, header_blk, first_blk, sb_lo, sb_hi, ndocs, sum_dl, bytes, flags FROM %s ORDER BY gen", t);
	int			rc = sqlite3_prepare_v2(st->db, sql, -1, &q, NULL);
	SiftDirSnapshot *d = &st->dir;

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	d->nsegs = 0;
	d->meta.ndocs = d->meta.sum_dl = 0;
	while (sqlite3_step(q) == SQLITE_ROW)
	{
		SiftSegEntry *e = &d->segs[d->nsegs];

		if (d->nsegs == SIFT_MAX_SEGMENTS)
		{
			sqlite3_finalize(q);
			elog(ERROR, "sift: more than %d segments", SIFT_MAX_SEGMENTS);
		}
		memset(e, 0, sizeof(*e));
		e->gen = sqlite3_column_int64(q, 0);
		e->kind = SIFT_SEG_IMMUTABLE;
		e->format = SIFT_FORMAT_MINOR;
		e->header_blk = sqlite3_column_int64(q, 1);
		e->reserved = sqlite3_column_int64(q, 2);
		e->sb_lo = sqlite3_column_int64(q, 3);
		e->sb_hi = sqlite3_column_int64(q, 4);
		e->ndocs = sqlite3_column_int64(q, 5);
		e->sum_dl = sqlite3_column_int64(q, 6);
		e->bytes = sqlite3_column_int64(q, 7);
		e->flags = sqlite3_column_int(q, 8);
		d->meta.ndocs += e->ndocs;
		d->meta.sum_dl += e->sum_dl;
		d->nsegs++;
	}
	sqlite3_finalize(q);
}

static void
init_meta(SqStore *st)
{
	SiftMetaPageData *m = &st->dir.meta;

	m->magic = SIFT_MAGIC;
	m->format_major = SIFT_FORMAT_MAJOR;
	m->format_minor = SIFT_FORMAT_MINOR;
	m->k1 = (float4) st->opts.base.k1;
	m->b = (float4) st->opts.base.b;
	m->analyzer_hash = analyzer_hash(st);
	m->next_gen = st->next_gen;
	m->high_water = st->hw;
}

/* The committed (or own uncommitted) versions of the manifest and of the LIVE pages */
static bool
versions_moved(SqStore *st)
{
	sqlite3_stmt *q = prep(st, &st->ver, "SELECT version, lversion FROM %s", "meta");
	int			rc = sqlite3_step(q);
	bool		moved = true;

	if (rc == SQLITE_ROW)
		moved = (uint32) sqlite3_column_int(q, 0) != st->dir.version || (uint32) sqlite3_column_int(q, 1) != st->lversion;
	sqlite3_reset(q);
	if (rc != SQLITE_ROW)
		elog(ERROR, "sift: cannot read the metadata of \"%s\": %s", st->name, sqlite3_errmsg(st->db));
	return moved;
}

/*
 * Another connection's commit adds and retires whole segments, whose chunks are immutable and numbered for good, so
 * the cache stays; only an edit of LIVE pages (lversion) or a rollback of ours makes cached chunks untrustworthy.
 */
bool
sq_store_refresh(SqStore *st, bool changed_only)
{
	uint32		version,
				lversion = st->lversion;

	if (changed_only && st->dir.version != 0 && !versions_moved(st))
		return false;
	if (!changed_only)
		load_config(st);
	load_meta(st, &version);
	load_segs(st);
	st->dir.version = version;
	init_meta(st);
	if (!changed_only || st->lversion != lversion)
		cache_invalidate(st);
	return true;
}

/* final: the transaction is over; else a savepoint was rolled back and what came before it may still be undone */
void
sq_store_rollback(SqStore *st, bool final)
{
	if (!st->changed)
		return;
	st->changed = !final;
	close_blobs(st);
	st->dir.version = 0;
	sq_store_refresh(st, false);
}

void
sq_store_commit(SqStore *st)
{
	st->changed = false;
}

static void
persist_meta(SqStore *st, bool live)
{
	char	   *t = tbl(st, "meta"),
			   *sql = sqlite3_mprintf("UPDATE %s SET version = version + 1, lversion = lversion + %d, hw = %u, next_gen = %llu", t,
									  live, st->hw, (unsigned long long) st->next_gen);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	st->dir.version++;
	st->lversion += live;
	st->dir.meta.next_gen = st->next_gen;
	st->dir.meta.high_water = st->hw;
	st->changed = true;
}

void
sq_store_publish(SqStore *st, const SiftSegEntry *e)
{
	SiftDirSnapshot *d = &st->dir;
	char	   *t = tbl(st, "segs"),
			   *sql = sqlite3_mprintf("INSERT INTO %s VALUES(%llu, %u, %u, %u, %u, %llu, %llu, %llu, %u)", t,
									  (unsigned long long) e->gen, e->header_blk, e->reserved, e->sb_lo, e->sb_hi,
									  (unsigned long long) e->ndocs, (unsigned long long) e->sum_dl,
									  (unsigned long long) e->bytes, e->flags);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	if (d->nsegs == SIFT_MAX_SEGMENTS)
		elog(ERROR, "sift: more than %d segments", SIFT_MAX_SEGMENTS);
	d->segs[d->nsegs++] = *e;
	d->meta.ndocs += e->ndocs;
	d->meta.sum_dl += e->sum_dl;
	persist_meta(st, false);
}

void
sq_store_seg_rows(const SqStore *st, const SiftSegEntry *e, int64_t *lo, int64_t *hi)
{
	*lo = (int64_t) (e->reserved / st->P) * st->R;
	*hi = (int64_t) (e->header_blk / st->P) * st->R + st->R - 1;
}

void
sq_store_retire(SqStore *st, uint64 gen)
{
	SiftDirSnapshot *d = &st->dir;
	int			i = 0;
	int64_t		lo,
				hi;
	char	   *t;
	char	   *sql;
	int			rc;

	while (i < d->nsegs && d->segs[i].gen != gen)
		i++;
	if (i == d->nsegs)
		elog(ERROR, "sift: segment %llu is not in the directory", (unsigned long long) gen);
	sq_store_seg_rows(st, &d->segs[i], &lo, &hi);
	close_blobs(st);
	t = tbl(st, "chunks");
	sql = sqlite3_mprintf("DELETE FROM %s WHERE id BETWEEN %lld AND %lld", t, (long long) lo, (long long) hi);
	rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	for (uint32 u = d->segs[i].reserved / st->P; u <= d->segs[i].header_blk / st->P; u++)
		cache_drop_unit(st, u);
	t = tbl(st, "segs");
	sql = sqlite3_mprintf("DELETE FROM %s WHERE gen = %llu", t, (unsigned long long) gen);
	rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	d->meta.ndocs -= d->segs[i].ndocs;
	d->meta.sum_dl -= d->segs[i].sum_dl;
	memmove(&d->segs[i], &d->segs[i + 1], (d->nsegs - i - 1) * sizeof(SiftSegEntry));
	d->nsegs--;
	persist_meta(st, false);
}

void
sq_store_set_flags(SqStore *st, uint64 gen, uint16 flags)
{
	char	   *t = tbl(st, "segs"),
			   *sql = sqlite3_mprintf("UPDATE %s SET flags = %u WHERE gen = %llu", t, flags, (unsigned long long) gen);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].gen == gen)
			st->dir.segs[i].flags = flags;
	persist_meta(st, false);
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

/* ---------- sift_store.h: pages, directory ---------- */

void
sift_page_init(Page page, SiftPageType type, uint64 gen)
{
	PageHeader	h = (PageHeader) page;
	SiftPageOpaque o;

	memset(page, 0, BLCKSZ);
	h->pd_lower = SIFT_PAGE_DATA_OFFSET;
	h->pd_special = BLCKSZ - MAXALIGN(sizeof(SiftPageOpaqueData));
	h->pd_upper = h->pd_special;
	h->pd_pagesize_version = BLCKSZ;
	o = SiftPageGetOpaque(page);
	o->type = type;
	o->gen = gen;
}

void
sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen)
{
	SiftPageOpaque o = SiftPageGetOpaque(BufferGetPage(buf));

	if (o->type != type || (gen != 0 && o->gen != gen))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("sift table \"%s\": block %u is not a page of type %d and generation %llu",
							   RelationGetRelationName(index), BufferGetBlockNumber(buf), (int) type,
							   (unsigned long long) gen)));
}

void
sift_dir_read(Relation index, SiftDirSnapshot *snap)
{
	*snap = index->store->dir;
}

void
sift_dir_update(Relation index, const SiftDirChange *chg)
{
	SqStore    *st = index->store;
	SiftDirectoryData *dir = malloc(offsetof(SiftDirectoryData, segs) + SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));

	dir->nsegs = st->dir.nsegs;
	memcpy(dir->segs, st->dir.segs, st->dir.nsegs * sizeof(SiftSegEntry));
	chg->fn(&st->dir.meta, dir, NULL, chg->arg);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].flags != st->dir.segs[i].flags)
			sq_store_set_flags(st, dir->segs[i].gen, dir->segs[i].flags);
	free(dir);
}

/* ---------- stream reader ---------- */

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents, uint64 gen)
{
	memset(sr, 0, sizeof(*sr));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	for (int i = 0; i < nextents; i++)
	{
		sr->pagestart[i] = sr->npages;
		sr->npages += extents[i].nblocks;
	}
	sr->pagestart[nextents] = sr->npages;
}

/* The reader keeps its current page pinned until it moves to another one or is released */
static Buffer
sr_page(SiftStreamReader *sr, uint32 p)
{
	int			e = sr->cur;
	BlockNumber blk;
	Buffer		nb;

	if (p < sr->pagestart[e] || p >= sr->pagestart[e + 1])
	{
		int			lo = 0,
					hi = sr->nextents - 1;

		while (lo < hi)
		{
			int			mid = (lo + hi + 1) / 2;

			if (sr->pagestart[mid] <= p)
				lo = mid;
			else
				hi = mid - 1;
		}
		e = sr->cur = lo;
	}
	blk = sr->extents[e].start + (p - sr->pagestart[e]);
	if (sr->buf != InvalidBuffer && sr->blkno == blk)
		return sr->buf;
	nb = ReadBuffer(sr->index, blk);
	sq_unpin(sr->buf);
	sr->buf = nb;
	sr->blkno = blk;
	return nb;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	uint32		p = off / SIFT_PAGE_PAYLOAD;
	size_t		o = off % SIFT_PAGE_PAYLOAD;
	char	   *dst = scratch;

	if (off + len > (uint64) sr->npages * SIFT_PAGE_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("sift table \"%s\": read of %zu bytes at %llu is past the segment", RelationGetRelationName(sr->index),
							   len, (unsigned long long) off)));
	if (o + len <= SIFT_PAGE_PAYLOAD)
		return SiftPageGetData(BufferGetPage(sr_page(sr, p))) + o;
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - o);

		memcpy(dst, SiftPageGetData(BufferGetPage(sr_page(sr, p++))) + o, n);
		dst += n;
		len -= n;
		o = 0;
	}
	return scratch;
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
}

void
sift_sr_release(SiftStreamReader *sr)
{
	sq_unpin(sr->buf);
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
}

/* ---------- write target and stream writer ---------- */

/* Pages arrive in ascending block order; a unit is inserted as a row (or rows) once its last page is behind us */
struct SiftWriteTarget
{
	SqStore    *st;
	BlockNumber first;
	BlockNumber next;
	int64_t		cur;					/* unit being filled, -1 none */
	uint32		filled;					/* blocks of it written */
	char	   *buf;
};

SiftWriteTarget *
sift_wt_begin(Relation index, SiftWriteMode mode)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));
	SqStore    *st = index->store;

	wt->st = st;
	wt->first = wt->next = (st->hw + st->P - 1) / st->P * st->P;
	wt->cur = -1;
	wt->buf = MemoryContextAllocAligned(CurrentMemoryContext, st->unit, 64, 0);
	return wt;
}

static void
fault_check(SqStore *st)
{
	if (st->fault_mode && st->fault_after-- == 0)
	{
		if (st->fault_mode == 2)
			_exit(86);
		st->fault_mode = 0;
		elog(ERROR, "sift: injected fault while writing chunks");
	}
}

static void
unit_flush(SiftWriteTarget *wt)
{
	SqStore    *st = wt->st;
	sqlite3_stmt *q = prep(st, &st->put, "INSERT INTO %s VALUES(?1, ?2)", "chunks");
	uint32		total = wt->filled * SQ_PAGE;

	if (wt->cur < 0)
		return;
	for (uint32 j = 0; j * st->chunk_bytes < total; j++)
	{
		int			rc;

		fault_check(st);
		sqlite3_bind_int64(q, 1, wt->cur * st->R + j);
		sqlite3_bind_blob(q, 2, wt->buf + (size_t) j * st->chunk_bytes, Min((uint32) st->chunk_bytes, total - j * st->chunk_bytes),
						  SQLITE_STATIC);
		rc = sqlite3_step(q);
		sqlite3_reset(q);
		sqlite3_clear_bindings(q);
		CHECK(st, rc);
	}
	wt->cur = -1;
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	unit_flush(wt);
	wt->st->hw = wt->next;
	wt->st->changed = true;
}

BlockNumber
sq_wt_first(const SiftWriteTarget *wt)
{
	return wt->first;
}

BlockNumber
sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks)
{
	BlockNumber first = wt->next;

	wt->next += nblocks;
	return first;
}

void
sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	SqStore    *st = wt->st;
	int64_t		unit = blkno / st->P;

	if (unit != wt->cur)
	{
		if (unit < wt->cur)
			elog(ERROR, "sift: block %u written out of order", blkno);
		unit_flush(wt);
		wt->cur = unit;
		wt->filled = 0;
		memset(wt->buf, 0, st->unit);
	}
	memcpy(wt->buf + (size_t) (blkno % st->P) * SQ_PAGE, page, SQ_PAGE);
	wt->filled = Max(wt->filled, blkno % st->P + 1);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	BlockNumber blk = wt->next++;

	sift_wt_put_page(wt, blk, page);
	return blk;
}

void
sq_store_set_fault(SqStore *st, int mode, int64_t after_chunks)
{
	st->fault_mode = mode;
	st->fault_after = after_chunks;
}

struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		total;
	uint32		used;
	char	   *page;
	SiftExtent *ext;
	int			next,
				cap;
};

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->page = MemoryContextAllocAligned(CurrentMemoryContext, BLCKSZ, 64, 0);
	sift_page_init(sw->page, SIFT_PT_DATA, gen);
	sw->cap = 16;
	sw->ext = palloc(sw->cap * sizeof(SiftExtent));
	return sw;
}

static void
sw_flush(SiftStreamWriter *sw)
{
	BlockNumber blk;

	SiftPageSetUsed(sw->page, sw->used);
	blk = sift_wt_write_page(sw->wt, sw->page);
	if (sw->next > 0 && sw->ext[sw->next - 1].start + sw->ext[sw->next - 1].nblocks == blk)
		sw->ext[sw->next - 1].nblocks++;
	else
	{
		if (sw->next == sw->cap)
		{
			sw->cap *= 2;
			sw->ext = repalloc(sw->ext, sw->cap * sizeof(SiftExtent));
		}
		sw->ext[sw->next++] = (SiftExtent) {blk, 1};
	}
	sift_page_init(sw->page, SIFT_PT_DATA, sw->gen);
	sw->used = 0;
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	return sw->total;
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	const char *src = data;

	sw->total += len;
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - sw->used);

		memcpy(SiftPageGetData(sw->page) + sw->used, src, n);
		sw->used += n;
		src += n;
		len -= n;
		if (sw->used == SIFT_PAGE_PAYLOAD)
			sw_flush(sw);
	}
}

void
sift_sw_align(SiftStreamWriter *sw, size_t align)
{
	size_t		at = (SIFT_PAGE_DATA_OFFSET + sw->used) % align;

	static const char zeros[SIFT_SIMD_ALIGN];

	if (at)
		sift_sw_write(sw, zeros, align - at);
}

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	if (sw->used > 0 || sw->next == 0)
		sw_flush(sw);
	*nextents = sw->next;
	return sw->ext;
}
