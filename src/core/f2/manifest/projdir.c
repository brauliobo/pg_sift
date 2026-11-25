/*
 * core/f2/manifest/projdir.c - the projection directory of the manifest of format major 2: a chain of SIFT_PT_PROJDIR pages of refs in ascending proj_id, and for each projection its own
 * directory of parts (core/f2/proj.h). Every piece is written afresh by the commit that changes it and the pages it replaces are released with that commit, as the scope directory is.
 */
#include "core/host.h"

#include "col/f2/col_meta.h"
#include "core/f2/manifest/manifest_int.h"
#include "core/f2/proj.h"
#include "core/page_io.h"

#define PDIRP(p)	((SiftProjDirPageData *) SiftPageGetData(p))
#define PNODE(p)	((SiftPartDirPageData *) SiftPageGetData(p))

static void
check_chain_header(Relation rel, BlockNumber blk, const SiftProjDirPageData *d, uint64 gen, uint16 flags, size_t cap, uint32 room)
{
	if (d->proj_dir_gen != gen || d->flags != flags || d->nentries == 0 || d->nentries > cap || d->nentries > room)
		SIFT_CORRUPT(rel, "Block %u: the projection directory page holds %u entries (flags %u, generation %llu), %u are left to read.", blk, d->nentries, d->flags,
					 (unsigned long long) d->proj_dir_gen, room);
}

/* The entries of a chain of nentries in all (refs when proj_id is 0, else the links of that projection), appended to blocks */
static char *
read_chain(Relation rel, BlockNumber head, uint32 nentries, uint64 gen, uint32 proj_id, size_t esize, size_t cap, BlkVec *blocks, int *pages)
{
	char	   *out = palloc(Max(nentries, 1) * esize);
	uint32		n = 0;
	uint16		flags = proj_id == 0 ? 0 : SIFT_PROJDIR_FLAG_LINKS;

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_PROJDIR, SIFT_PROJDIR_MAGIC, gen, proj_id, pages);
		SiftProjDirPageData *d = PDIRP(p);

		check_chain_header(rel, blk, d, gen, flags, cap, nentries - n);
		memcpy(out + n * esize, d->entries, d->nentries * esize);
		n += d->nentries;
		if (blocks != NULL)
			sift_mf_blk_add(blocks, blk);
		blk = d->next_blk;
	}
	if (n != nentries)
		SIFT_CORRUPT(rel, "The projection directory chain holds %u entries, the anchor says %u.", n, nentries);
	return out;
}

static SiftManifestProjRef *
read_ref_chain(Relation rel, const SiftColMetaPageData *meta, int *nprojs, BlkVec *blocks, int *pages)
{
	SiftManifestProjRef *out;

	*nprojs = 0;
	if (meta->proj_dir_blk == InvalidBlockNumber)
	{
		if (meta->nprojs != 0)
			SIFT_CORRUPT(rel, "The metapage counts %u projections and names no projection directory.", meta->nprojs);
		if (meta->common.ro_compat_flags & SIFT_ROCOMPAT_PROJECTIONS)
			SIFT_CORRUPT(rel, "The metapage says the relation has projections and names no projection directory.");
		return NULL;
	}
	if (meta->nprojs == 0 || meta->nprojs > SIFT_PROJ_ID_MAX || !(meta->common.ro_compat_flags & SIFT_ROCOMPAT_PROJECTIONS))
		SIFT_CORRUPT(rel, "The metapage counts %u projections (feature bit %s).", meta->nprojs, (meta->common.ro_compat_flags & SIFT_ROCOMPAT_PROJECTIONS) ? "set" : "clear");
	out = (SiftManifestProjRef *) read_chain(rel, meta->proj_dir_blk, meta->nprojs, meta->proj_dir_gen, 0, sizeof(SiftManifestProjRef), SIFT_PROJDIR_REF_CAP, blocks, pages);
	for (int i = 0; i < (int) meta->nprojs; i++)
	{
		const SiftManifestProjRef *r = &out[i];

		if (r->proj_id == 0 || r->proj_id > SIFT_PROJ_ID_MAX || (i > 0 && out[i - 1].proj_id >= r->proj_id) || (r->kind != SIFT_PROJ_SORT && r->kind != SIFT_PROJ_AGG) ||
			r->state > SIFT_PROJ_STALE || memchr(r->name, '\0', SIFT_PROJ_NAME_LEN) == NULL || r->name[0] == '\0' || r->partdir_root_blk == InvalidBlockNumber ||
			r->npartitions != 1 || (r->link_blk == InvalidBlockNumber) != (r->nlinks == 0) || r->nlinks != r->nparts)
			SIFT_CORRUPT(rel, "The projection directory entry %d (id %u) is damaged.", i, r->proj_id);
	}
	*nprojs = (int) meta->nprojs;
	return out;
}

SiftManifestProjRef *
sift_manifest_read_projs(Relation rel, const SiftColMetaPageData *meta, int *nprojs)
{
	return read_ref_chain(rel, meta, nprojs, NULL, NULL);
}

const SiftManifestProjRef *
sift_proj_find(const SiftManifestProjRef *refs, int nrefs, const char *name)
{
	for (int i = 0; i < nrefs; i++)
		if (strncmp(refs[i].name, name, SIFT_PROJ_NAME_LEN) == 0)
			return &refs[i];
	return NULL;
}

/* The leaf of a projection's own directory, and the part list it names: the blocks they occupy go to blocks */
static int part_cmp(const void *a, const void *b);

static SiftPartDesc *
read_part_list(Relation rel, const SiftManifestProjRef *ref, BlkVec *blocks, int *pages)
{
	Page		leaf = sift_mf_read_page(rel, ref->partdir_root_blk, SIFT_PT_PARTDIR, SIFT_PARTDIR_MAGIC, ref->manifest_gen, 0, pages);
	const SiftPartitionEntry *e = (const SiftPartitionEntry *) (SiftPageGetData(leaf) + sizeof(SiftPartDirPageData));
	SiftPartDesc *out = palloc(Max(ref->nparts, 1) * sizeof(SiftPartDesc));
	uint32		n = 0;

	if (ref->partdir_level != 0 || PNODE(leaf)->level != 0 || PNODE(leaf)->nentries != 1 || e->part_id != 1 || e->nparts != ref->nparts || e->manifest_gen != ref->manifest_gen)
		SIFT_CORRUPT(rel, "Block %u: the directory of projection \"%s\" is damaged.", ref->partdir_root_blk, ref->name);
	if (blocks != NULL)
		sift_mf_blk_add(blocks, ref->partdir_root_blk);
	for (BlockNumber blk = e->manifest_blk; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, ref->manifest_gen, 1, pages);
		SiftPartListPageData *l = PLIST(p);

		if (l->part_id != 1 || l->flags != 0 || l->manifest_gen != ref->manifest_gen || l->nparts == 0 || l->nparts > SIFT_PARTLIST_CAP || n + l->nparts > ref->nparts)
			SIFT_CORRUPT(rel, "Block %u: the part list of projection \"%s\" holds %u parts, the directory says %u in all.", blk, ref->name, l->nparts, ref->nparts);
		memcpy(out + n, l->parts, l->nparts * sizeof(SiftPartDesc));
		n += l->nparts;
		if (blocks != NULL)
			sift_mf_blk_add(blocks, blk);
		blk = l->next_blk;
	}
	if (n != ref->nparts)
		SIFT_CORRUPT(rel, "The part list of projection \"%s\" holds %u parts, the directory says %u.", ref->name, n, ref->nparts);
	for (uint32 i = 1; i < n; i++)
		if (out[i - 1].part_gen >= out[i].part_gen)
			SIFT_CORRUPT(rel, "The part list of projection \"%s\" is not in ascending generation at part %llu.", ref->name, (unsigned long long) out[i].part_gen);
	return out;
}

static SiftProjParts
read_proj_parts(Relation rel, const SiftManifestProjRef *ref, BlkVec *blocks, int *pages)
{
	SiftProjParts r = {0};

	r.nparts = (int) ref->nparts;
	r.parts = read_part_list(rel, ref, blocks, pages);
	r.nlinks = (int) ref->nlinks;
	if (ref->nlinks > 0)
		r.links = (SiftProjLink *) read_chain(rel, ref->link_blk, ref->nlinks, ref->link_gen, ref->proj_id, sizeof(SiftProjLink), SIFT_PROJDIR_LINK_CAP, blocks, pages);
	for (int i = 1; i < r.nlinks; i++)
		if (r.links[i - 1].src_gen >= r.links[i].src_gen)
			SIFT_CORRUPT(rel, "The links of projection \"%s\" are not in ascending base generation at part %llu.", ref->name, (unsigned long long) r.links[i].src_gen);
	for (int i = 0; i < r.nlinks; i++)
	{
		SiftPartDesc key = {.part_gen = r.links[i].proj_gen};

		if (bsearch(&key, r.parts, r.nparts, sizeof(SiftPartDesc), part_cmp) == NULL)
			SIFT_CORRUPT(rel, "A link of projection \"%s\" names the part %llu it does not have.", ref->name, (unsigned long long) r.links[i].proj_gen);
	}
	return r;
}

SiftProjParts
sift_manifest_read_proj_parts(Relation rel, const SiftManifestProjRef *ref)
{
	return read_proj_parts(rel, ref, NULL, NULL);
}

BlockNumber *
sift_proj_dir_blocks(Relation rel, const SiftColMetaPageData *meta, int *n)
{
	BlkVec		blocks = {0};
	int			nrefs;
	SiftManifestProjRef *refs = read_ref_chain(rel, meta, &nrefs, &blocks, NULL);

	for (int i = 0; i < nrefs; i++)
		(void) read_proj_parts(rel, &refs[i], &blocks, NULL);
	*n = blocks.n;
	return blocks.v;
}

BlockNumber *
sift_proj_ref_dir_blocks(Relation rel, const SiftColMetaPageData *meta, int *n)
{
	BlkVec		blocks = {0};
	int			nrefs;

	(void) read_ref_chain(rel, meta, &nrefs, &blocks, NULL);
	*n = blocks.n;
	return blocks.v;
}

BlockNumber *
sift_proj_one_dir_blocks(Relation rel, const SiftManifestProjRef *ref, int *n)
{
	BlkVec		blocks = {0};

	(void) read_proj_parts(rel, ref, &blocks, NULL);
	*n = blocks.n;
	return blocks.v;
}

static BlockNumber
write_chain(Relation rel, uint64 gen, uint64 version, uint32 proj_id, const char *entries, size_t esize, size_t cap, int n, BlkVec *created)
{
	int			npages = (n + (int) cap - 1) / (int) cap;
	BlockNumber *blks;
	PGAlignedBlock img;
	BlockNumber head;
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	if (n == 0)
		return InvalidBlockNumber;
	blks = palloc(npages * sizeof(BlockNumber));
	for (int p = 0; p < npages; p++)
		blks[p] = sift_mf_new_page_blk(rel, created);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * (int) cap,
					cnt = Min(n - from, (int) cap);
		SiftProjDirPageData *d;

		sift_page_init_part(img.data, SIFT_PT_PROJDIR, gen, proj_id);
		d = PDIRP(img.data);
		d->magic = SIFT_PROJDIR_MAGIC;
		d->nentries = cnt;
		d->flags = proj_id == 0 ? 0 : SIFT_PROJDIR_FLAG_LINKS;
		d->next_blk = p + 1 < npages ? blks[p + 1] : InvalidBlockNumber;
		d->manifest_version = version;
		d->proj_dir_gen = gen;
		memcpy(d->entries, entries + (size_t) from * esize, cnt * esize);
		SiftPageSetUsed(img.data, offsetof(SiftProjDirPageData, entries) + cnt * esize);
		sift_page_seal(img.data);
		sift_mf_log_image(&log, blks[p], img.data);
	}
	sift_newlog_end(&log);
	head = blks[0];
	pfree(blks);
	return head;
}

static BlockNumber
write_part_list(Relation rel, uint64 gen, const SiftPartDesc *parts, int n, BlkVec *created)
{
	int			npages = (n + (int) SIFT_PARTLIST_CAP - 1) / (int) SIFT_PARTLIST_CAP;
	BlockNumber *blks;
	PGAlignedBlock img;
	BlockNumber head;
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	if (n == 0)
		return InvalidBlockNumber;
	blks = palloc(npages * sizeof(BlockNumber));
	for (int p = 0; p < npages; p++)
		blks[p] = sift_mf_new_page_blk(rel, created);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * (int) SIFT_PARTLIST_CAP,
					cnt = Min(n - from, (int) SIFT_PARTLIST_CAP);
		SiftPartListPageData *l;

		sift_page_init_part(img.data, SIFT_PT_PARTLIST, gen, 1);
		l = PLIST(img.data);
		l->magic = SIFT_PARTLIST_MAGIC;
		l->nparts = cnt;
		l->next_blk = p + 1 < npages ? blks[p + 1] : InvalidBlockNumber;
		l->part_id = 1;
		l->manifest_gen = gen;
		memcpy(l->parts, parts + from, cnt * sizeof(SiftPartDesc));
		SiftPageSetUsed(img.data, offsetof(SiftPartListPageData, parts) + cnt * sizeof(SiftPartDesc));
		sift_page_seal(img.data);
		sift_mf_log_image(&log, blks[p], img.data);
	}
	sift_newlog_end(&log);
	head = blks[0];
	pfree(blks);
	return head;
}

static uint64
sum_rows(const SiftPartDesc *parts, int n, bool bytes)
{
	uint64		s = 0;

	for (int i = 0; i < n; i++)
		s += bytes ? parts[i].nbytes : parts[i].nrows - parts[i].ndead;
	return s;
}

void
sift_proj_write_parts(Relation rel, SiftManifestProjRef *ref, uint64 gen, uint64 version, const SiftProjParts *p, BlkVec *created)
{
	PGAlignedBlock img;
	SiftPartitionEntry *e;

	sift_page_init_part(img.data, SIFT_PT_PARTDIR, gen, 0);
	PNODE(img.data)->magic = SIFT_PARTDIR_MAGIC;
	PNODE(img.data)->level = 0;
	PNODE(img.data)->nentries = 1;
	PNODE(img.data)->manifest_version = version;
	e = (SiftPartitionEntry *) (SiftPageGetData(img.data) + sizeof(SiftPartDirPageData));
	memset(e, 0, sizeof(SiftPartitionEntry));
	e->part_id = 1;
	e->manifest_blk = write_part_list(rel, gen, p->parts, p->nparts, created);
	e->nparts = p->nparts;
	e->manifest_gen = gen;
	e->l0_slab_blk = e->pend_del_blk = InvalidBlockNumber;
	e->total_rows = sum_rows(p->parts, p->nparts, false);
	e->total_bytes = sum_rows(p->parts, p->nparts, true);
	SiftPageSetUsed(img.data, sizeof(SiftPartDirPageData) + sizeof(SiftPartitionEntry));
	sift_page_seal(img.data);
	ref->partdir_root_blk = sift_mf_new_page_blk(rel, created);
	sift_mf_write_image(rel, ref->partdir_root_blk, img.data);
	ref->partdir_level = 0;
	ref->npartitions = 1;
	ref->nparts = p->nparts;
	ref->manifest_gen = gen;
	ref->synced_version = version;
	ref->total_rows = e->total_rows;
	ref->total_bytes = e->total_bytes;
	ref->link_gen = gen;
	ref->nlinks = p->nlinks;
	ref->link_blk = write_chain(rel, gen, version, ref->proj_id, (const char *) p->links, sizeof(SiftProjLink), SIFT_PROJDIR_LINK_CAP, p->nlinks, created);
}

static int
ref_cmp(const void *a, const void *b)
{
	uint16		x = ((const SiftManifestProjRef *) a)->proj_id,
				y = ((const SiftManifestProjRef *) b)->proj_id;

	return x < y ? -1 : x > y;
}

static int
part_cmp(const void *a, const void *b)
{
	uint64		x = ((const SiftPartDesc *) a)->part_gen,
				y = ((const SiftPartDesc *) b)->part_gen;

	return x < y ? -1 : x > y;
}

static int
link_cmp(const void *a, const void *b)
{
	uint64		x = ((const SiftProjLink *) a)->src_gen,
				y = ((const SiftProjLink *) b)->src_gen;

	return x < y ? -1 : x > y;
}

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* A sorted copy of gens: the membership tests of one call are binary searches */
static uint64 *
sorted_gens(const uint64 *v, int n)
{
	/* check-memory: allow the generations of one projection delta, which a commit bounds */
	uint64	   *s = palloc(Max(n, 1) * sizeof(uint64));

	if (n > 0)
		memcpy(s, v, n * sizeof(uint64));
	qsort(s, n, sizeof(uint64), gen_cmp);
	return s;
}

static bool
sorted_has(const uint64 *v, int n, uint64 g)
{
	return bsearch(&g, v, n, sizeof(uint64), gen_cmp) != NULL;
}

/* The links and parts of cur are in ascending order (read_proj_parts checks it, and every apply keeps it) */
static const SiftProjLink *
find_link(const SiftProjParts *cur, uint64 src_gen)
{
	SiftProjLink key = {.src_gen = src_gen};

	return bsearch(&key, cur->links, cur->nlinks, sizeof(SiftProjLink), link_cmp);
}

bool
sift_proj_delta_applies(const SiftProjParts *cur, const SiftProjDelta *d)
{
	uint64	   *drops = sorted_gens(d->drop_proj_gens, d->ndrop_parts);
	bool		ok = true;

	for (int i = 0; i < d->ndrop_parts && ok; i++)
	{
		SiftPartDesc key = {.part_gen = drops[i]};

		ok = bsearch(&key, cur->parts, cur->nparts, sizeof(SiftPartDesc), part_cmp) != NULL;
	}
	for (int i = 0; i < d->nadd_links && ok; i++)
	{
		const SiftProjLink *l = find_link(cur, d->add_links[i].src_gen);

		ok = l != NULL && sorted_has(drops, d->ndrop_parts, l->proj_gen);
	}
	pfree(drops);
	return ok;
}

/* The parts of cur that are not in drops (both ascending) in out, and how many drops met a part */
static int
keep_parts(const SiftProjParts *cur, const uint64 *drops, int ndrops, SiftPartDesc *out, int *nout)
{
	int			di = 0,
				dropped = 0;

	*nout = 0;
	for (int i = 0; i < cur->nparts; i++)
	{
		while (di < ndrops && drops[di] < cur->parts[i].part_gen)
			di++;
		if (di < ndrops && drops[di] == cur->parts[i].part_gen)
		{
			dropped++;
			di++;
		}
		else
			out[(*nout)++] = cur->parts[i];
	}
	return dropped;
}

static void
keep_links(const SiftProjParts *cur, const SiftProjLink *adds, int nadds, const uint64 *drop_src, int ndrop_src, SiftProjLink *out, int *nout)
{
	int			ai = 0,
				di = 0;

	*nout = 0;
	for (int i = 0; i < cur->nlinks; i++)
	{
		uint64		src = cur->links[i].src_gen;

		while (ai < nadds && adds[ai].src_gen < src)
			ai++;
		while (di < ndrop_src && drop_src[di] < src)
			di++;
		if ((ai < nadds && adds[ai].src_gen == src) || (di < ndrop_src && drop_src[di] == src))
			continue;
		out[(*nout)++] = cur->links[i];
	}
}

void
sift_proj_apply_delta(const SiftProjParts *cur, const SiftProjDelta *d, SiftProjParts *out)
{
	SiftPartDesc *kept = palloc((cur->nparts + 1) * sizeof(SiftPartDesc)),
			   *adds = palloc((d->nadd_parts + 1) * sizeof(SiftPartDesc)),
			   *parts = palloc((cur->nparts + d->nadd_parts + 1) * sizeof(SiftPartDesc));
	SiftProjLink *kept_links = palloc((cur->nlinks + 1) * sizeof(SiftProjLink)),
			   *add_links = palloc((d->nadd_links + 1) * sizeof(SiftProjLink)),
			   *links = palloc((cur->nlinks + d->nadd_links + 1) * sizeof(SiftProjLink));
	uint64	   *drops = sorted_gens(d->drop_proj_gens, d->ndrop_parts),
			   *drop_src = sorted_gens(d->drop_src_gens, d->ndrop_links);
	int			np = 0,
				nl = 0,
				nk,
				nkl,
				dropped_parts;
	bool	   *used;

	dropped_parts = keep_parts(cur, drops, d->ndrop_parts, kept, &nk);
	if (dropped_parts != d->ndrop_parts)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("projection %u does not have %d of the parts to drop", d->proj_id, d->ndrop_parts - dropped_parts)));
	if (d->nadd_parts > 0)
		memcpy(adds, d->add_parts, d->nadd_parts * sizeof(SiftPartDesc));
	qsort(adds, d->nadd_parts, sizeof(SiftPartDesc), part_cmp);
	for (int k = 0, a = 0; k < nk || a < d->nadd_parts;)
	{
		bool		from_kept = a == d->nadd_parts || (k < nk && kept[k].part_gen < adds[a].part_gen);

		parts[np++] = from_kept ? kept[k++] : adds[a++];
		if (np > 1 && parts[np - 2].part_gen == parts[np - 1].part_gen)
			ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("projection %u has the part %llu twice", d->proj_id, (unsigned long long) parts[np - 1].part_gen)));
	}
	if (d->nadd_links > 0)
		memcpy(add_links, d->add_links, d->nadd_links * sizeof(SiftProjLink));
	qsort(add_links, d->nadd_links, sizeof(SiftProjLink), link_cmp);
	keep_links(cur, add_links, d->nadd_links, drop_src, d->ndrop_links, kept_links, &nkl);
	for (int k = 0, a = 0; k < nkl || a < d->nadd_links;)
	{
		bool		from_kept = a == d->nadd_links || (k < nkl && kept_links[k].src_gen < add_links[a].src_gen);

		links[nl++] = from_kept ? kept_links[k++] : add_links[a++];
		if (nl > 1 && links[nl - 2].src_gen == links[nl - 1].src_gen)
			ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("projection %u has two links of the base part %llu", d->proj_id, (unsigned long long) links[nl - 1].src_gen)));
	}
	used = palloc0((np + 1) * sizeof(bool));
	for (int i = 0; i < nl; i++)
	{
		SiftPartDesc key = {.part_gen = links[i].proj_gen};
		SiftPartDesc *at = bsearch(&key, parts, np, sizeof(SiftPartDesc), part_cmp);

		if (at == NULL || used[at - parts])
			ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("projection %u: the link of base part %llu names part %llu, which it does not have once", d->proj_id,
																   (unsigned long long) links[i].src_gen, (unsigned long long) links[i].proj_gen)));
		used[at - parts] = true;
	}
	if (nl != np)
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("projection %u would have %d parts and %d links", d->proj_id, np, nl)));
	pfree(used);
	pfree(kept);
	pfree(adds);
	pfree(kept_links);
	pfree(add_links);
	pfree(drops);
	pfree(drop_src);
	*out = (SiftProjParts) {.parts = parts,.nparts = np,.links = links,.nlinks = nl};
}

static const SiftProjDelta *
find_delta(const SiftManifestProjUpdate *u, uint16 id)
{
	for (int i = 0; i < u->ndeltas; i++)
		if (u->deltas[i].proj_id == id)
			return &u->deltas[i];
	return NULL;
}

static bool
has_ref(const SiftManifestProjRef *refs, int n, uint16 id)
{
	for (int i = 0; i < n; i++)
		if (refs[i].proj_id == id)
			return true;
	return false;
}

static bool
dropped(const SiftManifestProjUpdate *u, uint16 id)
{
	for (int i = 0; i < u->ndrop; i++)
		if (u->drop_ids[i] == id)
			return true;
	return false;
}

static void
release_add(Change *ch, const SiftExtent *ext, int n)
{
	if (ch->nproj_release + n > ch->proj_release_cap)
	{
		ch->proj_release_cap = Max(Max(16, ch->proj_release_cap * 2), ch->nproj_release + n);
		/* check-memory: allow the extents of the projection parts one commit releases, which the parts it drops bound */
		ch->proj_release = ch->proj_release == NULL ? palloc(ch->proj_release_cap * sizeof(SiftExtent)) : repalloc(ch->proj_release, ch->proj_release_cap * sizeof(SiftExtent));
	}
	memcpy(ch->proj_release + ch->nproj_release, ext, n * sizeof(SiftExtent));
	ch->nproj_release += n;
}

static void
release_parts(Relation rel, Change *ch, const SiftProjParts *cur, const uint64 *gens, int ngens, bool all)
{
	uint64	   *sorted = all ? NULL : sorted_gens(gens, ngens);

	for (int i = 0; i < cur->nparts; i++)
	{
		int			n;
		SiftExtent *ext;

		if (!all && !sorted_has(sorted, ngens, cur->parts[i].part_gen))
			continue;
		ext = sift_proj_part_extents(rel, &cur->parts[i], &n);
		release_add(ch, ext, n);
		pfree(ext);
	}
	if (sorted != NULL)
		pfree(sorted);
}

static SiftManifestProjRef
write_applied(Relation rel, SiftManifestProjRef ref, const SiftProjParts *cur, const SiftProjDelta *d, uint64 gen, uint64 version, BlkVec *created)
{
	SiftProjParts next;

	sift_proj_apply_delta(cur, d, &next);
	if (d->set_state)
		ref.state = d->state;
	sift_proj_write_parts(rel, &ref, gen, version, &next, created);
	pfree(next.parts);
	pfree(next.links);
	return ref;
}

/*
 * The directory the metapage names with the update applied to the lists it holds: a projection with a delta gets its directory of parts and links written afresh, one without keeps its own,
 * a dropped one leaves; the refs are written as a new chain. The pages of the chain it replaces and of every directory of a projection it rewrites or drops are released with the commit.
 */
static void
sift_mf_build_projs(Relation rel, Tree *t, Change *ch, const SiftManifestProjUpdate *u, BlkVec *created)
{
	int			n;
	SiftManifestProjRef *cur = read_ref_chain(rel, &t->meta, &n, &ch->old, &t->pages);
	SiftManifestProjRef *all = palloc((n + u->ndeltas + 1) * sizeof(SiftManifestProjRef));
	int			nall = 0,
				ncreated = 0,
				changed = 0,
				skipped = 0;
	uint64		gen = sift_manifest_new_gen(rel, 1),
				version = t->meta.manifest_version + 1;
	SiftProjParts none = {0};

	for (int i = 0; i < n; i++)
	{
		const SiftProjDelta *d = find_delta(u, cur[i].proj_id);
		SiftProjParts parts;

		if (dropped(u, cur[i].proj_id) && d != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": projection %d is dropped and changed in one commit", RelationGetRelationName(rel), cur[i].proj_id)));
		if (d == NULL && !dropped(u, cur[i].proj_id))
		{
			all[nall++] = cur[i];
			continue;
		}
		parts = read_proj_parts(rel, &cur[i], &ch->old, &t->pages);
		if (d != NULL && d->move_if_present)
		{
			*d->move_applied = sift_proj_delta_applies(&parts, d);
			if (!*d->move_applied)
			{
				all[nall++] = cur[i];
				skipped++;
				continue;
			}
		}
		release_parts(rel, ch, &parts, d != NULL ? d->drop_proj_gens : NULL, d != NULL ? d->ndrop_parts : 0, d == NULL);
		if (d != NULL)
		{
			if (d->create)
				ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("sift index \"%s\": projection %d exists already", RelationGetRelationName(rel), d->proj_id)));
			all[nall++] = write_applied(rel, cur[i], &parts, d, gen, version, created);
			changed++;
		}
	}
	for (int j = 0; j < u->ndeltas; j++)
	{
		SiftManifestProjRef ref;

		if (!u->deltas[j].create)
			continue;
		if (u->deltas[j].proj_id == 0 || u->deltas[j].proj_id > SIFT_PROJ_ID_MAX)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": projection id %d is out of range", RelationGetRelationName(rel), u->deltas[j].proj_id)));
		ref = u->deltas[j].ref;
		ref.proj_id = u->deltas[j].proj_id;
		all[nall++] = write_applied(rel, ref, &none, &u->deltas[j], gen, version, created);
		ncreated++;
	}
	for (int j = 0; j < u->ndeltas; j++)
		if (u->deltas[j].move_if_present && !has_ref(cur, n, u->deltas[j].proj_id))
		{
			*u->deltas[j].move_applied = false;
			skipped++;
		}
	if (changed + ncreated + skipped != u->ndeltas)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("sift index \"%s\": a projection to change does not exist", RelationGetRelationName(rel))));
	qsort(all, nall, sizeof(SiftManifestProjRef), ref_cmp);
	for (int i = 1; i < nall; i++)
		if (all[i - 1].proj_id == all[i].proj_id)
			ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("sift index \"%s\": projection id %d is in the directory twice", RelationGetRelationName(rel), all[i].proj_id)));
	ch->set_projs = true;
	ch->nprojs = nall;
	ch->proj_dir_gen = gen;
	ch->proj_dir_blk = write_chain(rel, gen, version, 0, (const char *) all, sizeof(SiftManifestProjRef), SIFT_PROJDIR_REF_CAP, nall, created);
}

static SiftProjDelta *
delta_slot(SiftProjDelta *deltas, int *n, uint16 id)
{
	for (int i = 0; i < *n; i++)
		if (deltas[i].proj_id == id)
			return &deltas[i];
	deltas[*n] = (SiftProjDelta) {.proj_id = id};
	return &deltas[(*n)++];
}

static void
append_gens(const uint64 **dst, int *ndst, const uint64 *src, int nsrc)
{
	uint64	   *v = palloc(Max(*ndst + nsrc, 1) * sizeof(uint64));

	if (*ndst > 0)
		memcpy(v, *dst, *ndst * sizeof(uint64));
	if (nsrc > 0)
		memcpy(v + *ndst, src, nsrc * sizeof(uint64));
	*dst = v;
	*ndst += nsrc;
}

/*
 * The projection directory of a commit of the manifest: the update the caller names, and for every projection the links and parts of the base parts the commit drops, which leave with them
 * whoever the caller is (a flush, a compaction, a vacuum, a delete that rewrites a part). A relation without projections that is given no update is not touched.
 */
void
sift_mf_build_commit_projs(Relation rel, Tree *t, Change *ch, const SiftManifestProjUpdate *u, const uint64 *drop_gens, int ndrop, BlkVec *created)
{
	bool		has = (t->meta.common.ro_compat_flags & SIFT_ROCOMPAT_PROJECTIONS) != 0;
	SiftManifestProjUpdate all = {0};
	SiftProjDelta *deltas;
	int			n = 0,
				nrefs;
	SiftManifestProjRef *refs = NULL;

	if (u == NULL && !(has && ndrop > 0))
		return;
	deltas = palloc(((u != NULL ? u->ndeltas : 0) + (has ? (int) t->meta.nprojs : 0) + 1) * sizeof(SiftProjDelta));
	if (u != NULL)
	{
		if (u->ndeltas > 0)
			memcpy(deltas, u->deltas, u->ndeltas * sizeof(SiftProjDelta));
		n = u->ndeltas;
		all.drop_ids = u->drop_ids;
		all.ndrop = u->ndrop;
	}
	if (has && ndrop > 0)
		refs = read_ref_chain(rel, &t->meta, &nrefs, NULL, NULL);
	for (int i = 0; refs != NULL && i < nrefs; i++)
	{
		SiftProjParts parts = read_proj_parts(rel, &refs[i], NULL, NULL);
		SiftProjDelta derived;

		if (dropped(&all, refs[i].proj_id))
			continue;
		sift_proj_delta_for_dropped_parts(&parts, drop_gens, ndrop, &derived);
		if (derived.ndrop_parts > 0)
		{
			SiftProjDelta *d = delta_slot(deltas, &n, refs[i].proj_id);

			append_gens(&d->drop_proj_gens, &d->ndrop_parts, derived.drop_proj_gens, derived.ndrop_parts);
			append_gens(&d->drop_src_gens, &d->ndrop_links, derived.drop_src_gens, derived.ndrop_links);
		}
	}
	all.deltas = deltas;
	all.ndeltas = n;
	sift_mf_build_projs(rel, t, ch, &all, created);
}

/* The whole directory read and checked: the pages, the order of the refs and links, a part for each link, and a base part for each link (the commit that dropped the base part dropped its link) */
void
sift_proj_check(Relation rel, const SiftColMetaPageData *meta, uint64 *base_gens, int nbase)
{
	int			nrefs;
	SiftManifestProjRef *refs = read_ref_chain(rel, meta, &nrefs, NULL, NULL);

	qsort(base_gens, nbase, sizeof(uint64), gen_cmp);
	for (int i = 0; i < nrefs; i++)
	{
		SiftProjParts p = read_proj_parts(rel, &refs[i], NULL, NULL);

		for (int j = 0; j < p.nlinks; j++)
			if (bsearch(&p.links[j].src_gen, base_gens, nbase, sizeof(uint64), gen_cmp) == NULL)
				SIFT_CORRUPT(rel, "Projection \"%s\" holds a link to base part %llu, which the manifest does not have.", refs[i].name, (unsigned long long) p.links[j].src_gen);
	}
}
