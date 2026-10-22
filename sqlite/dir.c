/*
 * dir.c - the directory of a SQLite table's page store: the manifest (<name>_segs, <name>_meta), its refresh, publish and
 * retire, and core/store.h's directory calls.
 */
#include "store_int.h"

SQLITE_EXTENSION_INIT3

/* ---------- directory ---------- */

BlockNumber
sq_store_hw(const SqStore *st)
{
	return st->hw;
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
		e->format = SIFT_WRITE_SEGFMT;
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
	m->format_minor = SIFT_WRITE_SEGFMT;
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
	close_blobs(st);
}

void
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

/* ---------- core/store.h: pages, directory ---------- */

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
