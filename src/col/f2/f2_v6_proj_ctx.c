/*
 * col/f2/f2_v6_proj_ctx.c - the candidates and the choice of a projection for a statement (col/f2/f2_v6_proj_ctx.h).
 */
#include "postgres.h"

#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"

#include "col/f2/f2_v6_proj_ctx.h"
#include "col/am.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_proj_overlay.h"
#include "col/f2/l0_stage.h"
#include "table/f2/f2_col_delta.h"
#include "access/xlog.h"

static const SiftProjDef *
def_for(const SiftProjDef *defs, int ndefs, const SiftManifestProjRef *ref)
{
	for (int i = 0; i < ndefs; i++)
		if (strcmp(defs[i].name, ref->name) == 0 && defs[i].spec_ok && defs[i].hash == ref->spec_hash)
			return &defs[i];
	return NULL;
}

/* The attributes a projection reads, as the bloom the column patches of a part are told in */
static uint64
projection_attrs(const SiftV6ProjHost *h, const SiftProjDef *def, const SiftProjColumns *cols)
{
	bool	   *used = palloc(Max(cols->n, 1) * sizeof(bool));
	uint64		bloom = 0;

	sift_proj_spec_uses(&def->spec, cols, used);
	for (int c = 0; c < cols->n; c++)
		if (used[c])
			bloom |= sift_cd_att_bit(h->store->leaf_attno[c]);
	pfree(used);
	return bloom;
}

static void
serve_source(const SiftV6ProjCtx *c, SiftV6ProjSource *s)
{
	const SiftV6BaseParts *b = &c->base;
	uint64		attrs = projection_attrs(c->h, s->def, &c->cols);

	s->serve = palloc(Max(b->n, 1) * sizeof(SiftProjPartServe));
	for (int i = 0; i < b->n; i++)
	{
		SiftProjServeIn in = {.ref = s->ref,.base = &b->part[i],.link = sift_proj_link_of(&s->parts, b->part[i].part_gen),.vis = b->vis[i],
		.part_has_pend = sift_proj_part_meets_pend(&b->part[i], b->seg_pdel[b->part_seg[i]]),.col_delta_bloom = b->cd_bloom[i],.proj_att_bloom = attrs};

		s->serve[i] = sift_proj_serve_part(&in);
		s->nserved += s->serve[i].serve != SPS_BASE;
	}
}

static int
sources(const SiftV6ProjCtx *c, const SiftProjDef *defs, int ndefs, SiftProjKind kind, SiftV6ProjSource **out)
{
	int			nrefs,
				ns = 0;
	SiftManifestProjRef *refs = sift_manifest_read_projs(c->h->rel, &c->meta, &nrefs);
	SiftV6ProjSource *src = palloc(Max(nrefs, 1) * sizeof(SiftV6ProjSource));

	for (int i = 0; i < nrefs; i++)
	{
		SiftV6ProjSource s = {.ref = &refs[i],.def = def_for(defs, ndefs, &refs[i])};

		if (refs[i].kind != kind || refs[i].state != SIFT_PROJ_READY || s.def == NULL)
			continue;
		s.parts = sift_manifest_read_proj_parts(c->h->rel, &refs[i]);
		serve_source(c, &s);
		if (s.nserved > 0)
			src[ns++] = s;
	}
	*out = src;
	return ns;
}

/* The plan and the metapage of one version of the manifest: a commit between the two reads is met by reading both again, a few times; false when the relation will not hold still */
static bool
pin_version(SiftV6ProjCtx *c)
{
	for (int attempt = 0; attempt < 4; attempt++)
	{
		SiftColMetaPageData meta = sift_col_meta_read(c->h->rel);

		if (meta.nprojs == 0)
			return false;
		c->plan = sift_plan_cache_get(c->h->rel, CurrentMemoryContext);
		if (c->plan->manifest_version == meta.manifest_version)
		{
			c->meta = meta;
			return true;
		}
	}
	return false;
}

/* A sift_table's slabs are written out as parts before the plan is pinned (a standby cannot, and reads the relation); a sift_col scan reads its slabs where they are */
static bool
settle_slabs(const SiftV6ProjHost *h)
{
	if (h->heap != NULL)
		return true;
	if (RecoveryInProgress())
	{
		const SiftPlanSnapshot *p = sift_plan_cache_get(h->rel, CurrentMemoryContext);

		for (int i = 0; i < p->npartitions; i++)
			if (p->partitions[i].l0_slab_blk != InvalidBlockNumber)
				return false;
		return true;
	}
	(void) sift_f2_view(h->rel, h->store, CurrentMemoryContext);
	return true;
}

bool
sift_v6_proj_ctx_open(SiftV6ProjCtx *c, const SiftV6ProjHost *h, SiftProjKind kind)
{
	SiftProjDef *defs;
	int			ndefs;

	*c = (SiftV6ProjCtx) {.h = h};
	if (!RecoveryInProgress() && sift_stage_has_pending(h->rel))
		sift_stage_flush(h->rel);
	if (!settle_slabs(h) || !pin_version(c))
		return false;
	c->cols = sift_proj_columns_of_store(h->rel, h->store);
	defs = sift_proj_defs_load(h->rel, &c->cols, &ndefs);
	c->base = sift_v6_host_base(h, c->plan, kind);
	if (c->base.n == 0)
		return false;
	c->nsrc = sources(c, defs, ndefs, kind, &c->src);
	return c->nsrc > 0;
}

/* The key_order of the relation as a sort spec: what its own parts give the statement for nothing, so that a projection is chosen only when it gives more */
static SiftProjSpec
base_order_of(const SiftF2Store *store)
{
	SiftProjSpec s = {.kind = SIFT_PROJ_SORT,.nkeys = store->nkey,.distinct_leaf = -1};

	for (int k = 0; k < store->nkey; k++)
		s.key[k] = (SiftProjKey) {.leaf = (int16) store->key_cols[k],.fn = SPF_COLUMN,.desc = (store->spec.sort_flags[k] & SIFT_KEY_DESC) != 0,
		.nulls_first = (store->spec.sort_flags[k] & SIFT_KEY_NULLS_FIRST) != 0};
	return s;
}

SiftV6ProjChoice
sift_v6_proj_ctx_choose(const SiftV6ProjCtx *c, const SiftV6ProjQuery *q)
{
	SiftV6ProjCand *cand = palloc(c->nsrc * sizeof(SiftV6ProjCand));
	SiftProjSpec base_order = base_order_of(c->h->store);
	SiftV6ProjInput in;

	for (int i = 0; i < c->nsrc; i++)
		cand[i] = (SiftV6ProjCand) {.ref = c->src[i].ref,.spec = &c->src[i].def->spec,.parts = &c->src[i].parts,.serve = c->src[i].serve};
	in = (SiftV6ProjInput) {.q = q,.cost = {seq_page_cost, random_page_cost, cpu_tuple_cost, cpu_operator_cost, cpu_index_tuple_cost},.base_order = base_order.nkeys > 0 ? &base_order : NULL,.base = c->base.part,.nbase = c->base.n,
	.ncols = c->h->store->ncols,.ncand = c->nsrc,.cand = cand};
	return sift_v6_proj_choose(&in, &c->cols);
}

const SiftPartDesc *
sift_v6_proj_part_of(const SiftV6ProjSource *s, uint64 gen)
{
	int			lo = 0,
				hi = s->parts.nparts - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (s->parts.parts[mid].part_gen == gen)
			return &s->parts.parts[mid];
		if (s->parts.parts[mid].part_gen < gen)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	SIFT_INVARIANT(false, "projection \"%s\" has no part %llu its link names", s->ref->name, (unsigned long long) gen);
	return NULL;
}

static int
gen_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(*(const uint64 *) a, *(const uint64 *) b);
}

uint64 *
sift_v6_proj_served_gens(const SiftV6ProjCtx *c, const SiftV6ProjSource *s, int *n)
{
	uint64	   *gens = palloc(Max(c->base.n, 1) * sizeof(uint64));

	*n = 0;
	for (int i = 0; i < c->base.n; i++)
		if (s->serve[i].serve != SPS_BASE)
			gens[(*n)++] = c->base.part[i].part_gen;
	qsort(gens, *n, sizeof(uint64), gen_cmp);
	return gens;
}

SiftV6ProjPred
sift_v6_proj_query_pred(const SiftProjPred *p)
{
	SiftV6ProjPred q = {.leaf = p->leaf,.fn = SPF_COLUMN,.unit = SPU_NONE,.nvals = p->nvals,.cell = p};

	switch (p->op)
	{
		case SPO_EQ:
			q.op = SV6P_EQ;
			q.sel = DEFAULT_EQ_SEL;
			break;
		case SPO_IN:
			q.op = SV6P_IN;
			q.sel = Min(1.0, Max(p->nvals, 1) * DEFAULT_EQ_SEL);
			break;
		case SPO_NE:
			q.op = SV6P_NE;
			q.sel = 1.0 - DEFAULT_EQ_SEL;
			break;
		default:
			q.op = SV6P_RANGE;
			q.sel = DEFAULT_INEQ_SEL;
			break;
	}
	return q;
}
