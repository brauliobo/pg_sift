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
 * The documents are in docs.c, the persistence (manifest, event log, segment files) in persist.c.
 */
#include "ix.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bm25/analyze.h"
#include "bm25/bitmap.h"
#include "core/cmp.h"
#include "core/cpu.h"
#include "bm25/kernels.h"
#include "bm25/prefix.h"

__thread char sift_err_msg[1024];

static pthread_rwlock_t reg_lock = PTHREAD_RWLOCK_INITIALIZER;
static SiftIdx *reg[256];
static int	nreg;
static char base_dir[400] = ".";
static char tmp_dir[400] = "/tmp";
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

#define SIFT_FORMAT_ID		((uint32) SIFT_FORMAT_MAJOR << 16 | SIFT_WRITE_SEGFMT)

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

void
init_rel(SiftIdx *ix)
{
	ix->rel = sift_rel_create(ix->name, ix->dir, ix->man.dpp, &ix->sr);
	apply_opts(ix->sr, ix->cfg);
	set_meta(ix);
}

static void
dir_of(const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", base_dir, name);
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
