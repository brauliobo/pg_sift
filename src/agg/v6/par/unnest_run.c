/*
 * agg/v6/par/unnest_run.c - the driver of agg/v6/par/unnest_run.h.
 */
/* check-memory: allow-allocs the slots follow the threads of the pool and the columns of one statement, never the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "miscadmin.h"
#include "utils/memutils.h"

#include "agg/v6/par/gsink.h"
#include "agg/v6/par/pinrun.h"
#include "agg/v6/par/unnest_run.h"
#include "agg/vexpr_str.h"
#include "core/memtrack.h"
#include "core/par/pool.h"
#include "core/popcount.h"

#define SCRATCH_BYTES	(64u << 10)
#define STATE_BYTES		(4u << 20)
#define WORK_BYTES		(1u << 20)
#define SINK_BYTES		(1u << 20)
#define INLINE_BYTES	12

typedef struct UnPar
{
	const SiftV6UnnestSrc *src;
	const SiftV6Spec *spec;
	SiftV6Group *g;
	SiftParWordShape sh;
	SiftParUnnest *un;
	SiftV6GSink *sink;
	SiftParGroup *grp;
	MemoryContext cx;
	int			ncols;
	SiftVecType type[SIFT_UN_COLS_MAX + 1];
	uint32		nslots;
	SiftUnSlot **slot;
	uint32	   *seq;			/* the batch of each slot */
	uint32		nbatch;			/* the batches read, the ones the leader folded itself too */
	uint32		nleft;			/* the slots a thread left to the leader */
	uint8	   *folded;			/* bit of a batch: the threads folded it */
	uint32		folded_cap;
	size_t		state_bytes;	/* the state arena and the stream of groups of each thread */
	size_t		sink_bytes;
} UnPar;

/* ---- the slots ---- */

static SiftUnSlot *
slot_new(UnPar *r)
{
	SiftUnSlot *sl = MemoryContextAllocAligned(r->cx, sizeof(SiftUnSlot), alignof(SiftUnSlot), MCXT_ALLOC_ZERO);

	sl->ncols = r->ncols;
	for (int i = 0; i < r->ncols; i++)
	{
		sl->data[i] = MemoryContextAllocAligned(r->cx, SIFT_VEC_LEN * sift_gather_width(r->type[i]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		sl->valid[i] = MemoryContextAllocZero(r->cx, SIFT_VEC_WORDS * sizeof(uint64));
	}
	return sl;
}

static void
grow_elems(UnPar *r, SiftUnSlot *sl, uint32 need)
{
	const size_t w = sift_gather_width(r->type[r->ncols]);

	if (need <= sl->ecap && sl->edata != NULL)
		return;
	if (sl->edata != NULL)
	{
		pfree(sl->edata);
		pfree(sl->evalid);
	}
	sl->ecap = Max(need, 4 * SIFT_VEC_LEN);
	sl->edata = MemoryContextAllocAligned(r->cx, (size_t) sl->ecap * w + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_HUGE);
	sl->evalid = MemoryContextAllocZero(r->cx, ((sl->ecap + 63) / 64 + 1) * sizeof(uint64));
}

static bool
bit_at(const uint64 *m, uint32 i)
{
	return m == NULL || ((m[i >> 6] >> (i & 63)) & 1);
}

/* The bytes of the strings of v that its views do not hold */
static size_t
long_bytes(const SiftVec *v, uint32 n)
{
	size_t		total = 0;

	if (v->type != SVEC_STRVIEW)
		return 0;
	for (uint32 i = 0; i < n; i++)
	{
		const SiftStrView *sv = &((const SiftStrView *) v->data)[i];

		if (sv->len > INLINE_BYTES && bit_at(v->valid, i))
			total += sv->len;
	}
	return total;
}

/* The strings that the views of v point to, copied to at and the views made to point to the copies: the batch they came from goes with the next one */
static char *
own_strings(SiftVec *v, uint32 n, char *at)
{
	if (v->type != SVEC_STRVIEW)
		return at;
	for (uint32 i = 0; i < n; i++)
	{
		SiftStrView *sv = &((SiftStrView *) v->data)[i];

		if (sv->len > INLINE_BYTES && bit_at(v->valid, i))
		{
			memcpy(at, sift_vstr_bytes(sv), sv->len);
			*sv = sift_vstr_make(at, sv->len);
			at += sv->len;
		}
	}
	return at;
}

static void
copy_valid(uint64 *dst, const SiftVec *src, uint32 from, uint32 n)
{
	memset(dst, 0, ((n + 63) / 64 + 1) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		dst[i >> 6] |= (uint64) (src->all_valid || bit_at(src->valid, from + i)) << (i & 63);
}

/* The batch b with the rows of mask, made the slot's own: nothing of it points into the memory of the scan */
static void
slot_fill(UnPar *r, SiftUnSlot *sl, const SiftF2ExtBatch *b, const uint64 *mask)
{
	const SiftF2ElemStream *es = b->elem;
	const uint32 n = b->n,
				words = (n + 63) / 64,
				first = es->layout.offsets[0],
				nelems = es->layout.offsets[n] - first;
	const size_t ew = sift_gather_width(r->type[r->ncols]);
	size_t		bytes = 0;
	char	   *at;

	sl->n = n;
	sl->nelems = nelems;
	grow_elems(r, sl, nelems);
	for (uint32 i = 0; i < n; i++)
		sl->lengths[i] = es->layout.lengths[i];
	for (uint32 i = 0; i <= n; i++)
		sl->offsets[i] = es->layout.offsets[i] - first;
	for (uint32 k = 0; k <= words; k++)
		sl->block_off[k] = es->layout.block_elem_off[k] - first;
	memcpy(sl->mask, mask, words * sizeof(uint64));
	memcpy(sl->doc_valid, es->doc_valid, words * sizeof(uint64));
	for (int i = 0; i < r->ncols; i++)
	{
		memcpy(sl->data[i], b->col[i].data, n * sift_gather_width(r->type[i]));
		copy_valid(sl->valid[i], &b->col[i], 0, n);
		sl->col[i] = (SiftVec) {.data = sl->data[i],.valid = sl->valid[i],.len = (uint16) n,.dense = true,.type = r->type[i]};
		bytes += long_bytes(&sl->col[i], n);
	}
	memcpy(sl->edata, (const char *) es->child_data[0] + first * ew, nelems * ew);
	copy_valid(sl->evalid, &(SiftVec) {.valid = (uint64 *) es->child_valid[0]}, first, nelems);
	sl->elem = (SiftVec) {.data = sl->edata,.valid = sl->evalid,.len = 0,.dense = true,.type = r->type[r->ncols]};
	bytes += long_bytes(&sl->elem, nelems);
	if (bytes > sl->strcap)
	{
		if (sl->str != NULL)
			pfree(sl->str);
		sl->strcap = Max(bytes, 64 * 1024);
		sl->str = MemoryContextAlloc(r->cx, sl->strcap);
	}
	at = sl->str;
	for (int i = 0; i < r->ncols; i++)
		at = own_strings(&sl->col[i], n, at);
	own_strings(&sl->elem, nelems, at);
}

/* ---- the batches the threads folded ---- */

static void
mark_folded(UnPar *r, uint32 seq, bool on)
{
	if (seq >= r->folded_cap * 8)
	{
		const uint32 cap = Max(r->folded_cap * 2, 1024);

		r->folded = r->folded == NULL ? MemoryContextAllocZero(r->cx, cap) : repalloc0(r->folded, r->folded_cap, cap);
		r->folded_cap = cap;
	}
	if (on)
		r->folded[seq >> 3] |= (uint8) (1 << (seq & 7));
	else
		r->folded[seq >> 3] &= (uint8) ~(1 << (seq & 7));
}

static bool
was_folded(const UnPar *r, uint32 seq)
{
	return seq >= r->nbatch || (seq < r->folded_cap * 8 && ((r->folded[seq >> 3] >> (seq & 7)) & 1));
}

/* ---- the ring ---- */

static bool
un_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	UnPar	   *r = arg;

	return sift_par_unnest_batch(r->un, worker, r->slot[lo % r->nslots], err, out);
}

static void
un_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	sift_v6_gsink_emit(((UnPar *) arg)->sink, worker, row);
}

/* The leader folds the rest itself when the threads leave it most of the slots (a NULL in a counted column of every batch) */
static bool
leader_folds(const UnPar *r)
{
	return r->nleft >= 8 && r->nleft * 2 > r->nbatch;
}

static bool
op_feed(void *vr, uint32 item)
{
	UnPar	   *r = vr;
	const SiftV6UnnestSrc *src = r->src;
	const SiftF2ExtBatch *b;
	uint64		mask[SIFT_VEC_WORDS];

	while (src->next(src->arg, &b, mask))
	{
		const uint32 seq = r->nbatch++;

		if (leader_folds(r))
		{
			src->fold(src->arg, b, mask);
			continue;
		}
		slot_fill(r, r->slot[item % r->nslots], b, mask);
		r->seq[item % r->nslots] = seq;
		mark_folded(r, seq, true);
		return true;
	}
	return false;
}

static void
op_release(void *vr, uint32 item)
{
}

static void
op_fallback(void *vr, uint32 item)
{
	UnPar	   *r = vr;
	const uint32 at = item % r->nslots;

	r->nleft++;
	mark_folded(r, r->seq[at], false);
	sift_un_slot_pairs(r->src->pairs, r->slot[at], r->src->left, NULL);
}

static bool
op_phase1(void *vr, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	return sift_par_group_ops.phase1(((UnPar *) vr)->grp, t, lo, hi, scratch, done, err);
}

static bool
op_seal(void *vr, uint32 *nparts, SiftErr *err)
{
	return sift_par_group_ops.seal(((UnPar *) vr)->grp, nparts, err);
}

static bool
op_phase2(void *vr, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	return sift_par_group_ops.phase2(((UnPar *) vr)->grp, t, part, work, err);
}

static void
op_finish(void *vr)
{
	sift_par_group_ops.finish(((UnPar *) vr)->grp);
}

static void
op_overflow(void *vr, const SiftParRange *left, uint32 nleft)
{
}

static const SiftParOps un_ops = {.feed_slot = op_feed,.release_slot = op_release,.fallback_item = op_fallback,.phase1 = op_phase1,.seal = op_seal,.phase2 = op_phase2,
	.finish = op_finish,.overflow_drain = op_overflow};

static void
un_fold(void *arg, const SiftParGroupRow *row)
{
	const UnPar *r = arg;

	sift_v6_pin_fold_words(r->g, r->spec, &r->sh, row);
}

/* The batches the threads had are read again and folded by the serial run, with the ones that were not read yet */
static void
redo_serial(UnPar *r)
{
	const SiftV6UnnestSrc *src = r->src;
	const SiftF2ExtBatch *b;
	uint64		mask[SIFT_VEC_WORDS];
	uint32		seq = 0;

	src->redo(src->arg);
	while (src->next(src->arg, &b, mask))
		if (was_folded(r, seq++))
			src->fold(src->arg, b, mask);
}

/*
 * The threads the leader makes for within work_mem (fewer than asked, or 0 when it does not hold two): each has its scratch, a sink of words, the slots of the ring, and what is left of its share
 * goes two thirds to its state (the tables and the partitions of its groups) and one third to its stream of groups
 */
static int
threads_fit(UnPar *r, int nthreads)
{
	const SiftParWordShape *sh = &r->sh;
	const uint64 words = (uint64) SIFT_UN_PAIRS_MAX * 8 * (sh->gs.nk + sh->nsum) + (uint64) sift_pc64(sh->gs.strs) * SIFT_UN_PAIRS_MAX * 28;
	const uint64 fixed = SCRATCH_BYTES + WORK_BYTES + words + 2 * sizeof(SiftUnSlot);
	const uint64 budget = (uint64) work_mem * 1024 * 3 / 4;
	uint64		share;

	nthreads = (int) Min((uint64) nthreads, budget / (fixed + STATE_BYTES + SINK_BYTES));
	if (nthreads < 2)
		return 0;
	share = budget / nthreads - fixed;
	r->state_bytes = share * 2 / 3;
	r->sink_bytes = share / 3;
	return nthreads;
}

bool
sift_v6_unnest_par(const SiftV6Spec *spec, SiftV6Group *g, Relation index, const SiftV6UnnestSrc *src)
{
	UnPar		run = {.src = src,.spec = spec,.g = g};
	UnPar	   *r = &run;
	int			nthreads = sift_par_pool_threads();
	MemoryContext old;
	SiftParArenas *ar;
	SiftParGroupHooks hooks;
	SiftErr		err = {0};
	bool		done,
				kept;

	if (nthreads < 2 || src->pipe->nfact - 1 > SIFT_UN_COLS_MAX || !sift_par_unnest_shape(spec, &r->sh))
		return false;
	if ((nthreads = threads_fit(r, nthreads)) < 2)
		return false;
	r->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 unnest run");
	old = MemoryContextSwitchTo(r->cx);
	r->ncols = src->pipe->nfact - 1;
	memcpy(r->type, src->pipe->type, src->pipe->nfact * sizeof(SiftVecType));
	r->nslots = 2 * nthreads;
	r->slot = palloc(r->nslots * sizeof(SiftUnSlot *));
	r->seq = palloc0(r->nslots * sizeof(uint32));
	for (uint32 s = 0; s < r->nslots; s++)
		r->slot[s] = slot_new(r);
	r->un = sift_par_unnest_new(r->cx, &r->sh, src->pipe, src->left, nthreads);
	r->sink = sift_v6_gsink_new(r->cx, &r->sh.gs, nthreads, r->sink_bytes);
	ar = sift_par_arenas_create(r->cx, nthreads, SCRATCH_BYTES, r->state_bytes, WORK_BYTES);
	hooks = (SiftParGroupHooks) {.fill = un_fill,.emit = un_emit,.arg = r};
	r->grp = sift_par_group_create(ar, &r->sh.gs, false, UINT64_MAX, &hooks, &err);
	MemoryContextSwitchTo(old);
	if (r->grp == NULL)
	{
		MemoryContextDelete(r->cx);
		return false;
	}
	done = sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = r->nslots}, &un_ops, r, ar, index);
	kept = done && !sift_v6_gsink_full(r->sink);
	if (kept)
		sift_v6_gsink_drain(r->sink, un_fold, r);
	else
		redo_serial(r);
	elog(DEBUG1, "sift agg v6 unnest: %u batches on %d threads, %s", r->nbatch, nthreads, kept ? "folded by the threads" : "the groups outgrew the stream: folded by the scan");
	MemoryContextDelete(r->cx);
	return true;
}
