/*
 * sift_aggsrc.c - the column source seam (sift_colsrc.h): the providers register here and the calls dispatch to the one that owns the
 * access method of the relation. A build whose providers are all absent never matches an aggregate scan.
 */
#include "postgres.h"

#include "utils/rel.h"

#include "sift_aggkern.h"
#include "sift_colsrc.h"

#define MAX_PROVIDERS	4

static const SiftSrcProvider *providers[MAX_PROVIDERS];
static int	nproviders;

void
sift_colsrc_register(const SiftSrcProvider *prov)
{
	if (nproviders == MAX_PROVIDERS)
		elog(ERROR, "sift: too many column source providers");
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

	if (prov == NULL)
		elog(ERROR, "sift: \"%s\" has no column source", RelationGetRelationName(rel));
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

void
sift_colsrc_close(SiftSrcSource *src)
{
	src->prov->close(src->impl);
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

	return prov != NULL && prov->tids != NULL && prov->zone != NULL && prov->read_rows != NULL;
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

void
sift_colsrc_read_rows(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	vec->ovf = NULL;
	src->prov->read_rows(src->impl, unit, leaf, rows, n, vec);
}

void
sift_colsrc_select(SiftSrcSource *src, const SiftFilter *keep)
{
	if (src->prov->select == NULL)
		elog(ERROR, "sift: this column source cannot be restricted to a match set");
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

void
sift_colsrc_read_sel(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	src->prov->read_sel(src->impl, unit, leaf, bits, vec);
}
