/*
 * agg/colsrc.c - the column source seam (agg/colsrc.h): the providers register here and the calls dispatch to the one that owns the
 * access method of the relation. A build whose providers are all absent never matches an aggregate scan.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/colsrc.h"

#define MAX_PROVIDERS	4

static const SiftSrcProvider *providers[MAX_PROVIDERS];
static int	nproviders;

void
sift_colsrc_register(const SiftSrcProvider *prov)
{
	SIFT_INVARIANT(nproviders < MAX_PROVIDERS, "sift: too many column source providers");
	providers[nproviders++] = prov;
}

static const SiftSrcProvider *
provider_of(Oid relam, bool index)
{
	for (int i = 0; i < nproviders; i++)
		if (index ? providers[i]->owns_index(relam) : providers[i]->owns_table(relam))
			return providers[i];
	return NULL;
}

bool
sift_colsrc_is_index(Oid indexrelam)
{
	return provider_of(indexrelam, true) != NULL;
}

bool
sift_colsrc_is_table(Oid tablerelam)
{
	return provider_of(tablerelam, false) != NULL;
}

/* the provider of an index or table relation */
static const SiftSrcProvider *
provider_of_rel(Relation rel)
{
	bool		index = rel->rd_rel->relkind == RELKIND_INDEX;
	const SiftSrcProvider *prov = provider_of(rel->rd_rel->relam, index);

	SIFT_INVARIANT(prov != NULL, "sift: \"%s\" has no column source", RelationGetRelationName(rel));
	return prov;
}

SiftSrcLayout *
sift_colsrc_layout(Relation rel)
{
	return provider_of_rel(rel)->layout(rel);
}

void
sift_colsrc_stats(Relation rel, SiftSrcStats *st)
{
	provider_of_rel(rel)->stats(rel, st);
}

void
sift_colsrc_stamp(Relation index, SiftSrcStamp *stamp)
{
	provider_of_rel(index)->stamp(index, stamp);
}

void
sift_colsrc_preagg(Relation rel, SiftSrcPreagg *pa)
{
	const SiftSrcProvider *prov = provider_of_rel(rel);

	memset(pa, 0, sizeof(*pa));
	if (prov->preagg)
		prov->preagg(rel, pa);
}

SiftSrcSource *
sift_colsrc_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	const SiftSrcProvider *prov = provider_of_rel(index ? index : heap);
	SiftSrcSource *src = MemoryContextAlloc(mcxt, sizeof(SiftSrcSource));

	src->prov = prov;
	src->impl = prov->open(heap, index, snapshot, mcxt);
	return src;
}

bool
sift_colsrc_can_part(Oid indexrelam)
{
	const SiftSrcProvider *prov = provider_of(indexrelam, true);

	return prov != NULL && prov->open_part != NULL;
}

SiftSrcSource *
sift_colsrc_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	const SiftSrcProvider *prov = provider_of_rel(index);
	SiftSrcSource *src = MemoryContextAlloc(mcxt, sizeof(SiftSrcSource));

	src->prov = prov;
	src->impl = prov->open_part(heap, index, snapshot, mcxt);
	return src;
}

void
sift_colsrc_close(SiftSrcSource *src)
{
	src->prov->close(src->impl);
}

void
sift_colsrc_unpin(SiftSrcSource *src)
{
	if (src->prov->unpin)
		src->prov->unpin(src->impl);
}

const SiftSrcLayout *
sift_colsrc_source_layout(const SiftSrcSource *src)
{
	return src->prov->source_layout(src->impl);
}

uint32
sift_colsrc_nsb(SiftSrcSource *src)
{
	return src->prov->nsb(src->impl);
}

bool
sift_colsrc_can_select(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->select != NULL;
}

bool
sift_colsrc_can_tids(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->tids != NULL && prov->zone != NULL && prov->zone_range != NULL && prov->read_rows != NULL;
}

bool
sift_colsrc_can_types(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->filter_types != NULL && prov->tolerate_ovf != NULL && prov->read_types != NULL;
}

bool
sift_colsrc_can_jnull(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->read_jnull != NULL;
}

bool
sift_colsrc_can_wide(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->set_wide != NULL && prov->filter_wide != NULL;
}

int
sift_colsrc_set_wide(SiftSrcSource *src, const SiftSrcWideTest *t, int nt)
{
	return src->prov->set_wide(src->impl, t, nt);
}

bool
sift_colsrc_filter_wide(SiftSrcSource *src, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	return src->prov->filter_wide(src->impl, unit, i, rows);
}

bool
sift_colsrc_filter_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	return src->prov->filter_types(src->impl, unit, leaf, types, bits);
}

void
sift_colsrc_read_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	src->prov->read_types(src->impl, unit, leaf, tags);
}

void
sift_colsrc_read_jnull(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	src->prov->read_jnull(src->impl, unit, leaf, bits);
}

bool
sift_colsrc_can_recheck(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->set_recheck != NULL && prov->overflow_raise != NULL;
}

void
sift_colsrc_set_recheck(SiftSrcSource *src, int leaf, SiftSrcRecheck mode, const char *key, Oid cast)
{
	src->prov->set_recheck(src->impl, leaf, mode, key, cast);
}

void
sift_colsrc_overflow_raise(SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rows)
{
	if (src->prov->overflow_raise)
		src->prov->overflow_raise(src->impl, unit, rows);
}

uint64 *
sift_colsrc_recheck_rows(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->recheck_rows ? src->prov->recheck_rows(src->impl, unit) : NULL;
}

void
sift_colsrc_tolerate_ovf(SiftSrcSource *src, int leaf)
{
	src->prov->tolerate_ovf(src->impl, leaf);
}

bool
sift_colsrc_can_raw(Oid relam, bool index)
{
	const SiftSrcProvider *prov = provider_of(relam, index);

	return prov != NULL && prov->set_raw != NULL;
}

void
sift_colsrc_set_raw(SiftSrcSource *src, int leaf)
{
	src->prov->set_raw(src->impl, leaf);
}

void
sift_colsrc_tids(SiftSrcSource *src, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	src->prov->tids(src->impl, unit, rows, n, tids);
}

void
sift_colsrc_zone(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z)
{
	src->prov->zone(src->impl, unit, leaf, z);
}

uint32
sift_colsrc_zone_range(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	return src->prov->zone_range(src->impl, lo, hi, leaf, z);
}

bool
sift_colsrc_has_zone_range(SiftSrcSource *src)
{
	return src->prov->zone_range != NULL;
}

bool
sift_colsrc_overflow(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	return src->prov->overflow && src->prov->overflow(src->impl, unit, leaf);
}

void
sift_colsrc_read_rows(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	vec->ovf = NULL;
	src->prov->read_rows(src->impl, unit, leaf, rows, n, vec);
}

bool
sift_colsrc_run_info(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	return src->prov->run_info && src->prov->run_info(src->impl, unit, leaf, nvals, nnull, id);
}

void
sift_colsrc_run_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows)
{
	src->prov->run_read(src->impl, unit, leaf, pos, cnt, rows);
}

void
sift_colsrc_select(SiftSrcSource *src, const SiftFilter *keep)
{
	SIFT_INVARIANT(src->prov->select != NULL, "sift: this column source cannot be restricted to a match set");
	src->prov->select(src->impl, keep);
}

void
sift_colsrc_begin(SiftSrcSource *src, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	src->prov->begin(src->impl, sb_lo, sb_hi, prune, nprune);
}

bool
sift_colsrc_next(SiftSrcSource *src, SiftSrcUnit *unit)
{
	return src->prov->next(src->impl, unit);
}

void
sift_colsrc_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	vec->ovf = NULL;
	src->prov->read(src->impl, unit, leaf, vec);
}

int64
sift_colsrc_leaf_n(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	SiftSrcVec	vec;

	if (src->prov->leaf_n)
		return src->prov->leaf_n(src->impl, unit, leaf);
	src->prov->read(src->impl, unit, leaf, &vec);
	return vec.n;
}

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

void
sift_colsrc_read_into(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	size_t		sz;

	if (src->prov->read_into)
	{
		src->prov->read_into(src->impl, unit, leaf, buf, vec);
		return;
	}
	src->prov->read(src->impl, unit, leaf, vec);
	sz = vtype_size[src->prov->source_layout(src->impl)->leaf[leaf].vtype];
	memcpy(buf->data, vec->data, vec->n * sz);
	memset((char *) buf->data + vec->n * sz, 0, SIFT_AGG_PAD * sz);
	vec->data = buf->data;
	if (vec->isnull)
	{
		memcpy(buf->isnull, vec->isnull, vec->n);
		vec->isnull = buf->isnull;
	}
}

void
sift_colsrc_live(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits)
{
	src->prov->live(src->impl, unit, bits);
}

void
sift_colsrc_vet(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits)
{
	src->prov->vet(src->impl, unit, bits);
}

bool
sift_colsrc_vet_needed(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->vet_needed == NULL || src->prov->vet_needed(src->impl, unit);
}

void
sift_colsrc_counters(const SiftSrcSource *src, SiftSrcCounters *c)
{
	src->prov->counters(src->impl, c);
}

uint64
sift_colsrc_set_filters(SiftSrcSource *src, const SiftSrcFilter *f, int nf)
{
	return src->prov->set_filters ? src->prov->set_filters(src->impl, f, nf) : 0;
}

bool
sift_colsrc_filter(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits)
{
	return src->prov->filter(src->impl, unit, bits);
}

bool
sift_colsrc_can_read_sel(const SiftSrcSource *src)
{
	return src->prov->read_sel != NULL;
}

bool
sift_colsrc_can_narrow(const SiftSrcSource *src)
{
	return src->prov->narrow != NULL;
}

bool
sift_colsrc_narrow(SiftSrcSource *src, SiftSrcUnit *unit, const uint64 *bits)
{
	return src->prov->narrow(src->impl, unit, bits);
}

void
sift_colsrc_read_sel(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	src->prov->read_sel(src->impl, unit, leaf, bits, vec);
}

void
sift_colsrc_nan_values(const SiftSrcLeaf *l, void *data, uint8 *isnull, const SiftSrcVec *vec)
{
	if (!vec->ovf || !sift_colsrc_nan_leaf(l))
		return;
	for (size_t w = 0; w * 64 < vec->n; w++)
		for (uint64 b = vec->ovf[w]; b; b &= b - 1)
		{
			size_t		i = w * 64 + pg_rightmost_one_pos64(b);

			((int64 *) data)[i] = PG_INT64_MAX;
			isnull[i] = 0;
		}
}
