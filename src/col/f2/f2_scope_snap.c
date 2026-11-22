/*
 * col/f2/f2_scope_snap.c - the settled change of a summary kept for the statements that follow (f2_scope_snap.h).
 */
/* check-memory: allow-allocs the reads kept are bounded by SNAP_MAX_READS and each is the cube of one key */
#include "postgres.h"

#include "common/hashfn.h"
#include "utils/memutils.h"

#include "col/f2/delvec_delta.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/f2_scope_snap.h"
#include "col/f2/l0_href_fetch.h"
#include "col/f2/l0_slab.h"
#include "col/f2/part_vis.h"
#include "col/f2/pend_del.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

#define SNAP_MAX_READS	128U

typedef struct Stamp
{
	uint64		hash;
	FullTransactionId newest;
} Stamp;

static void
stamp_add(Stamp *s, uint64 v)
{
	s->hash = hash_combine64(s->hash, murmurhash64(v));
}

static void
stamp_xid(Stamp *s, FullTransactionId x)
{
	if (FullTransactionIdIsValid(x) && (!FullTransactionIdIsValid(s->newest) || FullTransactionIdFollows(x, s->newest)))
		s->newest = x;
}

static void
stamp_chain(Stamp *s, const SiftChainStamp *c)
{
	stamp_add(s, c->lsn);
	stamp_add(s, c->count);
	stamp_add(s, c->pages);
	stamp_xid(s, c->max_xid);
}

bool
sift_f2s_overlay_stamp(SiftF2ScopeParts *p, const SiftManifestRead *mr, uint64 *out)
{
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);
	Stamp		s = {0};
	bool		href = false;

	for (int k = 0; k < mr->n; k++)
	{
		const SiftPartitionEntry *e = &mr->entries[k];
		const uint64 v[] = {e->part_id, e->manifest_gen, e->total_rows, e->total_dead, e->l0_count, e->flags, e->l0_slab_blk, e->pend_del_blk};

		for (int i = 0; i < lengthof(v); i++)
			stamp_add(&s, v[i]);
		stamp_xid(&s, e->max_xid);
		if (e->l0_slab_blk != InvalidBlockNumber)
		{
			href |= sift_l0_slab_is_href(p->rel, e->part_id, e->l0_slab_blk);
			if (!href)
			{
				const SiftChainStamp c = sift_l0_slab_stamp(p->rel, e->part_id, e->l0_slab_blk);

				stamp_chain(&s, &c);
			}
		}
		if (e->pend_del_blk != InvalidBlockNumber)
		{
			const SiftChainStamp c = sift_pend_del_stamp(p->rel, e->part_id, e->pend_del_blk);

			stamp_chain(&s, &c);
		}
	}
	for (int i = 0; i < p->n + p->nadd; i++)
	{
		const SiftPartDesc *d = &p->s[i].d;
		const uint64 v[] = {p->s[i].part_gen, d->dv_gen, d->ndead};

		for (int j = 0; j < lengthof(v); j++)
			stamp_add(&s, v[j]);
		stamp_xid(&s, d->max_xid);
		stamp_xid(&s, d->dv_max_xid);
		if (d->dv_blk != InvalidBlockNumber)
		{
			const SiftChainStamp c = sift_dv_stamp(p->rel, d->dv_blk);

			stamp_chain(&s, &c);
		}
	}
	*out = s.hash;
	return !href && FullTransactionIdIsValid(xmin) && (!FullTransactionIdIsValid(s.newest) || FullTransactionIdPrecedes(s.newest, xmin));
}

SiftF2ScopeSnap *
sift_f2_scope_snap_find(const SiftScopeMeta *cached, uint64 stamp)
{
	MemoryContext cx;
	SiftF2ScopeSnap **slot = sift_f2_scope_cache_overlay(cached, &cx);

	return *slot != NULL && (*slot)->stamp == stamp ? *slot : NULL;
}

SiftF2ScopeSnap *
sift_f2_scope_snap_start(const SiftScopeMeta *cached, uint64 stamp)
{
	MemoryContext parent;
	SiftF2ScopeSnap **slot = sift_f2_scope_cache_overlay(cached, &parent);
	MemoryContext cx = sift_mem_allocset_create(parent, "sift cache scope settled change");
	SiftF2ScopeSnap *sn = MemoryContextAllocZero(cx, sizeof(*sn));

	sn->cx = cx;
	sn->stamp = stamp;
	sn->slot = slot;
	return sn;
}

void
sift_f2_scope_snap_keep(const SiftScopeMeta *cached, SiftF2ScopeSnap *sn)
{
	/* MEM[MEM-SCOPE-05]: settled-change rowsets in the entry cx, charged by snap_charge (was free) */
	if (*sn->slot != NULL)
		MemoryContextDelete((*sn->slot)->cx);
	*sn->slot = sn;
}

void
sift_f2_scope_snap_drop(SiftF2ScopeSnap *sn)
{
	if (sn->slot != NULL && *sn->slot == sn)
		*sn->slot = NULL;
	MemoryContextDelete(sn->cx);
}

/* It stays with the backend while it fits the cache budget and the cache is under its limit once it is counted (evicting the oldest others for it): what does not goes with the statement */
static void
snap_charge(SiftF2ScopeSnap *sn)
{
	const Size	limit = (Size) sift_backend_cache_kb * 1024;

	if (sn->slot == NULL || *sn->slot != sn || (MemoryContextMemAllocated(sn->cx, true) <= limit && sift_cache_commit()))
		return;
	*sn->slot = NULL;
	sn->slot = NULL;
	MemoryContextSetParent(sn->cx, sn->reader);
}

void
sift_f2_scope_snap_grown(SiftF2ScopeSnap *sn, MemoryContext reader)
{
	sn->reader = reader;
	snap_charge(sn);
}

static bool
in_list(const int64 *v, uint32 n, const int64 *w, uint32 nw)
{
	return n == nw && (n == 0 || memcmp(v, w, n * sizeof(int64)) == 0);
}

static bool
read_is(const SiftF2ScopeRead *r, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed)
{
	return r->key == key && r->month == q->month && r->minmax == q->minmax && r->distinct == q->distinct && in_list(r->changed, r->nchanged, ed->changed, ed->nchanged) &&
		in_list(r->own, r->nown, ed->own, ed->nown);
}

bool
sift_f2_scope_snap_recall(const SiftF2ScopeSnap *sn, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed, SiftScopeCubeAcc *out, bool *has)
{
	for (uint32 i = 0; i < sn->nreads; i++)
		if (read_is(&sn->reads[i], key, q, ed))
		{
			*has = sn->reads[i].has;
			if (*has)
				sift_scube_copy(out, &sn->reads[i].cube);
			return true;
		}
	return false;
}

static void
read_free(SiftF2ScopeRead *r)
{
	if (r->has)
		sift_scube_free(&r->cube);
	pfree(r->changed);
	pfree(r->own);
}

void
sift_f2_scope_snap_remember(SiftF2ScopeSnap *sn, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed, const SiftScopeCubeAcc *cube)
{
	/* MEM[MEM-SCOPE-05]: cached reads, up to 128 cubes, charged to the cache budget (was free) */
	MemoryContext old = MemoryContextSwitchTo(sn->cx);
	SiftF2ScopeRead *r;

	if (sn->reads == NULL)
		sn->reads = palloc0(SNAP_MAX_READS * sizeof(SiftF2ScopeRead));
	r = &sn->reads[sn->next];
	if (sn->nreads == SNAP_MAX_READS)
		read_free(r);
	else
		sn->nreads++;
	sn->next = (sn->next + 1) % SNAP_MAX_READS;
	*r = (SiftF2ScopeRead) {.key = key,.month = q->month,.minmax = q->minmax,.distinct = q->distinct,.nchanged = ed->nchanged,.nown = ed->nown,.has = cube != NULL,
		.changed = memcpy(palloc(Max(ed->nchanged, 1) * sizeof(int64)), ed->changed, ed->nchanged * sizeof(int64)),
		.own = memcpy(palloc(Max(ed->nown, 1) * sizeof(int64)), ed->own, ed->nown * sizeof(int64))};
	if (cube != NULL)
		sift_scube_copy(&r->cube, cube);
	MemoryContextSwitchTo(old);
	snap_charge(sn);
}
