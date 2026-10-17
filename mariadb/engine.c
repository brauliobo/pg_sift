/*
 * engine.c - indexes of segment files driven by the engine of src/: feed (drain), query, merge, persistence.
 *
 * Documents. A queue row (seq, op, pk, text) becomes document `seq` (its docid): the engine never renumbers, so a
 * row updated or deleted later leaves its old docid behind as a dead document, until a merge drops it. The mapping
 * docid -> (page, offset) is dpp documents per page (sift_docid_key). The primary key of a document comes from the
 * event log, which holds every applied event (seq, pk, op) and is replayed at open into
 *   latest : pk -> docid of the live version,    dead : sorted docids that are superseded or deleted,
 *   ids/pks: docid -> pk of every document in the segments.
 * Durability. A drain writes its segment, appends its events to the log and replaces MANIFEST (write, fsync, rename):
 * the manifest names the segments, the log length and the highest applied seq, so a crash at any point leaves the
 * previous state (files it does not name are removed at open) and a drain repeated after the commit skips what the
 * watermark covers.
 * The engine of src/ is not thread safe: one lock serializes all calls.
 */
#include "postgres.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/memutils.h"

#include "sift_analyze.h"
#include "sift_bitmap.h"
#include "sift_build.h"
#include "sift_cpu.h"
#include "sift_eval.h"
#include "sift_kernels.h"
#include "engine.h"
#include "shim.h"

#define MAN_MAGIC		"SIFTMDB1"
#define BUILD_BUDGET	((Size) 512 << 20)

typedef struct Ev
{
	int64		seq;
	int64		pk;
	int64		op;				/* 'I' or 'D' */
} Ev;

typedef struct ManSeg
{
	SiftSegEntry e;
	uint32		slot;
	uint32		pad;
	char		file[48];
} ManSeg;

typedef struct Manifest
{
	char		magic[8];
	uint64		version;
	uint64		next_gen;
	uint64		log_len;
	int64		drained;
	uint32		dpp;
	uint32		nsegs;
	uint32		logno;
	uint32		pad;
	float		k1;
	float		b;
} Manifest;

typedef struct Slot				/* open addressing pk -> docid; 0 = no live document */
{
	int64		key;
	int64		val;
	uint8		used;
} Slot;

struct SiftIdx
{
	char		name[64];
	char		dir[512];
	Relation	rel;
	SiftRel    *sr;
	Manifest	man;
	ManSeg		segs[SIFT_MAX_SEGMENTS];
	int64	   *ids,
			   *pks;
	size_t		ndoc,
				capdoc;
	int64	   *dead;
	size_t		ndead,
				capdead;
	Slot	   *latest;
	size_t		caplatest,
				nlatest;
	int64		bytes;
};

typedef struct Registry
{
	SiftIdx    *ix[64];
	int			n;
} Registry;

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static Registry reg;
static char base_dir[400] = ".";
static char tmp_dir[400] = "/tmp";
static bool inited;

/* ------------------------------------------------ small helpers */

static void
fail(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(sift_err_msg, sizeof(sift_err_msg), fmt, ap);
	va_end(ap);
	siglongjmp(*sift_err_jmp, 1);
}

static void
codec_rebind(const SiftCpu *cpu)
{
	static const int isa[] = {SIFT_ISA_SCALAR, SIFT_ISA_AVX2, SIFT_ISA_AVX512};

	for (int level = cpu->active; !sift_set_isa(isa[level]); level--)
		;
}

static void
engine_init(void)
{
	if (inited)
		return;
	sift_shim_init();
	pfree(palloc(1));			/* creates TopMemoryContext */
	sift_tmpdir = tmp_dir;
	sift_cpu_init();
	sift_init();
	sift_cpu_register(codec_rebind);
	sift_kernels_init();
	inited = true;
}

static int
end_call(int rc, char *err, size_t errlen)
{
	if (rc != 0 && err)
		snprintf(err, errlen, "%s", sift_err_msg);
	pthread_mutex_unlock(&big);
	return rc;
}

static MemoryContext
op_begin(void)
{
	MemoryContext cx = AllocSetContextCreate(TopMemoryContext, "sift op", ALLOCSET_DEFAULT_SIZES);

	return cx;
}

static void
op_end(MemoryContext cx)
{
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(cx);
}

static void
path_of(const SiftIdx *ix, char *out, size_t n, const char *file)
{
	snprintf(out, n, "%s/%s", ix->dir, file);
}

static void
sync_dir(const char *dir)
{
	int			fd = open(dir, O_RDONLY);

	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
}

/* ------------------------------------------------ documents: latest, dead, ids/pks */

static bool
is_dead(void *ctx, int64 docid)
{
	const SiftIdx *ix = ctx;
	size_t		lo = 0,
				hi = ix->ndead;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (ix->dead[mid] < docid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < ix->ndead && ix->dead[lo] == docid;
}

static void
sync_dead(SiftIdx *ix)
{
	ix->sr->is_dead = is_dead;
	ix->sr->dead_ctx = ix;
	ix->sr->any_dead = ix->ndead > 0;
}

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

static void
latest_grow(SiftIdx *ix)
{
	Slot	   *old = ix->latest;
	size_t		oldcap = ix->caplatest;

	ix->caplatest = oldcap ? oldcap * 2 : 1024;
	ix->latest = calloc(ix->caplatest, sizeof(Slot));
	ix->nlatest = 0;
	for (size_t i = 0; i < oldcap; i++)
		if (old[i].used && old[i].val)
		{
			size_t		h = ((uint64) old[i].key * 0x9E3779B97F4A7C15ULL) >> 20;

			while (ix->latest[h % ix->caplatest].used)
				h++;
			ix->latest[h % ix->caplatest] = old[i];
			ix->nlatest++;
		}
	free(old);
}

/* The slot of pk, or the free one it would take */
static Slot *
latest_slot(SiftIdx *ix, int64 pk)
{
	size_t		h = ((uint64) pk * 0x9E3779B97F4A7C15ULL) >> 20;

	for (;; h++)
	{
		Slot	   *s = &ix->latest[h % ix->caplatest];

		if (!s->used || s->key == pk)
			return s;
	}
}

/* Applies one event: the previous live version of pk dies, an insert becomes the live one */
static void
apply_event(SiftIdx *ix, int64 seq, int64 pk, int64 op)
{
	Slot	   *s;

	if ((ix->nlatest + 1) * 2 > ix->caplatest)
		latest_grow(ix);
	s = latest_slot(ix, pk);
	if (s->used && s->val)
	{
		if (ix->ndead == ix->capdead)
			ix->dead = realloc(ix->dead, (ix->capdead = ix->capdead ? ix->capdead * 2 : 1024) * sizeof(int64));
		ix->dead[ix->ndead++] = s->val;
	}
	if (!s->used)
		ix->nlatest++;
	s->used = 1;
	s->key = pk;
	s->val = op == 'I' ? seq : 0;
	if (op == 'I')
	{
		if (ix->ndoc == ix->capdoc)
		{
			ix->capdoc = ix->capdoc ? ix->capdoc * 2 : 1024;
			ix->ids = realloc(ix->ids, ix->capdoc * sizeof(int64));
			ix->pks = realloc(ix->pks, ix->capdoc * sizeof(int64));
		}
		ix->ids[ix->ndoc] = seq;
		ix->pks[ix->ndoc++] = pk;
	}
}

static void
finish_events(SiftIdx *ix)
{
	qsort(ix->dead, ix->ndead, sizeof(int64), cmp_i64);
	sync_dead(ix);
}

static int64
pk_of(const SiftIdx *ix, int64 docid)
{
	size_t		lo = 0,
				hi = ix->ndoc;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (ix->ids[mid] < docid)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == ix->ndoc || ix->ids[lo] != docid)
		fail("sift: document %lld has no row key in the event log", (long long) docid);
	return ix->pks[lo];
}

/* ------------------------------------------------ persistence */

static void
write_all(int fd, const void *p, size_t n, const char *what)
{
	if (write(fd, p, n) != (ssize_t) n)
		fail("sift: could not write %s: %s", what, strerror(errno));
}

static void
write_manifest(SiftIdx *ix, const Manifest *m, const ManSeg *segs)
{
	char		tmp[600],
				final[600];
	int			fd;

	path_of(ix, tmp, sizeof(tmp), "MANIFEST.tmp");
	path_of(ix, final, sizeof(final), "MANIFEST");
	fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not create %s: %s", tmp, strerror(errno));
	write_all(fd, m, sizeof(Manifest), "the manifest");
	write_all(fd, segs, m->nsegs * sizeof(ManSeg), "the manifest");
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", tmp, strerror(errno));
	close(fd);
	if (rename(tmp, final) != 0)
		fail("sift: could not rename %s: %s", tmp, strerror(errno));
	sync_dir(ix->dir);
}

static void
log_path(const SiftIdx *ix, uint32 logno, char *out, size_t n)
{
	snprintf(out, n, "%s/events-%u.log", ix->dir, logno);
}

static void
append_events(SiftIdx *ix, const Ev *ev, size_t n)
{
	char		path[600];
	int			fd;

	log_path(ix, ix->man.logno, path, sizeof(path));
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not open %s: %s", path, strerror(errno));
	if (lseek(fd, ix->man.log_len, SEEK_SET) < 0 || ftruncate(fd, ix->man.log_len) != 0)
		fail("sift: could not position %s: %s", path, strerror(errno));
	write_all(fd, ev, n * sizeof(Ev), path);
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", path, strerror(errno));
	close(fd);
}

static void
replay_log(SiftIdx *ix)
{
	char		path[600];
	int			fd;
	Ev			ev[4096];
	uint64		left = ix->man.log_len;

	log_path(ix, ix->man.logno, path, sizeof(path));
	fd = open(path, O_RDONLY);
	if (fd < 0 && left > 0)
		fail("sift: event log %s is missing", path);
	while (left > 0)
	{
		ssize_t		got = read(fd, ev, Min(left, sizeof(ev)));

		if (got <= 0 || got % sizeof(Ev))
			fail("sift: event log %s is truncated", path);
		for (size_t i = 0; i < got / sizeof(Ev); i++)
			apply_event(ix, ev[i].seq, ev[i].pk, ev[i].op);
		left -= got;
	}
	if (fd >= 0)
		close(fd);
	finish_events(ix);
}

static void
load_manifest(SiftIdx *ix)
{
	char		path[600];
	int			fd;

	path_of(ix, path, sizeof(path), "MANIFEST");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail("sift: index \"%s\" does not exist (%s)", ix->name, path);
	if (read(fd, &ix->man, sizeof(Manifest)) != sizeof(Manifest) || memcmp(ix->man.magic, MAN_MAGIC, 8) != 0 ||
		ix->man.nsegs > SIFT_MAX_SEGMENTS ||
		read(fd, ix->segs, ix->man.nsegs * sizeof(ManSeg)) != (ssize_t) (ix->man.nsegs * sizeof(ManSeg)))
		fail("sift: manifest %s is corrupted", path);
	close(fd);
}

/* Files the manifest does not name are leftovers of an interrupted drain or merge */
static void
remove_orphans(SiftIdx *ix)
{
	DIR		   *d = opendir(ix->dir);
	struct dirent *e;
	char		keep_log[48];

	snprintf(keep_log, sizeof(keep_log), "events-%u.log", ix->man.logno);
	while (d && (e = readdir(d)) != NULL)
	{
		bool		keep = e->d_name[0] == '.' || strcmp(e->d_name, "MANIFEST") == 0 || strcmp(e->d_name, keep_log) == 0;
		char		path[600];

		for (uint32 i = 0; i < ix->man.nsegs; i++)
			keep |= strcmp(e->d_name, ix->segs[i].file) == 0;
		if (!keep)
		{
			path_of(ix, path, sizeof(path), e->d_name);
			unlink(path);
		}
	}
	if (d)
		closedir(d);
}

static void
init_rel(SiftIdx *ix)
{
	ix->rel = sift_rel_create(ix->name, ix->dir, ix->man.dpp, &ix->sr);
	ix->sr->opts.k1 = ix->man.k1;
	ix->sr->opts.b = ix->man.b;
	sync_dead(ix);
}

static void
map_segments(SiftIdx *ix)
{
	ix->bytes = 0;
	for (uint32 i = 0; i < ix->man.nsegs; i++)
	{
		char		path[600];

		path_of(ix, path, sizeof(path), ix->segs[i].file);
		sift_slot_map(ix->sr, ix->segs[i].slot, path);
		ix->bytes += (int64) ix->sr->slot_blocks[ix->segs[i].slot] * BLCKSZ;
	}
}

static void
dir_of(const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", base_dir, name);
}

static void
do_open(SiftIdx *ix)
{
	load_manifest(ix);
	remove_orphans(ix);
	init_rel(ix);
	map_segments(ix);
	replay_log(ix);
}

/* ------------------------------------------------ public: lifecycle */

void
sx_set_dirs(const char *base, const char *tmp)
{
	snprintf(base_dir, sizeof(base_dir), "%s", base);
	snprintf(tmp_dir, sizeof(tmp_dir), "%s", tmp);
}

static SiftIdx *
registered(const char *name)
{
	for (int i = 0; i < reg.n; i++)
		if (strcmp(reg.ix[i]->name, name) == 0)
			return reg.ix[i];
	return NULL;
}

static void
do_create(SiftIdx *ix, int dpp)
{
	char		d[600];
	Manifest	m = {.magic = MAN_MAGIC,.dpp = dpp,.k1 = SIFT_DEFAULT_K1,.b = SIFT_DEFAULT_B,.next_gen = 1};

	if (dpp < 1 || dpp > 256)
		fail("sift: documents per page must be between 1 and 256");
	if (mkdir(ix->dir, 0755) != 0 && errno != EEXIST)
		fail("sift: could not create %s: %s", ix->dir, strerror(errno));
	path_of(ix, d, sizeof(d), "MANIFEST");
	if (access(d, F_OK) == 0)
		fail("sift: index \"%s\" already exists", ix->name);
	write_manifest(ix, &m, NULL);
}

int
sx_create(const char *name, int dpp, char *err, size_t errlen)
{
	SiftIdx		ix = {0};
	int			rc;

	pthread_mutex_lock(&big);
	engine_init();
	snprintf(ix.name, sizeof(ix.name), "%s", name);
	dir_of(name, ix.dir, sizeof(ix.dir));
	rc = SIFT_TRY(do_create(&ix, dpp));
	return end_call(rc, err, errlen);
}

static void
free_idx(SiftIdx *ix)
{
	if (ix->rel)
		sift_rel_destroy(ix->rel);
	free(ix->ids);
	free(ix->pks);
	free(ix->dead);
	free(ix->latest);
	free(ix);
}

static void
do_drop(const char *name, const char *dir)
{
	DIR		   *d = opendir(dir);
	struct dirent *e;
	SiftIdx    *ix = registered(name);

	if (ix)
	{
		for (int i = 0; i < reg.n; i++)
			if (reg.ix[i] == ix)
				reg.ix[i] = reg.ix[--reg.n];
		free_idx(ix);
	}
	if (d == NULL)
		fail("sift: index \"%s\" does not exist", name);
	while ((e = readdir(d)) != NULL)
	{
		char		path[600];

		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	rmdir(dir);
}

int
sx_drop(const char *name, char *err, size_t errlen)
{
	char		dir[600];
	int			rc;

	pthread_mutex_lock(&big);
	engine_init();
	dir_of(name, dir, sizeof(dir));
	rc = SIFT_TRY(do_drop(name, dir));
	return end_call(rc, err, errlen);
}

SiftIdx *
sx_open(const char *name, char *err, size_t errlen)
{
	SiftIdx    *ix;
	int			rc = 0;

	pthread_mutex_lock(&big);
	engine_init();
	if ((ix = registered(name)) == NULL)
	{
		ix = calloc(1, sizeof(SiftIdx));
		snprintf(ix->name, sizeof(ix->name), "%s", name);
		dir_of(name, ix->dir, sizeof(ix->dir));
		rc = SIFT_TRY(do_open(ix));
		if (rc == 0)
			reg.ix[reg.n++] = ix;
		else
		{
			if (err)
				snprintf(err, errlen, "%s", sift_err_msg);
			free_idx(ix);
			ix = NULL;
		}
	}
	pthread_mutex_unlock(&big);
	return ix;
}

/* ------------------------------------------------ queries */

typedef struct SxQuery
{
	SiftIdx    *ix;
	const char *q;
	int			qlen;
	int			k;
	SxHit    *hits;
	int			n;
	int64		count;
	int64	   *pks;
	int64		npks,
				cappks;
	SiftDirSnapshot snap;
} SxQuery;

static bool
verify_cb(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SxQuery	   *q = arg;

	if (want_doc)
		fail("sift: this phrase needs the text of the row (a phrase under OR or NOT is not supported)");
	return !is_dead(q->ix, sift_key_docid(q->ix->sr, sift_key_from_tid(tid)));
}

static SiftIndexState *
open_state(SxQuery *q, MemoryContext cx)
{
	SiftIdx    *ix = q->ix;
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_from_options(&ix->sr->opts, &cfg);
	q->snap.meta.analyzer_hash = cfg.hash;
	q->snap.meta.k1 = ix->man.k1;
	q->snap.meta.b = ix->man.b;
	q->snap.nsegs = ix->man.nsegs;
	for (uint32 i = 0; i < ix->man.nsegs; i++)
		q->snap.segs[i] = ix->segs[i].e;
	return sift_state_open_dir(ix->rel, cx, &q->snap);
}

static SiftPlan *
bind_query(SxQuery *q, SiftIndexState *st, int flags)
{
	SiftQuery  *query = sift_state_query(st, q->q, q->qlen);

	return sift_plan_bind(st, &query, 1, flags);
}

static void
run_topk(SxQuery *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, SIFT_BIND_SCORE);
	SiftRanker *r = sift_rank_begin(plan, q->k, verify_cb, q);
	ItemPointerData tid;
	float4		score;

	while (q->n < q->k && sift_rank_next(r, &tid, &score))
	{
		q->hits[q->n].pk = pk_of(q->ix, sift_key_docid(q->ix->sr, sift_key_from_tid(&tid)));
		q->hits[q->n++].score = score;
	}
	sift_rank_end(r);
	sift_state_close(st);
}

static void
run_count(SxQuery *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, 0);
	SiftVis		vis = {.heap = q->ix->rel};

	q->count = sift_eval_count(plan, &vis, verify_cb, q);
	sift_state_close(st);
}

static void
collect_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	SxQuery	   *q = arg;

	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
		for (uint64 m = offmask[w]; m; m &= m - 1)
		{
			int64		docid = (int64) blk * q->ix->sr->dpp + w * 64 + __builtin_ctzll(m);

			if (q->npks == q->cappks)
				q->pks = realloc(q->pks, (q->cappks = q->cappks ? q->cappks * 2 : 1024) * sizeof(int64));
			q->pks[q->npks++] = pk_of(q->ix, docid);
		}
}

static void
run_matches(SxQuery *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, 0);
	SiftVis		vis = {.heap = q->ix->rel};

	sift_eval_pages(plan, &vis, verify_cb, q, collect_page, q);
	sift_state_close(st);
}

int
sx_matches(SiftIdx *ix, const char *qs, int qlen, int64_t **pks, int64_t *n, char *err, size_t errlen)
{
	SxQuery		q = {.ix = ix,.q = qs,.qlen = qlen};
	MemoryContext cx;
	int			rc;

	pthread_mutex_lock(&big);
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_matches(&q, cx));
	op_end(cx);
	*pks = q.pks;
	*n = q.npks;
	return end_call(rc, err, errlen);
}

/* ------------------------------------------------ feed: a drain becomes one segment */

struct SiftFeed
{
	SiftIdx    *ix;
	MemoryContext cx;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	SiftRun   **runs;
	int			nruns,
				maxruns;
	Ev		   *ev;
	size_t		nev,
				capev;
	int64		last;			/* highest seq added */
	int64		ndocs;
	SiftWriteTarget *wt;
	int			mapped;			/* slot of the finished segment before the manifest names it, else -1 */
	SiftScoreRef ref;
};

static void
feed_init(SiftFeed *f)
{
	SiftAnalyzerConfig cfg;

	f->cx = AllocSetContextCreate(TopMemoryContext, "sift feed", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(f->cx);
	sift_analyzer_config_from_options(&f->ix->sr->opts, &cfg);
	f->an = sift_analyzer_create(&cfg, f->cx);
	sift_analyzer_set_positions(f->an, true);
	f->inv = sift_inv_create(BUILD_BUDGET, f->cx, true);
	f->maxruns = 16;
	f->runs = palloc(f->maxruns * sizeof(SiftRun *));
	f->ref = (SiftScoreRef) {f->ix->man.k1, f->ix->man.b, 0};
	f->mapped = -1;
}

SiftFeed *
sx_feed_begin(SiftIdx *ix, int64_t upto, char *err, size_t errlen)
{
	SiftFeed   *f = calloc(1, sizeof(SiftFeed));
	int			rc;

	pthread_mutex_lock(&big);
	f->ix = ix;
	f->last = ix->man.drained;
	rc = SIFT_TRY(feed_init(f));
	MemoryContextSwitchTo(TopMemoryContext);
	end_call(rc, err, errlen);
	if (rc != 0)
	{
		free(f);
		return NULL;
	}
	return f;
}

static void
feed_spill(SiftFeed *f)
{
	if (f->nruns + 1 >= f->maxruns)
	{
		f->maxruns *= 2;
		f->runs = repalloc(f->runs, f->maxruns * sizeof(SiftRun *));
	}
	f->runs[f->nruns++] = sift_inv_spill(f->inv);
}

static void
feed_one(SiftFeed *f, int64 seq, char op, int64 pk, const char *text, size_t len)
{
	if (seq <= f->ix->man.drained)
		return;
	if (seq <= f->last)
		fail("sift: queue rows must be fed in ascending seq (%lld after %lld)", (long long) seq, (long long) f->last);
	if (op != 'I' && op != 'D')
		fail("sift: unknown queue operation '%c'", op);
	if (op == 'I' && text == NULL)
		op = 'D';				/* a row without text has no document: it only retires the previous version */
	f->last = seq;
	if (f->nev == f->capev)
		f->ev = realloc(f->ev, (f->capev = f->capev ? f->capev * 2 : 4096) * sizeof(Ev));
	f->ev[f->nev++] = (Ev) {seq, pk, op};
	if (op == 'I' && text)
	{
		SiftTermFreq *terms;
		uint32		dl;
		ItemPointerData tid;
		int			n;

		sift_key_to_tid(sift_docid_key(f->ix->sr, seq), &tid);
		n = sift_analyze_doc(f->an, text, len, &terms, &dl);
		sift_inv_add(f->inv, &tid, terms, n, dl);
		sift_analyzer_reset(f->an);
		f->ndocs++;
		if (sift_inv_full(f->inv))
			feed_spill(f);
	}
}

int
sx_feed_add(SiftFeed *f, int64_t seq, char op, int64_t pk, const char *text, size_t len, char *err, size_t errlen)
{
	int			rc;

	pthread_mutex_lock(&big);
	MemoryContextSwitchTo(f->cx);
	rc = SIFT_TRY(feed_one(f, seq, op, pk, text, len));
	MemoryContextSwitchTo(TopMemoryContext);
	return end_call(rc, err, errlen);
}

static void
rename_segment(SiftIdx *ix, SiftWriteTarget *wt, uint64 gen, ManSeg *ms)
{
	char		final[600];

	snprintf(ms->file, sizeof(ms->file), "seg-%llu.sift", (unsigned long long) gen);
	path_of(ix, final, sizeof(final), ms->file);
	if (rename(sift_wt_path(wt), final) != 0)
		fail("sift: could not rename %s: %s", sift_wt_path(wt), strerror(errno));
	sync_dir(ix->dir);
	ms->slot = sift_wt_slot(wt);
	sift_slot_map(ix->sr, ms->slot, final);
}

static void
feed_write_segment(SiftFeed *f, Manifest *m, ManSeg *ms)
{
	uint64		gen = m->next_gen++;

	f->wt = sift_wt_begin(f->ix->rel, SIFT_WRITE_BULK);
	if (f->nruns == 0)
		sift_inv_write(f->inv, f->wt, gen, &f->ref, &ms->e);
	else
	{
		if (!sift_inv_empty(f->inv))
			feed_spill(f);
		sift_runs_write(f->runs, f->nruns, f->wt, gen, &f->ref, &ms->e);
	}
	sift_wt_finish(f->wt);
	rename_segment(f->ix, f->wt, gen, ms);
	f->mapped = ms->slot;
	f->wt = NULL;
}

static void
feed_commit(SiftFeed *f, int64 *rows)
{
	SiftIdx    *ix = f->ix;
	Manifest	m = ix->man;
	ManSeg		segs[SIFT_MAX_SEGMENTS];
	ManSeg		ms = {0};

	memcpy(segs, ix->segs, m.nsegs * sizeof(ManSeg));
	*rows = f->nev;
	if (f->nev == 0)
		return;
	if (f->ndocs > 0)
	{
		if (m.nsegs == SIFT_MAX_SEGMENTS)
			fail("sift: the index has %d segments: merge it", SIFT_MAX_SEGMENTS);
		feed_write_segment(f, &m, &ms);
		segs[m.nsegs++] = ms;
	}
	append_events(ix, f->ev, f->nev);
	m.log_len += f->nev * sizeof(Ev);
	m.drained = f->last;
	m.version++;
	write_manifest(ix, &m, segs);
	ix->man = m;
	memcpy(ix->segs, segs, m.nsegs * sizeof(ManSeg));
	if (f->ndocs > 0)
		ix->bytes += (int64) ix->sr->slot_blocks[ms.slot] * BLCKSZ;
	f->mapped = -1;
	for (size_t i = 0; i < f->nev; i++)
		apply_event(ix, f->ev[i].seq, f->ev[i].pk, f->ev[i].op);
	finish_events(ix);
}

static void
feed_release(SiftFeed *f)
{
	if (f->wt)
		sift_wt_abort(f->wt);
	if (f->mapped >= 0)
		sift_slot_unmap(f->ix->sr, f->mapped);
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(f->cx);
	free(f->ev);
	free(f);
}

static void run_merge(SiftIdx *ix);

#define AUTO_MERGE_SEGMENTS	16

int
sx_feed_end(SiftFeed *f, int64_t *rows, char *err, size_t errlen)
{
	SiftIdx    *ix = f->ix;
	int			rc;

	pthread_mutex_lock(&big);
	MemoryContextSwitchTo(f->cx);
	rc = SIFT_TRY(feed_commit(f, rows));
	if (rc == 0 && ix->man.nsegs > AUTO_MERGE_SEGMENTS)
		rc = SIFT_TRY(run_merge(ix));
	feed_release(f);
	return end_call(rc, err, errlen);
}

void
sx_feed_abort(SiftFeed *f)
{
	pthread_mutex_lock(&big);
	feed_release(f);
	pthread_mutex_unlock(&big);
}

/* ------------------------------------------------ merge */

static bool
merge_dead_cb(ItemPointer tid, void *state)
{
	SiftIdx    *ix = state;

	return is_dead(ix, sift_key_docid(ix->sr, sift_key_from_tid(tid)));
}

static void
write_compact_log(SiftIdx *ix, uint32 logno, uint64 *len)
{
	char		path[600];
	int			fd;
	Ev			ev[4096];
	size_t		n = 0;

	log_path(ix, logno, path, sizeof(path));
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not create %s: %s", path, strerror(errno));
	*len = 0;
	for (size_t i = 0; i < ix->ndoc; i++)
	{
		if (is_dead(ix, ix->ids[i]))
			continue;
		ev[n++] = (Ev) {ix->ids[i], ix->pks[i], 'I'};
		if (n == lengthof(ev))
		{
			write_all(fd, ev, n * sizeof(Ev), path);
			*len += n * sizeof(Ev);
			n = 0;
		}
	}
	write_all(fd, ev, n * sizeof(Ev), path);
	*len += n * sizeof(Ev);
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", path, strerror(errno));
	close(fd);
}

static void
merge_runs(SiftIdx *ix, Manifest *m, ManSeg *ms, bool *have)
{
	MemoryContext cx = CurrentMemoryContext;
	SiftScoreRef ref = {ix->man.k1, ix->man.b, 0};
	SiftRun   **runs = palloc(ix->man.nsegs * sizeof(SiftRun *));
	int			nruns = 0;
	SiftWriteTarget *wt;
	uint64		gen = m->next_gen++;

	for (uint32 i = 0; i < ix->man.nsegs; i++)
	{
		SiftSegment *seg = sift_seg_open(ix->rel, &ix->segs[i].e, cx);
		IndexBulkDeleteResult stats = {0};
		SiftRun    *r;

		if (ix->ndead > 0)
			sift_seg_bulkdelete(seg, merge_dead_cb, ix, &stats);
		if ((r = sift_run_from_segment(seg, &ref, cx)) != NULL)
			runs[nruns++] = r;
		sift_seg_close(seg);
	}
	*have = nruns > 0;
	if (nruns == 0)
		return;
	wt = sift_wt_begin(ix->rel, SIFT_WRITE_BULK);
	sift_runs_write(runs, nruns, wt, gen, &ref, &ms->e);
	sift_wt_finish(wt);
	rename_segment(ix, wt, gen, ms);
}

static void
run_merge(SiftIdx *ix)
{
	Manifest	m = ix->man;
	ManSeg		ms = {0};
	bool		have;
	size_t		keep = 0;
	uint64		len;
	char		oldlog[600];

	if (ix->man.nsegs == 0 || (ix->man.nsegs == 1 && ix->ndead == 0))
		return;
	merge_runs(ix, &m, &ms, &have);
	write_compact_log(ix, m.logno + 1, &len);
	m.logno++;
	m.log_len = len;
	m.version++;
	m.nsegs = have;
	write_manifest(ix, &m, &ms);
	for (uint32 i = 0; i < ix->man.nsegs; i++)
	{
		char		path[600];

		sift_slot_unmap(ix->sr, ix->segs[i].slot);
		path_of(ix, path, sizeof(path), ix->segs[i].file);
		unlink(path);
	}
	log_path(ix, ix->man.logno, oldlog, sizeof(oldlog));
	unlink(oldlog);
	ix->man = m;
	if (have)
		ix->segs[0] = ms;
	ix->bytes = have ? (int64) ix->sr->slot_blocks[ms.slot] * BLCKSZ : 0;
	for (size_t i = 0; i < ix->ndoc; i++)
		if (!is_dead(ix, ix->ids[i]))
		{
			ix->ids[keep] = ix->ids[i];
			ix->pks[keep++] = ix->pks[i];
		}
	ix->ndoc = keep;
	ix->ndead = 0;
	sync_dead(ix);
}

int
sx_merge(SiftIdx *ix, char *err, size_t errlen)
{
	MemoryContext cx;
	int			rc;

	pthread_mutex_lock(&big);
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_merge(ix));
	op_end(cx);
	return end_call(rc, err, errlen);
}

/* ------------------------------------------------ inspection */

int
sx_stats(SiftIdx *ix, SiftIdxStats *st, char *err, size_t errlen)
{
	pthread_mutex_lock(&big);
	st->nsegs = ix->man.nsegs;
	st->ndocs = ix->ndoc;
	st->ndead = ix->ndead;
	st->nlive = ix->ndoc - ix->ndead;
	st->drained = ix->man.drained;
	st->bytes = ix->bytes;
	st->dpp = ix->man.dpp;
	return end_call(0, err, errlen);
}

typedef struct Tok
{
	SiftIdx    *ix;
	const char *text;
	size_t		len;
	char	   *out;
} Tok;

static void
run_tokens(Tok *t)
{
	SiftAnalyzerConfig cfg;
	SiftAnalyzer *a;
	SiftToken  *toks;
	StringInfoData s;
	int			n;

	sift_analyzer_config_from_options(&t->ix->sr->opts, &cfg);
	a = sift_analyzer_create(&cfg, CurrentMemoryContext);
	n = sift_analyze_tokens(a, t->text, t->len, &toks);
	initStringInfo(&s);
	for (int i = 0; i < n; i++)
	{
		if (i)
			appendStringInfoChar(&s, ' ');
		appendBinaryStringInfo(&s, toks[i].str, toks[i].len);
	}
	t->out = strdup(s.data);
}

int
sx_tokens(SiftIdx *ix, const char *text, size_t len, char **out, char *err, size_t errlen)
{
	Tok			t = {.ix = ix,.text = text,.len = len};
	MemoryContext cx;
	int			rc;

	pthread_mutex_lock(&big);
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_tokens(&t));
	op_end(cx);
	*out = t.out;
	return end_call(rc, err, errlen);
}

int
sx_topk(SiftIdx *ix, const char *qs, int qlen, int k, SxHit *hits, int *n, char *err, size_t errlen)
{
	SxQuery		q = {.ix = ix,.q = qs,.qlen = qlen,.k = k,.hits = hits};
	MemoryContext cx;
	int			rc;

	pthread_mutex_lock(&big);
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_topk(&q, cx));
	op_end(cx);
	*n = q.n;
	return end_call(rc, err, errlen);
}

int
sx_count(SiftIdx *ix, const char *qs, int qlen, int64_t *count, char *err, size_t errlen)
{
	SxQuery		q = {.ix = ix,.q = qs,.qlen = qlen};
	MemoryContext cx;
	int			rc;

	pthread_mutex_lock(&big);
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_count(&q, cx));
	op_end(cx);
	*count = q.count;
	return end_call(rc, err, errlen);
}
