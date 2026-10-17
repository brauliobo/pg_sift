/*
 * sqstore.c - sift_store.h over SQLite. A "relation" is the page file of one virtual table: block b is the row
 * b of the shadow table <name>_pages (8 KiB BLOB), cached in memory on first use and never evicted while the
 * table is open (pointers into pages are handed out, like PostgreSQL's shared buffers). Segments are immutable
 * runs of blocks; the manifest (<name>_segs, <name>_meta) is the directory and commits with the SQLite
 * transaction, so a rollback drops segments and block allocations with it. The LIVE pages of a segment
 * are edited in place in the cache and written back with UPDATE before the transaction commits.
 */
#include "sqapi.h"

SQLITE_EXTENSION_INIT3

#define SQ_PAGE_HDR		64

typedef struct SqPage
{
	uint32		blk;
	uint32		dirty;
	char		pad[SQ_PAGE_HDR - 8];
	char		data[BLCKSZ];
} SqPage;

struct SqStore
{
	sqlite3    *db;
	char	   *schema;
	char	   *name;
	SqOptions	opts;
	RelationData rel;
	/* page cache: pages[blk], valid blocks are < hw */
	SqPage	  **pages;
	uint32		npages_cap;
	uint64		npages_cached;
	uint64		pages_read;
	SqPage	  **dirty;
	int			ndirty,
				dirty_cap;
	SqPage	  **garbage;			/* replaced pages, freed when no cursor can hold a pointer to them */
	int			ngarbage,
				garbage_cap;
	/* directory */
	SiftDirSnapshot dir;
	BlockNumber hw;
	uint64		next_gen;
	uint32		epoch;
	uint32		serial;
	/* statements */
	sqlite3_stmt *get,
			   *put,
			   *upd,
			   *del,
			   *ver;
	sqlite3_blob *blob;
};

static uint32 store_serial;
int			sq_read_mode = 0;
int			sift_max_expansions = 512;

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

static void
exec(SqStore *st, const char *fmt, const char *a, const char *b)
{
	char	   *sql = sqlite3_mprintf(fmt, a, b);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
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
finalize_all(SqStore *st)
{
	sqlite3_finalize(st->get);
	sqlite3_finalize(st->put);
	sqlite3_finalize(st->upd);
	sqlite3_finalize(st->del);
	sqlite3_finalize(st->ver);
	st->get = st->put = st->upd = st->del = st->ver = NULL;
	sqlite3_blob_close(st->blob);
	st->blob = NULL;
}

/* ---------- lifecycle ---------- */

SqStore *
sq_store_new(sqlite3 *db, const char *schema, const char *name, const SqOptions *opts)
{
	SqStore    *st = calloc(1, sizeof(SqStore));

	st->db = db;
	st->schema = strdup(schema);
	st->name = strdup(name);
	st->opts = *opts;
	st->serial = ++store_serial;
	snprintf(st->rel.relname, sizeof(st->rel.relname), "%s", name);
	st->rel.store = st;
	st->rel.rd_locator.relNumber = st->serial << 12;
	return st;
}

static void
free_pages(SqPage **v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i]);
}

void
sq_store_release_garbage(SqStore *st)
{
	free_pages(st->garbage, st->ngarbage);
	st->ngarbage = 0;
}

void
sq_store_free(SqStore *st)
{
	finalize_all(st);
	for (uint32 i = 0; i < st->npages_cap; i++)
		free(st->pages[i]);
	free(st->pages);
	sq_store_release_garbage(st);
	free(st->garbage);
	free(st->dirty);
	free(st->schema);
	free(st->name);
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

void
sq_store_create_tables(SqStore *st)
{
	char	   *p = tbl(st, "pages"),
			   *s = tbl(st, "segs"),
			   *m = tbl(st, "meta");

	exec(st, "CREATE TABLE %s(blk INTEGER PRIMARY KEY, data BLOB)", p, NULL);
	exec(st, "CREATE TABLE %s(gen INTEGER PRIMARY KEY, header_blk, first_blk, sb_lo, sb_hi, ndocs, sum_dl, bytes, flags)", s, NULL);
	exec(st, "CREATE TABLE %s(id INTEGER PRIMARY KEY, version, hw, next_gen)", m, NULL);
	exec(st, "INSERT INTO %s VALUES(1, 1, 2, 1)", m, NULL);
	sqlite3_free(p);
	sqlite3_free(s);
	sqlite3_free(m);
}

void
sq_store_drop_tables(SqStore *st)
{
	finalize_all(st);
	exec(st, "DROP TABLE IF EXISTS %s", tbl(st, "pages"), NULL);
	exec(st, "DROP TABLE IF EXISTS %s", tbl(st, "segs"), NULL);
	exec(st, "DROP TABLE IF EXISTS %s", tbl(st, "meta"), NULL);
}

void
sq_store_rename(SqStore *st, const char *newname)
{
	finalize_all(st);
	free(st->name);
	st->name = strdup(newname);
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
load_meta(SqStore *st, uint32 *version)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "meta"),
			   *sql = sqlite3_mprintf("SELECT version, hw, next_gen FROM %s", t);
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
	st->hw = (BlockNumber) sqlite3_column_int64(q, 1);
	st->next_gen = sqlite3_column_int64(q, 2);
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

/* Cursors may still point into a cached page: it is freed later, when the table has no open cursor */
static void
drop_page(SqStore *st, uint32 blk)
{
	SqPage	   *p = st->pages[blk];

	if (p == NULL)
		return;
	for (int i = 0; p->dirty && i < st->ndirty; i++)
		if (st->dirty[i] == p)
			st->dirty[i] = st->dirty[--st->ndirty];
	if (st->ngarbage == st->garbage_cap)
	{
		st->garbage_cap = st->garbage_cap ? st->garbage_cap * 2 : 64;
		st->garbage = realloc(st->garbage, st->garbage_cap * sizeof(SqPage *));
	}
	st->garbage[st->ngarbage++] = p;
	st->pages[blk] = NULL;
	st->npages_cached--;
}

static void
invalidate_pages(SqStore *st, bool all)
{
	for (uint32 i = 0; i < st->npages_cap; i++)
		drop_page(st, i);
	st->ndirty = 0;
}

void
sq_store_invalidate(SqStore *st)
{
	invalidate_pages(st, true);
}

/* The committed (or own uncommitted) version of the manifest, which moves whenever a segment is published or retired */
static uint32
meta_version(SqStore *st)
{
	sqlite3_stmt *q = prep(st, &st->ver, "SELECT version FROM %s", "meta");
	int			rc = sqlite3_step(q);
	uint32		version = rc == SQLITE_ROW ? sqlite3_column_int(q, 0) : 0;

	sqlite3_reset(q);
	if (rc != SQLITE_ROW)
		elog(ERROR, "sift: cannot read the metadata of \"%s\": %s", st->name, sqlite3_errmsg(st->db));
	return version;
}

bool
sq_store_refresh(SqStore *st, bool changed_only)
{
	uint32		version;

	if (changed_only && st->dir.version != 0 && meta_version(st) == st->dir.version)
		return false;
	load_meta(st, &version);
	load_segs(st);
	st->dir.version = version;
	init_meta(st);
	/* another connection (or a rollback) changed the file: nothing cached can be trusted */
	invalidate_pages(st, true);
	st->epoch++;
	st->rel.rd_locator.relNumber = (st->serial << 12) + (st->epoch & 0xFFF);
	return true;
}

void
sq_store_rollback(SqStore *st)
{
	st->dir.version = 0;
	sq_store_refresh(st, false);
}

static void
persist_meta(SqStore *st)
{
	char	   *t = tbl(st, "meta"),
			   *sql = sqlite3_mprintf("UPDATE %s SET version = version + 1, hw = %u, next_gen = %llu", t,
									  st->hw, (unsigned long long) st->next_gen);
	int			rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	st->dir.version++;
	st->dir.meta.next_gen = st->next_gen;
	st->dir.meta.high_water = st->hw;
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
	persist_meta(st);
}

void
sq_store_retire(SqStore *st, uint64 gen)
{
	SiftDirSnapshot *d = &st->dir;
	int			i = 0;
	char	   *t;
	char	   *sql;
	int			rc;

	while (i < d->nsegs && d->segs[i].gen != gen)
		i++;
	if (i == d->nsegs)
		elog(ERROR, "sift: segment %llu is not in the directory", (unsigned long long) gen);
	t = tbl(st, "pages");
	sql = sqlite3_mprintf("DELETE FROM %s WHERE blk BETWEEN %u AND %u", t, d->segs[i].reserved, d->segs[i].header_blk);
	rc = sqlite3_exec(st->db, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	CHECK(st, rc);
	for (uint32 b = d->segs[i].reserved; b <= d->segs[i].header_blk && b < st->npages_cap; b++)
		drop_page(st, b);
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
	persist_meta(st);
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
	persist_meta(st);
}

/* ---------- pages ---------- */

static void
put_row(SqStore *st, sqlite3_stmt *q, BlockNumber blk, const void *data)
{
	sqlite3_bind_int64(q, 1, blk);
	sqlite3_bind_blob(q, 2, data, BLCKSZ, SQLITE_STATIC);
	{
		int			rc = sqlite3_step(q);

		sqlite3_reset(q);
		sqlite3_clear_bindings(q);
		CHECK(st, rc);
	}
}

void
sq_store_flush_pages(SqStore *st)
{
	for (int i = 0; i < st->ndirty; i++)
		if (st->dirty[i]->dirty)
		{
			put_row(st, prep(st, &st->upd, "UPDATE %s SET data = ?2 WHERE blk = ?1", "pages"), st->dirty[i]->blk,
					st->dirty[i]->data);
			st->dirty[i]->dirty = 0;
		}
	st->ndirty = 0;
	sqlite3_blob_close(st->blob);
	st->blob = NULL;
}

static void
read_page(SqStore *st, BlockNumber blk, char *dst)
{
	if (sq_read_mode == 1)
	{
		int			rc;

		if (st->blob)
			rc = sqlite3_blob_reopen(st->blob, blk);
		else
		{
			char	   *tn = sqlite3_mprintf("%s_pages", st->name);

			rc = sqlite3_blob_open(st->db, st->schema, tn, "data", blk, 0, &st->blob);
			sqlite3_free(tn);
		}
		if (rc == SQLITE_OK)
			rc = sqlite3_blob_read(st->blob, dst, BLCKSZ, 0);
		if (rc != SQLITE_OK)
		{
			sqlite3_blob_close(st->blob);
			st->blob = NULL;
			elog(ERROR, "sift: cannot read block %u: %s", blk, sqlite3_errmsg(st->db));
		}
		return;
	}
	{
		sqlite3_stmt *q = prep(st, &st->get, "SELECT data FROM %s WHERE blk = ?1", "pages");
		int			rc;

		sqlite3_bind_int64(q, 1, blk);
		rc = sqlite3_step(q);
		if (rc == SQLITE_ROW && sqlite3_column_bytes(q, 0) == BLCKSZ)
			memcpy(dst, sqlite3_column_blob(q, 0), BLCKSZ);
		sqlite3_reset(q);
		if (rc != SQLITE_ROW)
			elog(ERROR, "sift: block %u of \"%s\" is missing", blk, st->name);
	}
}

Buffer
ReadBuffer(Relation rel, BlockNumber blkno)
{
	SqStore    *st = rel->store;
	SqPage	   *p;

	if (blkno >= st->npages_cap)
	{
		uint32		cap = Max(1024, st->npages_cap);

		while (cap <= blkno)
			cap *= 2;
		st->pages = realloc(st->pages, cap * sizeof(SqPage *));
		memset(st->pages + st->npages_cap, 0, (cap - st->npages_cap) * sizeof(SqPage *));
		st->npages_cap = cap;
	}
	p = st->pages[blkno];
	if (p == NULL)
	{
		p = aligned_alloc(64, sizeof(SqPage));
		p->blk = blkno;
		p->dirty = 0;
		read_page(st, blkno, p->data);
		st->pages[blkno] = p;
		st->npages_cached++;
		st->pages_read++;
	}
	return p->data;
}

BlockNumber
BufferGetBlockNumber(Buffer buf)
{
	return ((SqPage *) (buf - SQ_PAGE_HDR))->blk;
}

GenericXLogState *
GenericXLogStart(Relation rel)
{
	return (GenericXLogState *) rel;
}

Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buf, int flags)
{
	Relation	rel = (Relation) state;
	SqStore    *st = rel->store;
	SqPage	   *p = (SqPage *) (buf - SQ_PAGE_HDR);

	if (!p->dirty)
	{
		p->dirty = 1;
		if (st->ndirty == st->dirty_cap)
		{
			st->dirty_cap = st->dirty_cap ? st->dirty_cap * 2 : 64;
			st->dirty = realloc(st->dirty, st->dirty_cap * sizeof(SqPage *));
		}
		st->dirty[st->ndirty++] = p;
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

void
sq_store_stats(SqStore *st, SqStoreStats *out)
{
	sqlite3_stmt *q;
	char	   *t = tbl(st, "pages"),
			   *sql = sqlite3_mprintf("SELECT count(*), coalesce(sum(length(data)), 0) FROM %s", t);

	memset(out, 0, sizeof(*out));
	out->pages_read = st->pages_read;
	out->pages_cached = st->npages_cached;
	if (sqlite3_prepare_v2(st->db, sql, -1, &q, NULL) == SQLITE_OK)
	{
		if (sqlite3_step(q) == SQLITE_ROW)
		{
			out->pages_total = sqlite3_column_int64(q, 0);
			out->bytes_total = sqlite3_column_int64(q, 1);
		}
		sqlite3_finalize(q);
	}
	sqlite3_free(sql);
	sqlite3_free(t);
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
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	for (int i = 0; i < nextents; i++)
	{
		sr->pagestart[i] = sr->npages;
		sr->npages += extents[i].nblocks;
	}
	sr->pagestart[nextents] = sr->npages;
}

static Buffer
sr_page(SiftStreamReader *sr, uint32 p)
{
	int			e = sr->cur;

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
	return ReadBuffer(sr->index, sr->extents[e].start + (p - sr->pagestart[e]));
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
}

/* ---------- write target and stream writer ---------- */

struct SiftWriteTarget
{
	SqStore    *st;
	BlockNumber first;
	BlockNumber next;
};

SiftWriteTarget *
sift_wt_begin(Relation index, SiftWriteMode mode)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));

	wt->st = index->store;
	wt->first = wt->next = index->store->hw;
	return wt;
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	wt->st->hw = wt->next;
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
	put_row(wt->st, prep(wt->st, &wt->st->put, "INSERT INTO %s VALUES(?1, ?2)", "pages"), blkno, page);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	BlockNumber blk = wt->next++;

	sift_wt_put_page(wt, blk, page);
	return blk;
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
