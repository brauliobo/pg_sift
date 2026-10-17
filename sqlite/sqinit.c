/*
 * sqinit.c - the loadable extension: entry points, one-time engine initialization (CPU dispatch included) and the
 * SQL functions that are not tied to a cursor. A connection that loads the extension gets its own SqConn, the list of
 * its sift tables, so connections (and threads, one per connection or serialized) share no mutable state.
 */
#include "sqvtab.h"

#include <pthread.h>

#include "sift_bitmap.h"
#include "sift_cpu.h"
#include "sift_kernels.h"
#include "sift_prefix.h"

SQLITE_EXTENSION_INIT1

int			sift_max_expansions = 512;

/* ---------- engine: once per process ---------- */

/* sift_prefix.c keeps one process-wide cache of wide-prefix unions: its entry point (renamed by the Makefile) runs under a lock */
extern const uint8 *sq_real_sift_prefix_union(SiftSegment *seg, const SiftTermMatcher *m, const char *key, int keylen,
											  const SiftScoreRef *ref, bool scoring, MemoryContext mcxt, SiftTermInfo *info);
static pthread_mutex_t prefix_lock = PTHREAD_MUTEX_INITIALIZER;

const uint8 *
sift_prefix_union(SiftSegment *seg, const SiftTermMatcher *m, const char *key, int keylen, const SiftScoreRef *ref,
				  bool scoring, MemoryContext mcxt, SiftTermInfo *info)
{
	const uint8 *blob = NULL;

	pthread_mutex_lock(&prefix_lock);
	SQ_TRY();
	blob = sq_real_sift_prefix_union(seg, m, key, keylen, ref, scoring, mcxt, info);
	SQ_CATCH();
	pthread_mutex_unlock(&prefix_lock);
	sq_error_rethrow();
	SQ_END();
	pthread_mutex_unlock(&prefix_lock);
	return blob;
}

static const char *const level_names[] = {"scalar", "avx2", "avx512"};

/* The kernels (scoring, bitmaps) and the codec follow one level, capped by what the CPU has */
static SiftSimdLevel
set_level(SiftSimdLevel level)
{
	static const int isa[] = {SIFT_ISA_SCALAR, SIFT_ISA_AVX2, SIFT_ISA_AVX512};

	if (level > sift_cpu.detected)
		level = sift_cpu.detected;
	sift_cpu.active = level;
	sift_kern = sift_kernels_for(level);
	for (int l = level; !sift_set_isa(isa[l]); l--)
		;
	return level;
}

static int
parse_level(const char *name)
{
	if (name == NULL || strcmp(name, "auto") == 0)
		return SIFT_SIMD_AVX512;
	for (int i = 0; i < 3; i++)
		if (strcmp(name, level_names[i]) == 0)
			return i;
	return -1;
}

static pthread_once_t engine_once = PTHREAD_ONCE_INIT;

static void
engine_init(void)
{
	int			level = parse_level(getenv("SIFT_SIMD"));

	sq_mem_init();
	sift_cpu_init();
#ifdef SQ_SIMD_NONE
	sift_cpu.detected = SIFT_SIMD_SCALAR;
#endif
	sift_init();
	sift_kernels_init();
	set_level(level < 0 ? SIFT_SIMD_AVX512 : level);
}

/* ---------- SQL functions ---------- */

static SqVtab *
table_arg(sqlite3_context *ctx, sqlite3_value *name, const char *fn)
{
	SqVtab	   *vt = sq_find_table(sqlite3_user_data(ctx), (const char *) sqlite3_value_text(name));

	if (vt == NULL)
	{
		char	   *msg = sqlite3_mprintf("%s: no such sift table", fn);

		sqlite3_result_error(ctx, msg, -1);
		sqlite3_free(msg);
	}
	return vt;
}

/* sift_count(table, query): matching documents, from the bitmaps without visiting them */
static void
fn_count(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = table_arg(ctx, argv[0], "sift_count");
	int64		n = 0;
	MemoryContext old;
	SqStore    *prev;
	bool		failed = false;

	if (vt == NULL)
		return;
	old = sq_enter();
	prev = sq_store_enter(vt->st);
	SQ_TRY();
	{
		const char *q = (const char *) sqlite3_value_text(argv[1]);
		SiftIndexState *state;
		SiftQuery  *qs[1];
		SiftPlan   *plan;
		SiftVis		vis;

		if (!vt->in_txn)
			sq_store_refresh(vt->st, true);
		sq_vt_flush(vt, false);
		state = sift_state_open(sq_store_rel(vt->st), CurrentMemoryContext);
		qs[0] = sift_state_query(state, q ? q : "", sqlite3_value_bytes(argv[1]));
		plan = sift_plan_bind(state, qs, 1, 0);
		if (sq_par_available(vt, plan))
			n = sq_par_count(vt, &(SqQuery) {q ? q : "", sqlite3_value_bytes(argv[1])}, 1);
		else
		{
			memset(&vis, 0, sizeof(vis));
			vis.use_vm = true;
			n = sift_eval_count(plan, &vis, sq_vt_verify, vt);
		}
	}
	SQ_CATCH();
	failed = true;
	sq_store_recover(vt->st);
	SQ_END();
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	else
		sqlite3_result_int64(ctx, n);
	sq_store_leave(prev);
	sq_leave(old);
}

/* sift_stat(table): one line of facts about the table */
static void
fn_stat(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = table_arg(ctx, argv[0], "sift_stat");
	SqStoreStats s;
	const SiftDirSnapshot *d;
	char	   *msg;

	if (vt == NULL)
		return;
	d = sq_store_dir(vt->st);
	sq_store_stats(vt->st, &s);
	msg = sqlite3_mprintf("segments=%d docs=%llu chunks=%llu bytes=%llu chunk=%d cache_slots=%u cached_bytes=%llu "
						  "cache_hits=%llu cache_misses=%llu evictions=%llu pinned=%u pin_leaks=%llu pending_postings=%llu parallel_scans=%llu",
						  d->nsegs, (unsigned long long) d->meta.ndocs, (unsigned long long) s.chunks_total,
						  (unsigned long long) s.bytes_total, sq_store_chunk_bytes(vt->st), s.slots,
						  (unsigned long long) s.cached_bytes, (unsigned long long) s.hits, (unsigned long long) s.misses,
						  (unsigned long long) s.evictions, s.pinned, (unsigned long long) s.pin_leaks,
						  (unsigned long long) sq_inv_postings(vt->pend), (unsigned long long) vt->par_scans);
	sqlite3_result_text(ctx, msg, -1, sqlite3_free);
}

static void
fn_drop_cache(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = table_arg(ctx, argv[0], "sift_drop_cache");

	if (vt)
		sq_store_invalidate(vt->st);
}

static void
fn_isa(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	char	   *msg = sqlite3_mprintf("%s (cpu: %s)", level_names[sift_cpu.active], level_names[sift_cpu.detected]);

	sqlite3_result_text(ctx, msg, -1, sqlite3_free);
}

/* sift_simd(level): caps the kernels at scalar, avx2 or avx512 (auto: what the CPU has) for the whole process; call it when no query is running */
static void
fn_simd(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	int			level = parse_level((const char *) sqlite3_value_text(argv[0]));

	if (level < 0)
		sqlite3_result_error(ctx, "sift_simd: the level is scalar, avx2, avx512 or auto", -1);
	else
		sqlite3_result_text(ctx, level_names[set_level(level)], -1, SQLITE_STATIC);
}

static void
fn_version(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	char	   *msg = sqlite3_mprintf("sift-sqlite %s (engine format %d.%d, storage format %d)", SQ_VERSION, SIFT_FORMAT_MAJOR,
									  SIFT_FORMAT_MINOR, SQ_STORE_FORMAT);

	sqlite3_result_text(ctx, msg, -1, sqlite3_free);
}

/* The connections that loaded the extension (a second load on one is a no-op); probing with SQL would leave an error in the connection */
static pthread_mutex_t loaded_lock = PTHREAD_MUTEX_INITIALIZER;
static SqConn *loaded;

static void
conn_free(void *p)
{
	SqConn	  **pp;

	pthread_mutex_lock(&loaded_lock);
	for (pp = &loaded; *pp != p; pp = &(*pp)->next_loaded)
		;
	*pp = ((SqConn *) p)->next_loaded;
	pthread_mutex_unlock(&loaded_lock);
	sqlite3_free(p);
}

static int
create_functions(sqlite3 *db, SqConn *conn)
{
	static const struct
	{
		const char *name;
		int			nargs;
		void		(*fn) (sqlite3_context *, int, sqlite3_value **);
	}			fns[] = {
		{"sift_count", 2, fn_count}, {"sift_stat", 1, fn_stat}, {"sift_drop_cache", 1, fn_drop_cache},
		{"sift_isa", 0, fn_isa}, {"sift_simd", 1, fn_simd}, {"sift_version", 0, fn_version},
	};
	int			rc = SQLITE_OK;

	for (int i = 0; i < (int) lengthof(fns) && rc == SQLITE_OK; i++)
		rc = sqlite3_create_function(db, fns[i].name, fns[i].nargs, SQLITE_UTF8, conn, fns[i].fn, NULL, NULL);
	return rc;
}

/* ---------- entry points ---------- */

static bool
already_loaded(sqlite3 *db)
{
	SqConn	   *c;

	pthread_mutex_lock(&loaded_lock);
	for (c = loaded; c && c->db != db; c = c->next_loaded)
		;
	pthread_mutex_unlock(&loaded_lock);
	return c != NULL;
}

/* sqlite3_api is process-wide and is the same for every connection: the first load sets it, under a lock the others take to see it */
static void
set_api(const sqlite3_api_routines *pApi)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&lock);
	if (sqlite3_api == NULL)
		SQLITE_EXTENSION_INIT2(pApi);
	pthread_mutex_unlock(&lock);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int
sqlite3_sift_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
	SqConn	   *conn;
	int			rc;

	set_api(pApi);
	if (sqlite3_libversion_number() < 3038000)
	{
		*pzErrMsg = sqlite3_mprintf("sift needs SQLite 3.38 or newer (LIMIT/OFFSET pushdown), this is %s", sqlite3_libversion());
		return SQLITE_ERROR;
	}
	if (already_loaded(db))
		return SQLITE_OK;
	pthread_once(&engine_once, engine_init);
	conn = sqlite3_malloc(sizeof(SqConn));
	if (conn == NULL)
		return SQLITE_NOMEM;
	memset(conn, 0, sizeof(*conn));
	conn->db = db;
	pthread_mutex_lock(&loaded_lock);
	conn->next_loaded = loaded;
	loaded = conn;
	pthread_mutex_unlock(&loaded_lock);
	rc = sqlite3_create_module_v2(db, "sift", &sq_sift_module, conn, conn_free);
	if (rc == SQLITE_OK)
		rc = create_functions(db, conn);
	if (rc == SQLITE_OK)
		rc = sq_aux_init(db, conn);
	return rc;
}

/* The default entry point of sqlite3_load_extension, for a file whose name does not give sqlite3_sift_init */
#ifdef _WIN32
__declspec(dllexport)
#endif
int
sqlite3_extension_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
	return sqlite3_sift_init(db, pzErrMsg, pApi);
}
