/*
 * write.c - the write path and the upkeep of the SQLite table: the analyzer and the in-memory buffer, flush and merge of segments,
 * xUpdate, the commands (INSERT INTO t(t, rank) VALUES('merge', 4)), rebuild and the integrity check.
 */
#include "vtab_int.h"

SQLITE_EXTENSION_INIT3

/* ---------- analyzer and indexing ---------- */

SiftAnalyzer *
sq_vt_analyzer(SqVtab *vt)
{
	if (vt->analyzer == NULL)
	{
		SiftAnalyzerConfig cfg;

		sift_analyzer_config_from_options(&vt->opts.base, &cfg);
		vt->acx = AllocSetContextCreate(NULL, "sift analyzer", ALLOCSET_DEFAULT_SIZES);
		vt->analyzer = sift_analyzer_create(&cfg, vt->acx);
		sift_analyzer_set_positions(vt->analyzer, vt->opts.base.positions);
	}
	return vt->analyzer;
}

static void
check_rowid(sqlite3_int64 rowid)
{
	if (rowid < 0 || rowid >= SQ_ROWID_MAX)
		elog(ERROR, "sift: rowid %lld is out of range (0 to %lld)", (long long) rowid, (long long) SQ_ROWID_MAX - 1);
}

static void
index_doc(SqVtab *vt, sqlite3_int64 rowid, sqlite3_value **cols)
{
	StringInfoData buf;
	SiftTermFreq *terms;
	uint32		dl;
	int			n;

	initStringInfo(&buf);
	for (int i = 0; i < vt->ncols; i++)
	{
		if (i)
			appendStringInfoChar(&buf, SIFT_FIELD_SEP);
		appendBinaryStringInfo(&buf, sqlite3_value_text(cols[i]), sqlite3_value_bytes(cols[i]));
	}
	n = sift_analyze_doc(sq_vt_analyzer(vt), buf.data, buf.len, &terms, &dl);
	sq_inv_add(vt->pend, sq_key(rowid), terms, n, dl);
	sift_analyzer_reset(sq_vt_analyzer(vt));
	pfree(buf.data);
}

/* ---------- segments: flush, merge ---------- */

static int
fanin(const SqVtab *vt)
{
	return vt->automerge >= 2 ? vt->automerge : SIFT_MERGE_FANIN;
}

/* Merge steps by the tier policy, at most max of them; each is a bounded rewrite of a few segments */
static int
merge_steps(SqVtab *vt, int64_t max)
{
	uint64		sel[SIFT_MAX_SEGMENTS];
	int64_t		steps = 0;

	while (steps < max)
	{
		int			n = sq_merge_pick(vt->st, false, fanin(vt), sel);

		if (n == 0)
			break;
		sq_merge(vt->st, sel, n);
		steps++;
	}
	return steps;
}

/* Everything into one segment, a few of the smallest at a time */
static void
optimize(SqVtab *vt)
{
	uint64		sel[SIFT_MAX_SEGMENTS];
	int			at_once = 2 * fanin(vt);

	for (;;)
	{
		int			n = sq_merge_pick(vt->st, true, 2, sel);

		if (n == 0)
			break;
		sq_merge(vt->st, sel, Min(n, at_once));
		if (n <= at_once)
			break;
	}
}

void
sq_vt_flush(SqVtab *vt, bool automerge)
{
	if (!sq_inv_empty(vt->pend))
	{
		if (sq_store_dir(vt->st)->nsegs >= SIFT_MAX_SEGMENTS - 1 && merge_steps(vt, 1) == 0)
			optimize(vt);
		sq_inv_write(vt->pend, vt->st);
	}
	else
		sq_inv_clear(vt->pend);
	sq_store_flush_pages(vt->st);
	if (automerge && vt->automerge >= 2)
		merge_steps(vt, INT64_MAX);
}

static bool
any_dirty(const SqVtab *vt)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].flags & SIFT_SEG_DIRTY)
			return true;
	return false;
}

/* ---------- writes ---------- */

/* The internal content table: INSERT or UPDATE of one row, whose rowid is returned */
static void
content_write(SqVtab *vt, sqlite3_int64 oldid, sqlite3_value *newid, sqlite3_value **cols, sqlite3_int64 *rowid)
{
	char	   *t = shadow(vt, "content"),
			   *cl = col_list(vt, ""),
			   *sql;
	sqlite3_stmt *q;
	int			rc;

	if (oldid >= 0)
	{
		char	   *set = sqlite3_mprintf("id = ?1");

		for (int i = 0; i < vt->ncols; i++)
		{
			char	   *s = sqlite3_mprintf("%s, c%d = ?%d", set, i, i + 3);

			sqlite3_free(set);
			set = s;
		}
		sql = sqlite3_mprintf("UPDATE %s SET %s WHERE id = ?2", t, set);
		sqlite3_free(set);
	}
	else
	{
		char	   *ph = sqlite3_mprintf("?1");

		for (int i = 0; i < vt->ncols; i++)
		{
			char	   *s = sqlite3_mprintf("%s, ?%d", ph, i + 3);

			sqlite3_free(ph);
			ph = s;
		}
		sql = sqlite3_mprintf("INSERT INTO %s(id, %s) VALUES(%s)", t, cl, ph);
		sqlite3_free(ph);
	}
	rc = sqlite3_prepare_v2(vt->db, sql, -1, &q, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	sqlite3_free(cl);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	if (sqlite3_value_type(newid) == SQLITE_NULL)
		sqlite3_bind_null(q, 1);
	else
		sqlite3_bind_value(q, 1, newid);
	if (oldid >= 0)
		sqlite3_bind_int64(q, 2, oldid);
	for (int i = 0; i < vt->ncols; i++)
		sqlite3_bind_value(q, i + 3, cols[i]);
	rc = sqlite3_step(q);
	if (rc != SQLITE_DONE)
	{
		char	   *msg = sqlite3_mprintf("%s", sqlite3_errmsg(vt->db));

		sqlite3_finalize(q);
		sq_error_set(ERRCODE_INTERNAL_ERROR, "%s", msg);
	}
	sqlite3_finalize(q);
	*rowid = oldid >= 0 && sqlite3_value_type(newid) == SQLITE_NULL ? oldid : sqlite3_last_insert_rowid(vt->db);
	if (oldid >= 0 && sqlite3_value_type(newid) != SQLITE_NULL)
		*rowid = sqlite3_value_int64(newid);
}

static void
index_remove(SqVtab *vt, sqlite3_int64 rowid)
{
	SiftKey		key = sq_key(rowid);

	if (!sq_inv_remove(vt->pend, key))
		sq_delete_key(vt->st, key);
}

static void
content_delete(SqVtab *vt, sqlite3_int64 rowid)
{
	char	   *t = shadow(vt, "content"),
			   *sql = sqlite3_mprintf("DELETE FROM %s WHERE id = %lld", t, (long long) rowid);
	int			rc = sqlite3_exec(vt->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
}

/* Reads every document of the content source again: the index is rebuilt from the text */
static void
rebuild(SqVtab *vt)
{
	sqlite3_stmt *q;
	SiftDirSnapshot dir;

	sq_inv_clear(vt->pend);
	sq_store_flush_pages(vt->st);
	dir = *sq_store_dir(vt->st);
	for (int i = 0; i < dir.nsegs; i++)
		sq_store_retire(vt->st, dir.segs[i].gen);
	q = source_stmt(vt, SRC_ALL, 0);
	while (sqlite3_step(q) == SQLITE_ROW)
	{
		sqlite3_value *cols[SIFT_MAX_FIELDS];

		for (int i = 0; i < vt->ncols; i++)
			cols[i] = sqlite3_column_value(q, i + 1);
		check_rowid(sqlite3_column_int64(q, 0));
		index_doc(vt, sqlite3_column_int64(q, 0), cols);
		if ((int64_t) sq_inv_postings(vt->pend) > vt->pending_postings)
			sq_vt_flush(vt, false);
	}
	sqlite3_finalize(q);
	sq_vt_flush(vt, true);
}

/* ---------- integrity check ---------- */

#define BAD(...) sq_error_set(ERRCODE_INDEX_CORRUPTED, "sift: integrity-check: " __VA_ARGS__)

static void
check_rows(SqVtab *vt, const SiftSegEntry *e)
{
	int64_t		lo,
				hi,
				rows,
				bytes,
				blocks = (int64_t) e->header_blk - e->reserved + 1,
				chunk = sq_store_chunk_bytes(vt->st);

	sq_store_seg_rows(vt->st, e, &lo, &hi);
	if (!sq_store_rows_between(vt->st, lo, hi, &rows, &bytes))
		BAD("cannot read the chunks of segment %llu", (unsigned long long) e->gen);
	if (rows != (blocks * BLCKSZ + chunk - 1) / chunk || bytes != blocks * BLCKSZ)
		BAD("segment %llu has %lld chunk rows of %lld bytes, the manifest says %lld blocks", (unsigned long long) e->gen,
			(long long) rows, (long long) bytes, (long long) blocks);
}

static uint64
check_segment(SqVtab *vt, const SiftSegEntry *e)
{
	const SqOptions *o = sq_store_options(vt->st);
	SiftScoreRef ref = {(float4) o->base.k1, (float4) o->base.b, 0};
	SiftSegment *seg;
	SiftSegStats stats;
	uint64		live;

	check_rows(vt, e);
	seg = sift_seg_open(sq_store_rel(vt->st), e, CurrentMemoryContext);
	if (seg->hdr->gen != e->gen || seg->hdr->ndocs != e->ndocs || seg->hdr->sum_dl != e->sum_dl)
		BAD("segment %llu holds %llu documents, the manifest %llu", (unsigned long long) e->gen,
			(unsigned long long) seg->hdr->ndocs, (unsigned long long) e->ndocs);
	sift_seg_walk(seg, &ref, true, &stats, NULL, NULL);
	live = sift_seg_live_docs(seg);
	sift_seg_close(seg);
	return live;
}

typedef struct DocCheck
{
	sqlite3_stmt *q;
	uint64		missing;
	int64_t		first;
} DocCheck;

static void
check_doc(void *arg, SiftKey key, uint16 dl)
{
	DocCheck   *d = arg;
	int64_t		id = sq_key_rowid(key);

	sqlite3_bind_int64(d->q, 1, id);
	if (sqlite3_step(d->q) != SQLITE_ROW && d->missing++ == 0)
		d->first = id;
	sqlite3_reset(d->q);
}

/* Every live document of the index is in the text, and the text has no others */
static void
check_content(SqVtab *vt, uint64 live)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);
	DocCheck	d = {0};
	sqlite3_stmt *c;
	int64_t		n = 0;

	d.q = source_stmt(vt, SRC_ID, 0);
	for (int i = 0; i < dir->nsegs; i++)
		sq_seg_live_docs(vt->st, &dir->segs[i], check_doc, &d);
	sqlite3_finalize(d.q);
	c = source_stmt(vt, SRC_IDS, 0);
	while (sqlite3_step(c) == SQLITE_ROW)
		n++;
	sqlite3_finalize(c);
	if (d.missing)
		BAD("%llu indexed documents are not in the text (first: rowid %lld)", (unsigned long long) d.missing, (long long) d.first);
	if (n != (int64_t) live)
		BAD("the text has %lld documents, the index %llu", (long long) n, (unsigned long long) live);
}

/* The manifest, every segment, the chunk rows and (deep) the text: what the engine reads must be what was written */
static void
integrity_check(SqVtab *vt, bool deep)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);
	uint64		live = 0;
	int64_t		rows = 0;

	sq_vt_flush(vt, false);
	for (int i = 0; i < dir->nsegs; i++)
	{
		int64_t		lo,
					hi,
					n,
					b;

		live += check_segment(vt, &dir->segs[i]);
		sq_store_seg_rows(vt->st, &dir->segs[i], &lo, &hi);
		sq_store_rows_between(vt->st, lo, hi, &n, &b);
		rows += n;
		if (dir->segs[i].gen >= dir->meta.next_gen || dir->segs[i].header_blk >= sq_store_hw(vt->st))
			BAD("segment %llu is past the high-water mark", (unsigned long long) dir->segs[i].gen);
	}
	if (sq_store_rows_total(vt->st) != rows)
		BAD("the chunk table has %lld rows, the segments account for %lld", (long long) sq_store_rows_total(vt->st),
			(long long) rows);
	if (deep && vt->content != SQ_CONTENT_NONE)
		check_content(vt, live);
}

/* ---------- commands ---------- */

static int64_t
arg_int(sqlite3_value *v, int64_t dflt)
{
	return sqlite3_value_type(v) == SQLITE_NULL ? dflt : sqlite3_value_int64(v);
}

static void
remove_by_command(SqVtab *vt, sqlite3_value *rowid)
{
	if (vt->content == SQ_CONTENT_INTERNAL)
		elog(ERROR, "sift: 'delete' is for tables with external or no content; use DELETE FROM");
	if (sqlite3_value_type(rowid) == SQLITE_NULL)
		elog(ERROR, "sift: 'delete' needs the rowid");
	index_remove(vt, sqlite3_value_int64(rowid));
}

static void
delete_all(SqVtab *vt)
{
	SiftDirSnapshot dir = *sq_store_dir(vt->st);

	sq_inv_clear(vt->pend);
	for (int i = 0; i < dir.nsegs; i++)
		sq_store_retire(vt->st, dir.segs[i].gen);
	if (vt->content == SQ_CONTENT_INTERNAL)
	{
		char	   *t = shadow(vt, "content"),
				   *sql = sqlite3_mprintf("DELETE FROM %s", t);

		sqlite3_exec(vt->db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
		sqlite3_free(t);
	}
}

static void
setting(SqVtab *vt, const char *cmd, sqlite3_value *arg)
{
	int64_t		n = arg_int(arg, -1);

	if (n < 0)
		elog(ERROR, "sift: '%s' needs a non-negative value in the rank column", cmd);
	if (strcmp(cmd, "cache-size") == 0)
	{
		vt->cache_kib = n;
		sq_store_set_cache(vt->st, vt->cache_kib);
	}
	else if (strcmp(cmd, "parallel") == 0)
		vt->parallel = n;
	else if (strcmp(cmd, "automerge") == 0)
	{
		vt->automerge = n;
		sq_store_config_set(vt->st, cmd, n);
	}
	else
	{
		vt->pending_postings = n;
		sq_store_config_set(vt->st, cmd, n);
	}
}

static void
command(SqVtab *vt, const char *cmd, sqlite3_value *arg, sqlite3_value *rowid)
{
	if (strcmp(cmd, "merge") == 0)
	{
		int64_t		steps = arg_int(arg, 0);

		sq_vt_flush(vt, false);
		merge_steps(vt, steps > 0 ? steps : INT64_MAX);
	}
	else if (strcmp(cmd, "optimize") == 0)
	{
		sq_vt_flush(vt, false);
		if (sq_store_dir(vt->st)->nsegs > 1 || any_dirty(vt))
			optimize(vt);
	}
	else if (strcmp(cmd, "flush") == 0)
		sq_vt_flush(vt, true);
	else if (strcmp(cmd, "rebuild") == 0)
		rebuild(vt);
	else if (strcmp(cmd, "integrity-check") == 0)
		integrity_check(vt, arg_int(arg, 0) != 0);
	else if (strcmp(cmd, "delete") == 0)
		remove_by_command(vt, rowid);
	else if (strcmp(cmd, "delete-all") == 0)
		delete_all(vt);
	else if (strcmp(cmd, "cache-size") == 0 || strcmp(cmd, "parallel") == 0 || strcmp(cmd, "automerge") == 0 ||
			 strcmp(cmd, "pending-postings") == 0)
		setting(vt, cmd, arg);
	else if (strcmp(cmd, "x-fault") == 0)
		sq_store_set_fault(vt->st, (int) (arg_int(arg, 0) / 1000000), arg_int(arg, 0) % 1000000);
	else
		elog(ERROR, "sift: unknown command \"%s\"", cmd);
}

static void
insert_doc(SqVtab *vt, sqlite3_int64 oldid, sqlite3_value *newid, sqlite3_value **cols, sqlite3_int64 *rowid)
{
	if (vt->content == SQ_CONTENT_INTERNAL)
		content_write(vt, oldid, newid, cols, rowid);
	else if (sqlite3_value_type(newid) == SQLITE_NULL)
		elog(ERROR, "sift: a table with external or no content needs a rowid");
	else
		*rowid = sqlite3_value_int64(newid);
	check_rowid(*rowid);
	index_doc(vt, *rowid, cols);
	if ((int64_t) sq_inv_postings(vt->pend) > vt->pending_postings)
		sq_vt_flush(vt, false);
}

/* INSERT OR REPLACE with a rowid that exists: the old document goes first */
static void
replace_existing(SqVtab *vt, sqlite3_value *newid)
{
	if (sqlite3_vtab_on_conflict(vt->db) != SQLITE_REPLACE || sqlite3_value_type(newid) == SQLITE_NULL)
		return;
	index_remove(vt, sqlite3_value_int64(newid));
	if (vt->content == SQ_CONTENT_INTERNAL)
		content_delete(vt, sqlite3_value_int64(newid));
}

int
sqUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite3_int64 *pRowid)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		int			nc = vt->ncols;

		if (argc == 1)
		{
			index_remove(vt, sqlite3_value_int64(argv[0]));
			if (vt->content == SQ_CONTENT_INTERNAL)
				content_delete(vt, sqlite3_value_int64(argv[0]));
		}
		else if (sqlite3_value_type(argv[2 + nc]) != SQLITE_NULL)
			command(vt, (const char *) sqlite3_value_text(argv[2 + nc]), argv[3 + nc], argv[1]);
		else
		{
			sqlite3_int64 oldid = sqlite3_value_type(argv[0]) == SQLITE_NULL ? -1 : sqlite3_value_int64(argv[0]);

			if (oldid >= 0)
				index_remove(vt, oldid);
			else
				replace_existing(vt, argv[1]);
			if (sqlite3_value_type(argv[1]) != SQLITE_NULL)
				check_rowid(sqlite3_value_int64(argv[1]));
			insert_doc(vt, oldid, argv[1], argv + 2, pRowid);
		}
	}
	TRY_END(vt)
}
