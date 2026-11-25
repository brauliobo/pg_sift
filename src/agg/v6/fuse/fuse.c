/*
 * agg/v6/fuse/fuse.c - the cohorts of SiftV6Fuse (v6_fuse.h).
 */
/* check-memory: allow-allocs the registry follows the arms of one statement, a few */
#include "postgres.h"

#include "agg/vec.h"
#include "agg/v6/fuse/fuse.h"

/* ---- the unions ---- */

typedef struct Span
{
	int64		lo;				/* INT64_MIN: unbounded below */
	int64		hi;				/* exclusive; unbounded above when open */
	bool		open;
	const SiftF2KeyRange *lo_src,
			   *hi_src;
} Span;

static int
spans_of(const SiftF2KeySet *ks, Span *out)
{
	static const SiftF2KeyRange everything;
	int			n = 0;

	if (ks->n == 0)
		out[n++] = (Span) {.lo = INT64_MIN,.open = true,.lo_src = &everything,.hi_src = &everything};
	for (int i = 0; i < ks->n; i++)
	{
		const SiftF2KeyRange *r = &ks->range[i];

		out[n++] = (Span) {.lo = r->has_lo ? r->ilo : INT64_MIN,.hi = r->ihi,.open = !r->has_hi,.lo_src = r,.hi_src = r};
	}
	return n;
}

static void
spans_merge(const Span *a, int na, const Span *b, int nb, Span *out)
{
	int			i = 0,
				j = 0,
				n = 0;

	while (i < na && j < nb)
		out[n++] = b[j].lo < a[i].lo ? b[j++] : a[i++];
	while (i < na)
		out[n++] = a[i++];
	while (j < nb)
		out[n++] = b[j++];
}

bool
sift_v6_fuse_union_keys(const SiftF2KeySet *a, const SiftF2KeySet *b, SiftF2KeySet *out)
{
	Span		sa[SIFT_F2_MAX_RANGES + 1],
				sb[SIFT_F2_MAX_RANGES + 1],
				all[2 * SIFT_F2_MAX_RANGES + 2];
	int			na,
				nb,
				n,
				merged = 0;

	for (int i = 0; i < a->n; i++)
		if (a->range[i].null_only)
			return false;
	for (int i = 0; i < b->n; i++)
		if (b->range[i].null_only)
			return false;
	na = spans_of(a, sa);
	nb = spans_of(b, sb);
	n = na + nb;
	spans_merge(sa, na, sb, nb, all);
	for (int i = 0; i < n; i++)
	{
		Span	   *cur = merged > 0 ? &all[merged - 1] : NULL;

		if (cur != NULL && (cur->open || all[i].lo <= cur->hi))
		{
			if (!cur->open && (all[i].open || all[i].hi > cur->hi))
			{
				cur->hi = all[i].hi;
				cur->open = all[i].open;
				cur->hi_src = all[i].hi_src;
			}
			continue;
		}
		all[merged++] = all[i];
	}
	if (merged > SIFT_F2_MAX_RANGES)
		return false;
	*out = (SiftF2KeySet) {.n = merged};
	for (int i = 0; i < merged; i++)
	{
		SiftF2KeyRange r = *all[i].lo_src;

		memcpy(r.hi, all[i].hi_src->hi, sizeof(r.hi));
		r.has_lo = all[i].lo != INT64_MIN;
		r.has_hi = !all[i].open;
		r.null_only = false;
		r.ilo = all[i].lo_src->ilo;
		r.ihi = all[i].hi;
		out->range[i] = r;
	}
	out->hull = out->range[0];
	memcpy(out->hull.hi, out->range[merged - 1].hi, sizeof(out->hull.hi));
	out->hull.has_hi = out->range[merged - 1].has_hi;
	out->hull.ihi = out->range[merged - 1].ihi;
	return true;
}

bool
sift_v6_fuse_union_time(const SiftF2TimeWin *a, const SiftF2TimeWin *b, SiftF2TimeWin *out)
{
	*out = (SiftF2TimeWin) {0};
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		double		wa,
					wb,
					wu;

		if (!a->use[s] || !b->use[s])
			continue;
		out->use[s] = true;
		out->lo[s] = Min(a->lo[s], b->lo[s]);
		out->hi[s] = Max(a->hi[s], b->hi[s]);
		wa = (double) a->hi[s] - (double) a->lo[s] + 1.0;
		wb = (double) b->hi[s] - (double) b->lo[s] + 1.0;
		wu = (double) out->hi[s] - (double) out->lo[s] + 1.0;
		if (wu > 1.25 * (wa + wb))
			return false;
	}
	return true;
}

/* ---- the registry ---- */

typedef struct RegEntry
{
	struct RegEntry *next;
	EState	   *estate;
	SiftV6FuseReg reg;
	MemoryContextCallback cb;
} RegEntry;

static RegEntry *regs;

static void
reg_gone(void *arg)
{
	RegEntry  **pp = &regs;

	for (; *pp != NULL; pp = &(*pp)->next)
		if (*pp == (RegEntry *) arg)
		{
			*pp = (*pp)->next;
			return;
		}
}

SiftV6FuseReg *
sift_v6_fuse_reg(EState *estate)
{
	RegEntry   *e;

	for (e = regs; e != NULL; e = e->next)
		if (e->estate == estate)
			return &e->reg;
	e = MemoryContextAllocZero(estate->es_query_cxt, sizeof(RegEntry));
	e->estate = estate;
	e->cb = (MemoryContextCallback) {.func = reg_gone,.arg = e};
	e->next = regs;
	regs = e;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &e->cb);
	return &e->reg;
}

void
sift_v6_fuse_arm_add(EState *estate, SiftV6FuseArm *arm)
{
	SiftV6FuseReg *r = sift_v6_fuse_reg(estate);
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);

	r->arms = lappend(r->arms, arm);
	MemoryContextSwitchTo(old);
	arm->started = false;
	arm->detached = false;
	arm->cohort = NULL;
}

static bool
excluded(const SiftV6FuseArm *a)
{
	return a->parallel || a->param_exec || a->cube_served;
}

static bool
may_join(const SiftV6FuseArm *lead, const SiftV6FuseArm *b)
{
	return b != lead && !b->started && !b->detached && !excluded(b) && b->indexoid == lead->indexoid && b->snap == lead->snap && b->ordered == lead->ordered;
}

SiftV6FuseCohort *
sift_v6_fuse_lead(EState *estate, SiftV6FuseArm *arm, bool shared_keys_only, bool explain_only)
{
	SiftV6FuseReg *r = sift_v6_fuse_reg(estate);
	SiftV6FuseCohort *c;

	if (arm->started || arm->detached || explain_only || excluded(arm))
	{
		arm->started = true;
		return NULL;
	}
	c = MemoryContextAllocZero(estate->es_query_cxt, sizeof(SiftV6FuseCohort));
	c->arms[c->narms++] = arm;
	c->same_keys = true;
	c->union_keys = arm->key_set;
	c->union_time = arm->time_win;
	foreach_ptr(SiftV6FuseArm, b, r->arms)
	{
		SiftF2KeySet keys;
		SiftF2TimeWin time;

		if (c->narms >= SIFT_V6_FUSE_MAX_ARMS)
			break;

		if (!may_join(arm, b))
			continue;
		if (b->scan_sig != arm->scan_sig)
		{
			if (shared_keys_only || (arm->may_union != NULL && !arm->may_union(arm, b)) || !sift_v6_fuse_union_keys(&c->union_keys, &b->key_set, &keys) || !sift_v6_fuse_union_time(&c->union_time, &b->time_win, &time))
				continue;
			c->union_keys = keys;
			c->union_time = time;
			c->same_keys = false;
		}
		c->arms[c->narms++] = b;
	}
	arm->started = true;
	if (c->narms == 1)
	{
		pfree(c);
		return NULL;
	}
	for (int i = 0; i < c->narms; i++)
	{
		c->arms[i]->started = true;
		c->arms[i]->cohort = c;
	}
	return c;
}

void
sift_v6_fuse_detach(SiftV6FuseArm *arm)
{
	SiftV6FuseCohort *c = arm->cohort;

	arm->detached = true;
	arm->cohort = NULL;
	if (c == NULL)
		return;
	for (int i = 0; i < c->narms; i++)
		if (c->arms[i] == arm)
		{
			memmove(&c->arms[i], &c->arms[i + 1], sizeof(SiftV6FuseArm *) * (c->narms - i - 1));
			c->narms--;
			break;
		}
	if (c->narms == 0)
		pfree(c);
}

const char *
sift_v6_fuse_role(const SiftV6FuseArm *arm)
{
	if (arm->cohort == NULL)
		return NULL;
	return arm->cohort->arms[0] == arm ? "leader" : "follower";
}

int
sift_v6_fuse_dispatch(SiftV6FuseCohort *c, const SiftV6FuseOps *ops, void *ctx, const uint64 *live, uint16 n_live)
{
	uint64		mask[SIFT_VEC_WORDS];
	int			consumed = 0;

	if (n_live == 0)
	{
		c->granules_pruned++;
		return 0;
	}
	c->granules_scanned++;
	c->rows_shared_vetted += n_live;
	for (int i = 0; i < c->narms; i++)
	{
		const uint16 n = ops->arm_mask(ctx, i, live, mask);

		if (n == 0)
			continue;
		ops->consume(ctx, i, mask, n);
		consumed++;
	}
	return consumed;
}
