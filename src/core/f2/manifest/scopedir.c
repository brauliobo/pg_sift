/*
 * core/manifest/scopedir.c - the scope directory of the manifest of format major 2: a chain of pages of refs in ascending scope_ord (core/manifest.h).
 */
#include "core/host.h"

#include "col/f2/col_meta.h"
#include "core/f2/manifest/manifest_int.h"
#include "core/page_io.h"

#define SDIR(p)		((SiftScopeDirPageData *) SiftPageGetData(p))

static SiftManifestScopeRef *
read_scope_chain(Relation rel, const SiftColMetaPageData *meta, int *nscopes, BlkVec *blocks, int *pages)
{
	SiftManifestScopeRef *out;
	int			n = 0;

	*nscopes = 0;
	if (meta->scope_dir_blk == InvalidBlockNumber)
	{
		if (meta->nscopes != 0)
			SIFT_CORRUPT(rel, "The metapage counts %u scopes and names no scope directory.", meta->nscopes);
		return NULL;
	}
	if (meta->nscopes == 0 || meta->nscopes > (uint32) SIFT_SCOPE_ORD_MAX + 1)
		SIFT_CORRUPT(rel, "The metapage counts %u scopes.", meta->nscopes);
	out = palloc(meta->nscopes * sizeof(SiftManifestScopeRef));
	for (BlockNumber blk = meta->scope_dir_blk; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_SCOPEDIR, SIFT_SCOPEDIR_MAGIC, meta->scope_dir_gen, 0, pages);
		SiftScopeDirPageData *d = SDIR(p);

		if (d->scope_dir_gen != meta->scope_dir_gen || d->nentries == 0 || d->nentries > SIFT_SCOPEDIR_CAP || n + d->nentries > meta->nscopes)
			SIFT_CORRUPT(rel, "Block %u: the scope directory page holds %u scopes, the metapage says %u in all.", blk, d->nentries, meta->nscopes);
		memcpy(out + n, d->entries, d->nentries * sizeof(SiftManifestScopeRef));
		n += d->nentries;
		if (blocks != NULL)
			sift_mf_blk_add(blocks, blk);
		blk = d->next_blk;
		pfree(p);
	}
	if (n != (int) meta->nscopes)
		SIFT_CORRUPT(rel, "The scope directory holds %d scopes, the metapage says %u.", n, meta->nscopes);
	for (int i = 1; i < n; i++)
		if (out[i - 1].scope_ord >= out[i].scope_ord)
			SIFT_CORRUPT(rel, "The scope directory is not in ascending scope order at scope %u.", out[i].scope_ord);
	*nscopes = n;
	return out;
}

SiftManifestScopeRef *
sift_manifest_read_scopes(Relation rel, const SiftColMetaPageData *meta, int *nscopes)
{
	return read_scope_chain(rel, meta, nscopes, NULL, NULL);
}

SiftManifestScopeRef *
sift_manifest_read_scope_dir(Relation rel, BlockNumber scope_dir_blk, uint32 nscopes, uint64 scope_dir_gen, int *nread)
{
	SiftColMetaPageData dir = {.scope_dir_blk = scope_dir_blk,.nscopes = nscopes,.scope_dir_gen = scope_dir_gen};

	return read_scope_chain(rel, &dir, nread, NULL, NULL);
}

static BlockNumber
write_scopedir(Relation rel, uint64 gen, uint64 version, const SiftManifestScopeRef *refs, int n, BlkVec *created)
{
	int			npages = (n + SIFT_SCOPEDIR_CAP - 1) / SIFT_SCOPEDIR_CAP;
	BlockNumber *blks = palloc(npages * sizeof(BlockNumber));
	PGAlignedBlock img;

	for (int p = 0; p < npages; p++)
		blks[p] = sift_mf_new_page_blk(rel, created);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * SIFT_SCOPEDIR_CAP,
					cnt = Min(n - from, (int) SIFT_SCOPEDIR_CAP);
		SiftScopeDirPageData *d;

		sift_page_init_part(img.data, SIFT_PT_SCOPEDIR, gen, 0);
		d = SDIR(img.data);
		d->magic = SIFT_SCOPEDIR_MAGIC;
		d->nentries = cnt;
		d->next_blk = p + 1 < npages ? blks[p + 1] : InvalidBlockNumber;
		d->manifest_version = version;
		d->scope_dir_gen = gen;
		memcpy(d->entries, refs + from, cnt * sizeof(SiftManifestScopeRef));
		SiftPageSetUsed(img.data, offsetof(SiftScopeDirPageData, entries) + cnt * sizeof(SiftManifestScopeRef));
		sift_page_seal(img.data);
		sift_mf_write_image(rel, blks[p], img.data);
	}
	return blks[0];
}

static int
scope_ref_cmp(const void *a, const void *b)
{
	uint16		x = ((const SiftManifestScopeRef *) a)->scope_ord,
				y = ((const SiftManifestScopeRef *) b)->scope_ord;

	return x < y ? -1 : x > y;
}

/* The directory the metapage names with the refs of the edit merged in by scope_ord (a later ref of the same ord wins), written as a new chain; the pages it replaces are released with the commit */
void
sift_mf_build_scopes(Relation rel, Tree *t, Change *ch, const SiftManifestEdit *e, BlkVec *created)
{
	int			n;
	SiftManifestScopeRef *cur = read_scope_chain(rel, &t->meta, &n, &ch->old, &t->pages);
	SiftManifestScopeRef *all = palloc((n + e->nupdate_scopes) * sizeof(SiftManifestScopeRef));
	int			nall = n;
	uint64		new_version = t->meta.manifest_version + 1;

	if (n > 0)
		memcpy(all, cur, n * sizeof(SiftManifestScopeRef));
	for (int i = 0; i < e->nupdate_scopes; i++)
	{
		SiftManifestScopeRef *at;

		if (e->update_scopes[i].scope_ord > SIFT_SCOPE_ORD_MAX)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": scope ordinal %d is out of range", RelationGetRelationName(rel), e->update_scopes[i].scope_ord)));
		at = nall > 0 ? bsearch(&e->update_scopes[i], all, nall, sizeof(SiftManifestScopeRef), scope_ref_cmp) : NULL;
		if (at != NULL)
			*at = e->update_scopes[i];
		else
		{
			all[nall++] = e->update_scopes[i];
			qsort(all, nall, sizeof(SiftManifestScopeRef), scope_ref_cmp);
		}
	}
	ch->set_scopes = true;
	ch->nscopes = nall;
	ch->scope_dir_gen = sift_manifest_new_gen(rel, 1);
	ch->scope_dir_blk = write_scopedir(rel, ch->scope_dir_gen, new_version, all, nall, created);
}
