/*
 * sqpar.c - parallel scans inside one query (setting parallel=N, off by default). A ranked top-k or a count runs on N
 * participants: the calling thread and N-1 threads of a process-wide pool, all over the same segments. The engine's own
 * work distribution does the rest (sift_parallel.c): participants claim morsels of superblocks or of bound-ordered
 * units, share the threshold of the k-th best score, and the last one to finish returns the merged top-k, identical to
 * the serial one.
 *
 * Threads cannot share SQLite's connection, so a parallel scan needs the whole index resident in the page cache
 * (sq_store_preload; else the scan stays serial) and no document text: phrases that the stored positions do not decide
 * are not parallel. While it runs the store is in shared mode: lookups and atomic pins only.
 */
#include "sqvtab.h"

#include <pthread.h>

#include "sift_parallel.h"

#define PAR_MAX		16
#define PAR_MIN_POSTINGS	100000

/* ---------- a pool of threads that run the participants of a scan ---------- */

typedef struct Run
{
	void		(*fn) (void *arg, int idx);
	void	   *arg;
	int			n,
				taken,
				done;
	struct Run *next;
} Run;

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_wake = PTHREAD_COND_INITIALIZER,
			run_done = PTHREAD_COND_INITIALIZER;
static Run *queue;
static int	nthreads;

/* The next participant of a queued run (of only, when given); pool_lock held */
static Run *
take(Run *only, int *idx)
{
	for (Run **pp = &queue; *pp; pp = &(*pp)->next)
	{
		Run		   *r = *pp;

		if ((only && r != only) || r->taken == r->n)
			continue;
		*idx = r->taken++;
		if (r->taken == r->n)
			*pp = r->next;
		return r;
	}
	return NULL;
}

static void
execute(Run *r, int idx)
{
	r->fn(r->arg, idx);
	pthread_mutex_lock(&pool_lock);
	if (++r->done == r->n)
		pthread_cond_broadcast(&run_done);
	pthread_mutex_unlock(&pool_lock);
}

static void *
pool_main(void *unused)
{
	pthread_mutex_lock(&pool_lock);
	for (;;)
	{
		int			idx;
		Run		   *r = take(NULL, &idx);

		if (r == NULL)
		{
			pthread_cond_wait(&pool_wake, &pool_lock);
			continue;
		}
		pthread_mutex_unlock(&pool_lock);
		execute(r, idx);
		pthread_mutex_lock(&pool_lock);
	}
	return NULL;
}

/* Runs fn(arg, 0..n-1) on up to n threads, the caller's among them; returns when all have run. A pool thread that cannot be made only means fewer helpers. */
static void
run_all(void (*fn) (void *, int), void *arg, int n)
{
	Run			r = {.fn = fn, .arg = arg, .n = n};
	int			idx;

	pthread_mutex_lock(&pool_lock);
	r.next = queue;
	queue = &r;
	while (nthreads < n - 1)
	{
		pthread_t	t;

		if (pthread_create(&t, NULL, pool_main, NULL) != 0)
			break;
		pthread_detach(t);
		nthreads++;
	}
	pthread_cond_broadcast(&pool_wake);
	while (take(&r, &idx))
	{
		pthread_mutex_unlock(&pool_lock);
		execute(&r, idx);
		pthread_mutex_lock(&pool_lock);
	}
	while (r.done < r.n)
		pthread_cond_wait(&run_done, &pool_lock);
	pthread_mutex_unlock(&pool_lock);
}

/* ---------- one scan ---------- */

typedef struct Scan
{
	SqVtab	   *vt;
	const SqQuery *q;
	int			nq;
	int64_t		k;						/* ranked: > 0; count: 0 */
	SiftPar		par;
	SqHit	  **hits;					/* per participant: what it emitted */
	int64_t	   *nhits;
	int64_t		count;
	char	   *err;
	pthread_mutex_t lock;
} Scan;

static bool
no_text(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	return true;
}

static SiftQuery **
parse(Scan *s, SiftIndexState *state)
{
	SiftQuery **qs = palloc(s->nq * sizeof(SiftQuery *));

	for (int i = 0; i < s->nq; i++)
		qs[i] = sift_state_query(state, s->q[i].text, s->q[i].len);
	return qs;
}

static void
rank_part(Scan *s, int idx)
{
	SiftPar		par = s->par;				/* the slot is the participant's own */
	Size		len;
	const void *shared = sift_par_get_bind(&par, &len);
	SiftIndexState *state = sift_state_open_dir(sq_store_rel(s->vt->st), CurrentMemoryContext, sift_shared_dir(shared));
	SiftPlan   *plan = sift_plan_bind_shared(state, parse(s, state), s->nq, SIFT_BIND_SCORE, shared);
	SiftRanker *r = sift_rank_begin(plan, s->k, no_text, NULL);
	ItemPointerData tid;
	float4		score;
	SqHit	   *out = NULL;
	int64_t		n = 0;

	sift_rank_parallel(r, &par);
	while (n < s->k && sift_rank_next(r, &tid, &score))
	{
		if ((n & (n - 1)) == 0)
			out = realloc(out, Max(2 * n, 16) * sizeof(SqHit));
		out[n++] = (SqHit) {sq_rowid(tid.blk, tid.off), score};
	}
	s->hits[idx] = out;
	s->nhits[idx] = n;
	sift_rank_end(r);
}

static void
count_part(Scan *s, int idx)
{
	SiftIndexState *state = sift_state_open(sq_store_rel(s->vt->st), CurrentMemoryContext);
	SiftPlan   *plan = sift_plan_bind(state, parse(s, state), s->nq, 0);
	SiftVis		vis;
	int64_t		n;

	memset(&vis, 0, sizeof(vis));
	vis.use_vm = true;
	n = sift_eval_count_ranges(plan, &vis, no_text, NULL, sift_par_next_range, &s->par);
	__atomic_fetch_add(&s->count, n, __ATOMIC_RELAXED);
}

static void
participant(void *arg, int idx)
{
	Scan	   *s = arg;
	MemoryContext old = sq_enter();
	SqStore    *prev = sq_store_enter(s->vt->st);
	bool		failed = false;

	SQ_TRY();
	{
		if (s->k > 0)
			rank_part(s, idx);
		else
			count_part(s, idx);
	}
	SQ_CATCH();
	failed = true;
	SQ_END();
	if (failed)
	{
		pthread_mutex_lock(&s->lock);
		if (s->err == NULL)
			s->err = strdup(sq_errmsg());
		pthread_mutex_unlock(&s->lock);
	}
	sq_store_leave(prev);
	sq_leave(old);
}

/* The superblocks the keys of the directory live in */
static uint32
superblocks(const SqStore *st)
{
	const SiftDirSnapshot *d = sq_store_dir(st);
	uint32		nsb = 0;

	for (int i = 0; i < d->nsegs; i++)
		nsb = Max(nsb, d->segs[i].sb_hi + 1);
	return nsb;
}

/* The bind every participant of a ranked scan scores from: statistics and directory as of now, whoever runs it */
static void
export_bind(Scan *s)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift parallel bind", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftIndexState *state = sift_state_open(sq_store_rel(s->vt->st), tmp);
	SiftPlan   *plan = sift_plan_bind(state, parse(s, state), s->nq, SIFT_BIND_SCORE);
	Size		len;
	void	   *data = sift_plan_export(plan, &len);

	sift_par_set_bind(s->par.sh, s->par.area, data, len);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

static void
scan_start(Scan *s, SqVtab *vt, const SqQuery *q, int nq, int64_t k)
{
	int			n = Min(vt->parallel, PAR_MAX);

	*s = (Scan) {.vt = vt, .q = q, .nq = nq, .k = k};
	pthread_mutex_init(&s->lock, NULL);
	s->par.sh = palloc(sift_par_estimate(n - 1));
	sift_par_init(s->par.sh, n - 1, superblocks(vt->st));
	s->hits = calloc(n, sizeof(SqHit *));
	s->nhits = calloc(n, sizeof(int64_t));
	if (k > 0)
		export_bind(s);
}

static void
scan_run(Scan *s)
{
	char	   *err;

	sq_store_set_shared(s->vt->st, true);
	run_all(participant, s, Min(s->vt->parallel, PAR_MAX));
	sq_store_set_shared(s->vt->st, false);
	err = s->err;
	if (err)
	{
		sq_store_recover(s->vt->st);
		elog(ERROR, "%s", err);
	}
}

static void
scan_free(Scan *s)
{
	for (int i = 0; i < Min(s->vt->parallel, PAR_MAX); i++)
		free(s->hits[i]);
	free(s->hits);
	free(s->nhits);
	free(s->err);
	pthread_mutex_destroy(&s->lock);
}

/* ---------- entry points ---------- */

/* Counting is bitmap arithmetic, microseconds: only a phrase, which decodes positions, takes long enough to share */
static bool
has_phrase(const SiftQNode *n)
{
	for (int i = 0; n->kind != SQ_PHRASE && i < n->nchild; i++)
		if (has_phrase(n->child[i]))
			return true;
	return n->kind == SQ_PHRASE;
}

bool
sq_par_count_pays(const SiftQuery *q)
{
	return has_phrase(q->root);
}

/* Postings a query reads below which handing the work to threads costs more than it saves */
static bool
big_enough(SqVtab *vt, const SqQuery *q, int nq)
{
	text	  **t = palloc(nq * sizeof(text *));
	SiftEstimate est;

	for (int i = 0; i < nq; i++)
		t[i] = cstring_to_text_with_len(q[i].text, q[i].len);
	sift_eval_estimate(sq_store_rel(vt->st), t, nq, &est);
	return est.postings >= PAR_MIN_POSTINGS;
}

int
sq_par_available(SqVtab *vt, SiftPlan *plan, const SqQuery *q, int nq)
{
	return vt->parallel >= 2 && plan != NULL && !sift_plan_recheck_needed(plan) && sq_store_preload(vt->st) &&
		big_enough(vt, q, nq);
}

SqHit *
sq_par_rank(SqVtab *vt, const SqQuery *q, int nq, int64_t k, int64_t *nhits)
{
	Scan		s;
	SqHit	   *all = NULL;
	int64_t		n = 0;

	scan_start(&s, vt, q, nq, k);
	vt->par_scans++;
	SQ_TRY();
	scan_run(&s);
	SQ_CATCH();
	scan_free(&s);
	sq_error_rethrow();
	SQ_END();
	for (int i = 0; i < Min(vt->parallel, PAR_MAX); i++)
		n += s.nhits[i];
	all = palloc(Max(n, 1) * sizeof(SqHit));
	n = 0;
	for (int i = 0; i < Min(vt->parallel, PAR_MAX); i++)
	{
		memcpy(all + n, s.hits[i], s.nhits[i] * sizeof(SqHit));
		n += s.nhits[i];
	}
	scan_free(&s);
	*nhits = n;
	return all;
}

int64_t
sq_par_count(SqVtab *vt, const SqQuery *q, int nq)
{
	Scan		s;
	int64_t		n;

	scan_start(&s, vt, q, nq, 0);
	vt->par_scans++;
	SQ_TRY();
	scan_run(&s);
	SQ_CATCH();
	scan_free(&s);
	sq_error_rethrow();
	SQ_END();
	n = s.count;
	scan_free(&s);
	return n;
}
