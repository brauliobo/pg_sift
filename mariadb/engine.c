/*
 * engine.c - indexes of segment files driven by the engine of src/: lifecycle, configuration, persistence, documents.
 *
 * Documents. A queue row (seq, op, pk, text) becomes document `seq + docbase` (its docid): the engine never renumbers, so
 * a row updated or deleted later leaves its old docid behind as a dead document, until a merge drops it. The mapping
 * docid -> (page, offset) is dpp documents per page (sift_docid_key). The key of a document comes from the event log,
 * which holds every applied event (docid, pk, op) and is replayed at open into
 *   latest : pk -> docid of the live version,    dead : sorted docids that are superseded or deleted,
 *   ids/pks: docid -> pk of every document in the segments.
 * Durability. A drain writes its segment, appends its events to the log and replaces MANIFEST (write, fsync, rename):
 * the manifest names the segments, the log length and the highest applied seq, so a crash at any point leaves the
 * previous state (files it does not name are removed at open) and a drain repeated after the commit skips what the
 * watermark covers. The manifest carries the format versions of the engine and of the pages, and the configuration of
 * the index: an index of another build is refused at open.
 */
#include "ix.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sift_analyze.h"
#include "sift_bitmap.h"
#include "sift_cpu.h"
#include "sift_kernels.h"
#include "sift_prefix.h"

__thread char sift_err_msg[1024];

static pthread_rwlock_t reg_lock = PTHREAD_RWLOCK_INITIALIZER;
static SiftIdx *reg[256];
static int	nreg;
static char base_dir[400] = ".";
static char tmp_dir[400] = "/tmp";
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

#define SIFT_FORMAT_ID		((uint32) SIFT_FORMAT_MAJOR << 16 | SIFT_FORMAT_MINOR)
#define PAGE_LAYOUT_ID		((uint32) BLCKSZ << 8)

/* ------------------------------------------------ small helpers */

void
rethrow(void)
{
	char		msg[sizeof(sift_err_msg)];

	memcpy(msg, sift_err_msg, sizeof(msg));
	fail("%s", msg);
}

void
fail(const char *fmt,...)
{
	char		msg[sizeof(sift_err_msg)];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	sq_error_set(ERRCODE_INTERNAL_ERROR, "%s", msg);
	pg_unreachable();
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
	sq_mem_init();
	sift_store_init();
	sq_tmpdir = tmp_dir;
	sift_cpu_init();
	sift_init();
	sift_cpu_register(codec_rebind);
	sift_kernels_init();
	sift_prefix_cache_size = 0;	/* the cache of expanded prefixes is a process-wide structure */
}

void
thread_init(void)
{
	pthread_once(&init_once, engine_init);
	sq_mem_init();
}

int
finish_call(int rc, char *err, size_t errlen)
{
	if (rc != 0 && err)
		snprintf(err, errlen, "%s", sift_err_msg);
	return rc;
}

MemoryContext
op_begin(void)
{
	return AllocSetContextCreate(TopMemoryContext, "sift op", ALLOCSET_DEFAULT_SIZES);
}

void
op_end(MemoryContext cx)
{
	MemoryContextDelete(cx);	/* leaves the current context alone unless it is cx: operations nest (the delta of a query) */
}

void
path_of(const SiftIdx *ix, char *out, size_t n, const char *file)
{
	snprintf(out, n, "%s/%s", ix->dir, file);
}

void
sync_dir(const char *dir)
{
	int			fd = open(dir, O_RDONLY);

	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
}

void
write_all(int fd, const void *p, size_t n, const char *what)
{
	if (write(fd, p, n) != (ssize_t) n)
		fail("sift: could not write %s: %s", what, strerror(errno));
}

const char *
sx_version(void)
{
	static char v[160];

	snprintf(v, sizeof(v), "sift-mariadb %s (format %d, pages %d.%d layout %u, built for MariaDB %s)", SIFT_MDB_RELEASE,
			 MDB_FORMAT, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR, (unsigned) PAGE_LAYOUT_ID, SIFT_MDB_SERVER_VERSION);
	return v;
}

/* ------------------------------------------------ documents: latest, dead, ids/pks */

size_t
lower_bound(const int64 *a, size_t n, int64 v)
{
	size_t		lo = 0,
				hi = n;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (a[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

bool
in_sorted(const int64 *a, size_t n, int64 v)
{
	size_t		i = lower_bound(a, n, v);

	return i < n && a[i] == v;
}

bool
is_dead(const SiftIdx *ix, int64 docid)
{
	return in_sorted(ix->dead, ix->ndead, docid);
}

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

static size_t
slot_hash(int64 pk)
{
	return ((uint64) pk * 0x9E3779B97F4A7C15ULL) >> 20;
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
			size_t		h = slot_hash(old[i].key);

			while (ix->latest[h % ix->caplatest].used)
				h++;
			ix->latest[h % ix->caplatest] = old[i];
			ix->nlatest++;
		}
	free(old);
}

/* The slot of pk, or the free one it would take */
static Slot *
latest_slot(const SiftIdx *ix, int64 pk)
{
	size_t		h = slot_hash(pk);

	for (;; h++)
	{
		Slot	   *s = &ix->latest[h % ix->caplatest];

		if (!s->used || s->key == pk)
			return s;
	}
}

int64
latest_get(const SiftIdx *ix, int64 pk)
{
	return ix->caplatest ? latest_slot(ix, pk)->val : 0;
}

/* Applies one event: the previous live version of pk dies, an insert becomes the live one */
void
apply_event(SiftIdx *ix, int64 docid, int64 pk, int64 op)
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
	s->val = op == 'I' ? docid : 0;
	if (op == 'I')
	{
		if (ix->ndoc == ix->capdoc)
		{
			ix->capdoc = ix->capdoc ? ix->capdoc * 2 : 1024;
			ix->ids = realloc(ix->ids, ix->capdoc * sizeof(int64));
			ix->pks = realloc(ix->pks, ix->capdoc * sizeof(int64));
		}
		ix->ids[ix->ndoc] = docid;
		ix->pks[ix->ndoc++] = pk;
	}
}

void
finish_events(SiftIdx *ix)
{
	qsort(ix->dead, ix->ndead, sizeof(int64), cmp_i64);
	ix->sr->any_dead = ix->ndead > 0;
}

void
reset_docs(SiftIdx *ix)
{
	free(ix->ids);
	free(ix->pks);
	free(ix->dead);
	free(ix->latest);
	ix->ids = ix->pks = ix->dead = NULL;
	ix->latest = NULL;
	ix->ndoc = ix->capdoc = ix->ndead = ix->capdead = ix->caplatest = ix->nlatest = 0;
}

int64
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

/* ------------------------------------------------ configuration */

/* The value of key in the lines of cfg, malloc'd; NULL when absent */
static char *
cfg_value(const char *cfg, const char *key)
{
	size_t		kl = strlen(key);

	for (const char *p = cfg; *p;)
	{
		const char *nl = strchr(p, '\n');
		size_t		len = nl ? (size_t) (nl - p) : strlen(p);

		if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
			return strndup(p + kl + 1, len - kl - 1);
		p += len + (nl != NULL);
	}
	return NULL;
}

static double
cfg_num(const char *cfg, const char *key, double def)
{
	char	   *v = cfg_value(cfg, key);
	double		d = v ? atof(v) : def;

	free(v);
	return d;
}

const char *
sx_cfg(SiftIdx *ix, const char *key)
{
	static const char *const names[NMETA] = {"queue", "keyexpr", "source", "textexpr", "database", "table"};

	for (int i = 0; i < NMETA; i++)
		if (strcmp(key, names[i]) == 0)
			return ix->meta[i];
	return NULL;
}

/* The SiftOptions of the configuration, with their strings behind them as the reloption layout wants */
static void
apply_opts(SiftRel *sr, const char *cfg)
{
	SiftOptions *o = &sr->opts;
	char	   *stemmer = cfg_value(cfg, "stemmer"),
			   *fields = cfg_value(cfg, "fields"),
			   *analyzer = cfg_value(cfg, "analyzer");
	size_t		at = 0;

	o->k1 = cfg_num(cfg, "k1", SIFT_DEFAULT_K1);
	o->b = cfg_num(cfg, "b", SIFT_DEFAULT_B);
	o->case_folding = cfg_num(cfg, "case_folding", 1) != 0;
	o->accent_folding = cfg_num(cfg, "accent_folding", 1) != 0;
	o->positions = cfg_num(cfg, "positions", 1) != 0;
	o->prefix_index = (int) cfg_num(cfg, "prefix_index", 0);
	o->analyzer = analyzer && strcmp(analyzer, "keyword") == 0 ? SIFT_ANALYZER_KEYWORD : SIFT_ANALYZER_TEXT;
	o->stemmer = o->fields = 0;
	if (strlen(stemmer ? stemmer : "") + strlen(fields ? fields : "") + 2 > sizeof(sr->optstr))
		fail("sift: stemmer and fields do not fit the options");
	for (int i = 0; i < 2; i++)
	{
		char	   *v = i ? fields : stemmer;
		int		   *off = i ? &o->fields : &o->stemmer;

		if (v == NULL || *v == '\0')
			continue;
		memcpy(sr->optstr + at, v, strlen(v) + 1);
		*off = (int) (offsetof(SiftRel, optstr) - offsetof(SiftRel, opts) + at);
		at += strlen(v) + 1;
	}
	free(stemmer);
	free(fields);
	free(analyzer);
}

/* Raises the engine's own complaint about options it cannot use (an unknown stemmer, a keyword analyzer with fields) */
static void
check_opts(const SiftRel *sr)
{
	SiftAnalyzerConfig acfg;
	MemoryContext cx = op_begin();

	MemoryContextSwitchTo(cx);
	sift_analyzer_config_from_options(&sr->opts, &acfg);
	op_end(cx);
}

static void
set_meta(SiftIdx *ix)
{
	static const char *const names[NMETA] = {"queue", "keyexpr", "source", "textexpr", "database", "table"};

	for (int i = 0; i < NMETA; i++)
		ix->meta[i] = cfg_value(ix->cfg, names[i]);
	ix->fresh = cfg_num(ix->cfg, "fresh", 1) != 0 && ix->meta[0] != NULL;
	ix->recheck = cfg_num(ix->cfg, "recheck", 1) != 0 && ix->meta[0] != NULL;
	ix->fanin = (int) cfg_num(ix->cfg, "merge_fanin", 6);
}

/* ------------------------------------------------ persistence */

void
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
	write_all(fd, ix->cfg, m->cfg_len, "the manifest");
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", tmp, strerror(errno));
	close(fd);
	if (rename(tmp, final) != 0)
		fail("sift: could not rename %s: %s", tmp, strerror(errno));
	sync_dir(ix->dir);
}

void
log_path(const SiftIdx *ix, uint32 logno, char *out, size_t n)
{
	snprintf(out, n, "%s/events-%u.log", ix->dir, logno);
}

void
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
			apply_event(ix, ev[i].docid, ev[i].pk, ev[i].op);
		left -= got;
	}
	if (fd >= 0)
		close(fd);
	finish_events(ix);
}

/* The log of the documents n given: an insert for each, then the delete of every key that has none left alive */
void
write_compact_log(SiftIdx *ix, uint32 logno, const int64 *ids, const int64 *pks, size_t nd, uint64 *len)
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
	for (int pass = 0; pass < 2; pass++)
		for (size_t i = 0; i < nd; i++)
		{
			if (pass == 0)
				ev[n++] = (Ev) {ids[i], pks[i], 'I'};
			else if (latest_get(ix, pks[i]) == 0)
				ev[n++] = (Ev) {0, pks[i], 'D'};
			else
				continue;
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
load_manifest(SiftIdx *ix)
{
	char		path[600];
	int			fd;
	Manifest   *m = &ix->man;

	path_of(ix, path, sizeof(path), "MANIFEST");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail("sift: index \"%s\" does not exist (%s)", ix->name, path);
	if (read(fd, m, sizeof(Manifest)) != sizeof(Manifest) || memcmp(m->magic, "SIFTMDB", 7) != 0)
		fail("sift: manifest %s is corrupted", path);
	if (memcmp(m->magic, MAN_MAGIC, 8) != 0 || m->format != MDB_FORMAT)
		fail("sift: index \"%s\" was built by an incompatible version of sift for MariaDB (manifest format %c, this build "
			 "reads %d): rebuild it with sift_rebuild", ix->name, m->magic[7], MDB_FORMAT);
	if (m->sift_format >> 16 != SIFT_FORMAT_MAJOR || (m->sift_format & 0xFFFF) > SIFT_FORMAT_MINOR)
		fail("sift: index \"%s\" has page format %u.%u, this build reads up to %d.%d: rebuild it with sift_rebuild",
			 ix->name, m->sift_format >> 16, m->sift_format & 0xFFFF, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR);
	if (m->page_layout != PAGE_LAYOUT_ID)
		fail("sift: index \"%s\" has page layout %u, this build uses %u: rebuild it with sift_rebuild", ix->name,
			 m->page_layout, (unsigned) PAGE_LAYOUT_ID);
	ix->cfg = malloc(m->cfg_len + 1);
	if (m->nsegs > MAX_SEGS ||
		read(fd, ix->segs, m->nsegs * sizeof(ManSeg)) != (ssize_t) (m->nsegs * sizeof(ManSeg)) ||
		read(fd, ix->cfg, m->cfg_len) != (ssize_t) m->cfg_len)
		fail("sift: manifest %s is corrupted", path);
	ix->cfg[m->cfg_len] = '\0';
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
	apply_opts(ix->sr, ix->cfg);
	set_meta(ix);
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
		ix->slot_reserved[ix->segs[i].slot] = true;
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

/* ------------------------------------------------ slots, generations */

int
slot_reserve(SiftIdx *ix)
{
	int			slot = 0;

	pthread_mutex_lock(&ix->wmu);
	while (slot < SIFT_DELTA_SLOT && (ix->sr->slot_used[slot] || ix->slot_reserved[slot]))
		slot++;
	if (slot == SIFT_DELTA_SLOT)
	{
		pthread_mutex_unlock(&ix->wmu);
		fail("sift: index \"%s\" has too many segments: merge it", ix->name);
	}
	ix->slot_reserved[slot] = true;
	pthread_mutex_unlock(&ix->wmu);
	return slot;
}

void
slot_release(SiftIdx *ix, int slot)
{
	pthread_mutex_lock(&ix->wmu);
	ix->slot_reserved[slot] = false;
	pthread_mutex_unlock(&ix->wmu);
}

uint64
gen_take(SiftIdx *ix)
{
	uint64		g;

	pthread_mutex_lock(&ix->wmu);
	g = ix->man.next_gen++;
	pthread_mutex_unlock(&ix->wmu);
	return g;
}

/* Names the finished segment file, maps it; the caller attaches the slot when it publishes */
void
rename_segment(SiftIdx *ix, SiftWriteTarget *wt, uint64 gen, ManSeg *ms, int *g, uint32 *blocks)
{
	char		final[600];

	snprintf(ms->file, sizeof(ms->file), "seg-%llu.sift", (unsigned long long) gen);
	path_of(ix, final, sizeof(final), ms->file);
	if (rename(sift_wt_path(wt), final) != 0)
		fail("sift: could not rename %s: %s", sift_wt_path(wt), strerror(errno));
	sync_dir(ix->dir);
	ms->slot = sift_wt_slot(wt);
	*g = sift_map_path(final, ms->slot, blocks);
}

void
delta_release(Delta *d)
{
	if (d == NULL || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	if (d->g >= 0)
		sift_unmap(d->g, d->blocks);
	for (size_t i = 0; i < d->nd; i++)
		free(d->text[i]);
	free(d->dead);
	free(d->dids);
	free(d->dpks);
	free(d->text);
	free(d->tlen);
	free(d);
}

/* Forgets the cached live segment (a writer published: the queue rows it was built over are in the segments) */
void
delta_drop(SiftIdx *ix)
{
	Delta	   *d;

	pthread_mutex_lock(&ix->dmu);
	d = ix->delta;
	ix->delta = NULL;
	pthread_mutex_unlock(&ix->dmu);
	delta_release(d);
}

/* ------------------------------------------------ public: lifecycle */

void
sx_set_dirs(const char *base, const char *tmp)
{
	snprintf(base_dir, sizeof(base_dir), "%s", base);
	snprintf(tmp_dir, sizeof(tmp_dir), "%s", tmp);
}

SiftIdx *
registered(const char *name)
{
	for (int i = 0; i < nreg; i++)
		if (strcmp(reg[i]->name, name) == 0)
			return reg[i];
	return NULL;
}

void
registry_each(void (*fn) (SiftIdx *ix, void *arg), void *arg)
{
	pthread_rwlock_rdlock(&reg_lock);
	for (int i = 0; i < nreg; i++)
		fn(reg[i], arg);
	pthread_rwlock_unlock(&reg_lock);
}

static void
do_create(const char *name, const char *dir, const char *cfg)
{
	char		d[600];
	SiftIdx		ix = {0};
	SiftRel    *sr;
	Relation	rel;
	Manifest	m = {.magic = MAN_MAGIC,.format = MDB_FORMAT,.sift_format = SIFT_FORMAT_ID,.page_layout = PAGE_LAYOUT_ID,
	.next_gen = 1};
	int			dpp = (int) cfg_num(cfg, "dpp", 32);

	if (dpp < 1 || dpp > 256)
		fail("sift: documents per page must be between 1 and 256");
	if (cfg_num(cfg, "merge_fanin", 6) < 2)
		fail("sift: merge_fanin must be at least 2");
	snprintf(ix.name, sizeof(ix.name), "%s", name);
	snprintf(ix.dir, sizeof(ix.dir), "%s", dir);
	rel = sift_rel_create(name, dir, dpp, &sr);
	apply_opts(sr, cfg);
	check_opts(sr);
	sift_rel_view_free(rel);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		fail("sift: could not create %s: %s", dir, strerror(errno));
	path_of(&ix, d, sizeof(d), "MANIFEST");
	if (access(d, F_OK) == 0)
		fail("sift: index \"%s\" already exists", name);
	ix.cfg = (char *) cfg;
	m.dpp = dpp;
	m.cfg_len = strlen(cfg);
	write_manifest(&ix, &m, NULL);
}

int
sx_create(const char *name, const char *cfg, char *err, size_t errlen)
{
	char		dir[600];
	int			rc;

	thread_init();
	dir_of(name, dir, sizeof(dir));
	pthread_rwlock_wrlock(&reg_lock);
	rc = SIFT_TRY(do_create(name, dir, cfg));
	pthread_rwlock_unlock(&reg_lock);
	return finish_call(rc, err, errlen);
}

int
sx_exists(const char *name)
{
	char		dir[600],
				man[700];

	dir_of(name, dir, sizeof(dir));
	snprintf(man, sizeof(man), "%s/MANIFEST", dir);
	return access(man, F_OK) == 0;
}

static void
free_idx(SiftIdx *ix)
{
	delta_drop(ix);
	if (ix->rel)
		sift_rel_destroy(ix->rel);
	reset_docs(ix);
	ix->dropped = true;
}

static void
unlink_dir(const char *dir)
{
	DIR		   *d = opendir(dir);
	struct dirent *e;

	while (d && (e = readdir(d)) != NULL)
	{
		char		path[600];

		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	if (d)
		closedir(d);
	rmdir(dir);
}

static void
do_drop(const char *name, const char *dir)
{
	SiftIdx    *ix = registered(name);

	if (!sx_exists(name))
		fail("sift: index \"%s\" does not exist", name);
	if (ix)
	{
		pthread_mutex_lock(&ix->mergemu);
		WR_LOCK(ix);
		for (int i = 0; i < nreg; i++)
			if (reg[i] == ix)
				reg[i] = reg[--nreg];
		free_idx(ix);			/* the struct itself stays: a query that started before may still hold it */
		WR_UNLOCK(ix);
		pthread_mutex_unlock(&ix->mergemu);
	}
	unlink_dir(dir);
}

int
sx_drop(const char *name, char *err, size_t errlen)
{
	char		dir[600];
	int			rc;

	thread_init();
	dir_of(name, dir, sizeof(dir));
	pthread_rwlock_wrlock(&reg_lock);
	rc = SIFT_TRY(do_drop(name, dir));
	pthread_rwlock_unlock(&reg_lock);
	return finish_call(rc, err, errlen);
}

static SiftIdx *
new_idx(const char *name)
{
	SiftIdx    *ix = calloc(1, sizeof(SiftIdx));
	pthread_rwlockattr_t at;

	pthread_rwlockattr_init(&at);
	pthread_rwlockattr_setkind_np(&at, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&ix->rw, &at);
	pthread_mutex_init(&ix->wmu, NULL);
	pthread_mutex_init(&ix->mergemu, NULL);
	pthread_mutex_init(&ix->dmu, NULL);
	snprintf(ix->name, sizeof(ix->name), "%s", name);
	dir_of(name, ix->dir, sizeof(ix->dir));
	return ix;
}

SiftIdx *
sx_open(const char *name, char *err, size_t errlen)
{
	SiftIdx    *ix;

	thread_init();
	pthread_rwlock_rdlock(&reg_lock);
	ix = registered(name);
	pthread_rwlock_unlock(&reg_lock);
	if (ix)
		return ix;
	pthread_rwlock_wrlock(&reg_lock);
	if ((ix = registered(name)) == NULL)
	{
		ix = new_idx(name);
		if (nreg == lengthof(reg))
			snprintf(sift_err_msg, sizeof(sift_err_msg), "sift: too many indexes open");
		if (nreg < lengthof(reg) && SIFT_TRY(do_open(ix)) == 0)
			reg[nreg++] = ix;
		else
		{
			finish_call(-1, err, errlen);
			free_idx(ix);
			free(ix);
			ix = NULL;
		}
	}
	pthread_rwlock_unlock(&reg_lock);
	return ix;
}

/* ------------------------------------------------ inspection */

int
sx_stats(SiftIdx *ix, SiftIdxStats *st, char *err, size_t errlen)
{
	RD_LOCK(ix);
	st->nsegs = ix->man.nsegs;
	st->ndocs = ix->ndoc;
	st->ndead = ix->ndead;
	st->nlive = ix->ndoc - ix->ndead;
	st->drained = ix->man.drained;
	st->docbase = ix->man.docbase;
	st->bytes = ix->bytes;
	st->dpp = ix->man.dpp;
	st->merges = ix->merges;
	st->format = ix->man.format;
	RD_UNLOCK(ix);
	return 0;
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

	thread_init();
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	rc = SIFT_TRY(run_tokens(&t));
	op_end(cx);
	*out = t.out;
	return finish_call(rc, err, errlen);
}

/* The registry is the only thing a library unload must clear: the merge thread first, then the mapped segments */
__attribute__((destructor)) static void
engine_unload(void)
{
	merge_stop();
}
