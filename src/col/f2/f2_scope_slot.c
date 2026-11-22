/*
 * col/f2/f2_scope_slot.c - the entity parts of a scope as f2_scope_ent.c reads them: the part opened when a granule asks, its directory of granules, its zones and deletion vectors, and the chunk of
 * the part that holds a leaf of the cubes.
 */
#include "postgres.h"

#include "miscadmin.h"

#include "col/f2/delvec.h"
#include "core/page_io.h"
#include "table/f2/f2_tbl.h"

#include "col/f2/f2_scope_int.h"

SiftPart *
sift_f2s_slot_part(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	if (s->part == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->part = sift_part_open_desc(p->rel, &s->d, (uint32) s->part_id);
		MemoryContextSwitchTo(old);
	}
	return s->part;
}

SiftF2ScopeSlot *
sift_f2s_slot_of(SiftF2ScopeParts *p, uint32 gidx, uint32 *g)
{
	int			lo = 0,
				hi = p->n - 1;

	while (lo < hi)
	{
		int			mid = (lo + hi + 1) / 2;

		if (p->s[mid].base <= gidx)
			lo = mid;
		else
			hi = mid - 1;
	}
	*g = gidx - p->s[lo].base;
	return &p->s[lo];
}

void
sift_f2s_gen_map_init(SiftGenMap *gm, uint32 expected)
{
	uint32		cap = 16;

	while (cap < expected * 2)
		cap *= 2;
	gm->gen = palloc0((Size) cap * sizeof(uint64));
	gm->val = palloc((Size) cap * sizeof(uint32));
	gm->mask = cap - 1;
}

static inline uint32
gen_slot(const SiftGenMap *gm, uint64 gen)
{
	return (uint32) ((gen * UINT64_C(0x9E3779B97F4A7C15)) >> 32) & gm->mask;
}

void
sift_f2s_gen_map_put(SiftGenMap *gm, uint64 gen, uint32 val)
{
	uint32		at = gen_slot(gm, gen);

	while (gm->gen[at] != 0 && gm->gen[at] != gen)
		at = (at + 1) & gm->mask;
	gm->gen[at] = gen;
	gm->val[at] = val;
}

bool
sift_f2s_gen_map_find(const SiftGenMap *gm, uint64 gen, uint32 *val)
{
	for (uint32 at = gen_slot(gm, gen); gm->gen[at] != 0; at = (at + 1) & gm->mask)
		if (gm->gen[at] == gen)
		{
			*val = gm->val[at];
			return true;
		}
	return false;
}

/* The first slot of a part generation: the map holds the slots seen so far and takes the ones added since (the slot array may move, the indexes do not) */
SiftF2ScopeSlot *
sift_f2s_slot_of_gen(SiftF2ScopeParts *p, uint64 gen)
{
	uint32		at;

	if (p->gen_map_n != p->n)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		if (p->gen_map.gen == NULL || (uint32) p->n * 2 > p->gen_map.mask + 1)
		{
			sift_f2s_gen_map_init(&p->gen_map, (uint32) p->n * 2);
			p->gen_map_n = 0;
		}
		for (int i = p->gen_map_n; i < p->n; i++)
		{
			uint32		have;

			if (p->s[i].part_gen != 0 && !sift_f2s_gen_map_find(&p->gen_map, p->s[i].part_gen, &have))
				sift_f2s_gen_map_put(&p->gen_map, p->s[i].part_gen, (uint32) i);
		}
		p->gen_map_n = p->n;
		MemoryContextSwitchTo(old);
	}
	return gen != 0 && sift_f2s_gen_map_find(&p->gen_map, gen, &at) ? &p->s[at] : NULL;
}

const struct SiftVariantExcSummary *
sift_f2s_slot_exc(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 *n)
{
	if (!s->exc_read)
	{
		uint64		len;
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->exc = sift_part_read_section(p->rel, sift_f2s_slot_part(p, s), SEC_VARIANT_EXC, &len);
		s->nexc = s->exc == NULL ? 0 : (uint32) (len / sizeof(SiftVariantExcSummary));
		s->exc_read = true;
		MemoryContextSwitchTo(old);
	}
	*n = s->nexc;
	return s->exc;
}

const SiftGranuleDesc *
sift_f2s_slot_gdir(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	if (s->gdir == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->gdir = sift_part_granule_dir(p->rel, sift_f2s_slot_part(p, s));
		MemoryContextSwitchTo(old);
	}
	return s->gdir;
}

/* The chunk of the part that holds the leaf: the leaf itself in an index and in a part that has the schema of the table */
int
sift_f2s_slot_phys(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, int leaf)
{
	if (!s->map_read)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->map = p->store->host->build_col_map != NULL ? p->store->host->build_col_map(p->store, p->rel, sift_f2s_slot_part(p, s), CurrentMemoryContext) : NULL;
		s->map_read = true;
		MemoryContextSwitchTo(old);
	}
	return s->map == NULL ? leaf : s->map->phys_col[leaf];
}

SiftZone
sift_f2s_slot_zone(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, int leaf, uint32 g)
{
	SiftPart   *part = sift_f2s_slot_part(p, s);
	int			phys = sift_f2s_slot_phys(p, s, leaf);

	if (s->zones == NULL)
	{
		uint64		len;
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->zones = sift_part_read_section(p->rel, part, SEC_ZONES, &len);
		MemoryContextSwitchTo(old);
		if (s->zones == NULL || len != (uint64) part->hdr.ncols * s->ng * sizeof(SiftZone))
			SIFT_CORRUPT(p->rel, "The zones of part %llu are not those of its granules.", (unsigned long long) s->part_gen);
	}
	return s->zones[(uint64) phys * s->ng + g];
}

const SiftDvGran *
sift_f2s_slot_dv(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g)
{
	if (s->d.dv_blk == InvalidBlockNumber)
		return NULL;
	if (!s->dv_read)
	{
		const SiftGranuleDesc *dir = sift_f2s_slot_gdir(p, s);
		uint16	   *rows = palloc(Max(s->ng, 1) * sizeof(uint16));
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		for (uint32 i = 0; i < s->ng; i++)
			rows[i] = dir[i].nrows;
		s->dv = sift_dv_read(p->rel, &s->d, (uint32) s->part_id, rows);
		s->dv_read = true;
		MemoryContextSwitchTo(old);
	}
	return sift_dv_granule(s->dv, g);
}
