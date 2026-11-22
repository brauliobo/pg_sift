/*
 * core/f2/manifest/check.c - the check of the partition directory of format major 2: its tiling of the key space, the totals each entry and the metapage keep and the header blocks of the parts.
 */
#include "core/host.h"

#include "core/f2/manifest/manifest_int.h"
#include "col/f2/part_format.h"
#include "core/page_io.h"

/* A part an edit adds lies inside its partition and has a header page, or is in object storage and has none */
void
sift_mf_check_added(Relation rel, const SiftPartitionEntry *e, const SiftPartDesc *p)
{
	if ((p->header_blk == InvalidBlockNumber && !(p->ext_flags & SIFT_PART_EXT_REMOTE)) || p->part_gen == 0 || bound_cmp(p->key_lo, p->key_hi) > 0 || bound_cmp(p->key_lo, e->key_lo) < 0 ||
		(!bound_inf(e->key_hi) && bound_cmp(p->key_hi, e->key_hi) >= 0))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": part %llu does not lie inside partition %llu", RelationGetRelationName(rel),
																		 (unsigned long long) p->part_gen, (unsigned long long) e->part_id)));
}

typedef struct HeaderRef
{
	BlockNumber blk;
	uint16		flags;
} HeaderRef;

static int
header_ref_cmp(const void *a, const void *b)
{
	BlockNumber x = ((const HeaderRef *) a)->blk,
				y = ((const HeaderRef *) b)->blk;

	return x < y ? -1 : x > y;
}

/* A header block is one part's, unless the parts that share it are all slots of a packed page and each has a slot of its own */
static void
check_header_blocks(Relation rel, HeaderRef *refs, uint64 n)
{
	if (n > 1)
		qsort(refs, n, sizeof(HeaderRef), header_ref_cmp);
	for (uint64 i = 1; i < n; i++)
		if (refs[i].blk == refs[i - 1].blk)
		{
			uint64		j = i - 1;
			uint32		slots = 0;

			for (; j < n && refs[j].blk == refs[i].blk; j++)
			{
				uint32		bit = UINT32_C(1) << SIFT_PART_PACK_SLOT(refs[j].flags);

				if (!(refs[j].flags & SIFT_PART_FLAG_PACKED_SLOT) || (slots & bit))
					SIFT_CORRUPT(rel, "Block %u is the header of more than one part.", refs[i].blk);
				slots |= bit;
			}
			i = j;
		}
}

uint64
sift_manifest_check(Relation rel)
{
	SiftPartitionEntry *inl;
	int			ninl;
	SiftColMetaPageData meta = sift_col_meta_read_inline(rel, &inl, &ninl);
	SiftManifestRead all = {.entries = palloc(16 * sizeof(SiftPartitionEntry)),.version = meta.manifest_version};
	int			cap = 16;
	uint64		rows = 0, dead = 0, bytes = 0, nparts = 0;
	int			nscopes;
	int			ntier,
				nlive_refs = 0;
	uint64		nremote = 0;
	SiftTierPartRef *tiers;
	const SiftTierPartRef *tref;
	HeaderRef  *refs;
	uint64	   *gens;
	uint64		nrefs = 0,
				ngens = 0;

	sift_mf_collect_range(rel, meta.partdir_root_blk, meta.partdir_level, NULL, NULL, &all, &cap);
	for (int i = 0; i < all.n; i++)
		nparts += all.entries[i].nparts;
	/* check-memory: allow one reference to a header page for each part, which the entries count; one array is the check */
	refs = palloc_extended(Max(nparts, 1) * sizeof(HeaderRef), MCXT_ALLOC_HUGE);
	gens = palloc_extended(Max(nparts, 1) * sizeof(uint64), MCXT_ALLOC_HUGE);	/* check-memory: allow one generation for each part, as the references to the headers are */
	nparts = 0;
	if (inl != NULL && (ninl != all.n || memcmp(inl, all.entries, ninl * sizeof(SiftPartitionEntry)) != 0))
		SIFT_CORRUPT(rel, "The inline summary of the metapage is not the partition directory.");
	(void) sift_manifest_read_scopes(rel, &meta, &nscopes);
	tiers = sift_manifest_read_tier(rel, &meta, &ntier);
	for (int i = 0; i < ntier; i++)
		nlive_refs += !(tiers[i].flags & SIFT_TIER_DELETE_PENDING);
	if ((uint32) all.n != meta.npartitions)
		SIFT_CORRUPT(rel, "The directory lists %d partitions, the metapage counts %u.", all.n, meta.npartitions);
	for (int i = 0; i < all.n; i++)
	{
		const SiftPartitionEntry *e = &all.entries[i];
		SiftPartitionEntry fold = *e;
		int			n;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &n);
		SiftColDeltaAnchor anchor = sift_manifest_read_col_delta(rel, e);

		if ((uint32) n != e->nparts)
			SIFT_CORRUPT(rel, "Partition %llu counts %u parts, its list holds %d.", (unsigned long long) e->part_id, e->nparts, n);
		if (((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0) != sift_col_delta_anchor_has_chain(&anchor) || (sift_col_delta_anchor_has_chain(&anchor) && n == 0))
			SIFT_CORRUPT(rel, "Partition %llu and the anchor in its part list do not agree on a column delta chain.", (unsigned long long) e->part_id);
		if ((i == 0 && bound_cmp(e->key_lo, sift_mf_zero_bound) != 0) || (i + 1 < all.n && bound_cmp(e->key_hi, all.entries[i + 1].key_lo) != 0) ||
			(i + 1 == all.n && !bound_inf(e->key_hi)) || e->part_id == 0 || e->part_id >= meta.next_part_id)
			SIFT_CORRUPT(rel, "Partition %llu does not tile the key space.", (unsigned long long) e->part_id);
		sift_mf_fold_entry(&fold, parts, n);
		fold.l0_slab_blk = e->l0_slab_blk;
		sift_mf_refresh_hot_active(&fold);
		if (fold.flags != e->flags || fold.hot_parts != e->hot_parts || memcmp(fold.time_min, e->time_min, sizeof(fold.time_min)) != 0 ||
			memcmp(fold.time_max, e->time_max, sizeof(fold.time_max)) != 0)
			SIFT_CORRUPT(rel, "The time bounds of partition %llu are not those of its parts.", (unsigned long long) e->part_id);
		if (fold.total_rows != e->total_rows || fold.total_dead != e->total_dead || fold.total_bytes != e->total_bytes || fold.l0_count != e->l0_count ||
			fold.min_xid.value != e->min_xid.value || fold.max_xid.value != e->max_xid.value)
			SIFT_CORRUPT(rel, "The totals of partition %llu do not match its parts.", (unsigned long long) e->part_id);
		for (int j = 0; j < n; j++)
		{
			if (bound_cmp(parts[j].key_lo, e->key_lo) < 0 || (!bound_inf(e->key_hi) && bound_cmp(parts[j].key_hi, e->key_hi) >= 0))
				SIFT_CORRUPT(rel, "A part of partition %llu lies outside its range.", (unsigned long long) e->part_id);
			gens[ngens++] = parts[j].part_gen;
			if (!(parts[j].ext_flags & SIFT_PART_EXT_REMOTE))
				refs[nrefs++] = (HeaderRef) {parts[j].header_blk, parts[j].flags};
			else if (parts[j].header_blk != InvalidBlockNumber || (tref = sift_tier_refs_find(tiers, ntier, parts[j].part_gen)) == NULL || !(tref->flags & SIFT_TIER_REMOTE_ONLY) ||
					 (tref->flags & SIFT_TIER_DELETE_PENDING))
				SIFT_CORRUPT(rel, "Part %llu is in object storage and the tier directory has no live ref for it.", (unsigned long long) parts[j].part_gen);
			else
				nremote++;
		}
		rows += e->total_rows;
		dead += e->total_dead;
		bytes += e->total_bytes;
		nparts += n;
	}
	sift_proj_check(rel, &meta, gens, (int) ngens);
	check_header_blocks(rel, refs, nrefs);
	if (nremote != (uint64) nlive_refs)
		SIFT_CORRUPT(rel, "The tier directory holds %d live refs, the part lists name %llu parts in object storage.", nlive_refs, (unsigned long long) nremote);
	if (rows != meta.total_rows || dead != meta.total_dead || bytes != meta.total_bytes)
		SIFT_CORRUPT(rel, "The totals of the metapage (%llu rows, %llu dead, %llu bytes) are not the sums of the partitions (%llu, %llu, %llu).",
					 (unsigned long long) meta.total_rows, (unsigned long long) meta.total_dead, (unsigned long long) meta.total_bytes,
					 (unsigned long long) rows, (unsigned long long) dead, (unsigned long long) bytes);
	return nparts;
}
