/*
 * core/manifest.c - the two-level copy on write manifest of format major 2 (core/manifest.h).
 *
 * A change is built as pages first: the new list of the partition, the leaf or leaves that hold its entry, and the interior nodes above them whose children moved, all from the
 * metadata slabs and each logged as a full image (P1). Then block 0 is taken exclusively, the version read at the start is compared with the one there, and a single record switches
 * the root, bumps the version and queues the pages and extents the change replaces (P2, P4). A change that lost the race to another partition's commit frees its pages again and
 * is built anew from the new root: nothing it wrote was reachable.
 */
#include "core/host.h"

#include "access/xact.h"
#include "storage/lmgr.h"
#include "storage/proc.h"

#include "col/f2/col_meta.h"
#include "col/f2/part_format.h"
#include "core/f2/f2_gate.h"
#include "core/f2/manifest.h"
#include "core/f2/manifest/manifest_int.h"
#include "core/page_io.h"

void
sift_mf_blk_add(BlkVec *b, BlockNumber blk)
{
	if (b->n == b->max)
	{
		b->max = Max(16, b->max * 2);
		b->v = b->v ? repalloc(b->v, b->max * sizeof(BlockNumber)) : palloc(b->max * sizeof(BlockNumber));
	}
	b->v[b->n++] = blk;
}

static void
dir_init(Page page, uint16 level, uint64 version)
{
	sift_page_init_part(page, SIFT_PT_PARTDIR, version, 0);
	PDIR(page)->magic = SIFT_PARTDIR_MAGIC;
	PDIR(page)->level = level;
	PDIR(page)->manifest_version = version;
}

static void
dir_seal(Page page)
{
	SiftPartDirPageData *h = PDIR(page);

	SiftPageSetUsed(page, sizeof(*h) + h->nentries * (h->level == 0 ? sizeof(SiftPartitionEntry) : sizeof(SiftPartDirKey)));
	sift_page_seal(page);
}

/* The time bounds of a partition without a part: every range is empty, so that no query that names a time meets it */
static void
entry_set_no_parts_time(SiftPartitionEntry *e)
{
	e->flags = (e->flags & ~SIFT_PARTENTRY_HOT_ACTIVE) | SIFT_PARTENTRY_HAS_TIME0 | SIFT_PARTENTRY_HAS_TIME1;
	e->hot_parts = 0;
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		e->time_min[s] = PG_INT64_MAX;
		e->time_max[s] = PG_INT64_MIN;
	}
}

void
sift_partdir_build_root(Page page)
{
	SiftPartitionEntry *e;

	dir_init(page, 0, 1);
	PDIR(page)->nentries = 1;
	e = LEAF(page);
	memset(e, 0, sizeof(*e));
	e->part_id = 1;
	e->manifest_blk = e->l0_slab_blk = e->pend_del_blk = InvalidBlockNumber;
	e->min_xid = e->max_xid = InvalidFullTransactionId;
	entry_set_no_parts_time(e);
	dir_seal(page);
}

Page
sift_mf_read_page(Relation rel, BlockNumber blk, SiftPageType type, uint32 magic, uint64 gen, uint32 part_id, int *pages)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	Page		copy = palloc(BLCKSZ);

	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	sift_page_expect(rel, copy, blk, type, gen, part_id);
	if (*(uint32 *) SiftPageGetData(copy) != magic || !sift_page_sum_ok(copy))
		SIFT_CORRUPT(rel, "Block %u: the magic or the checksum of the manifest page is wrong.", blk);
	if (pages != NULL)
		(*pages)++;
	return copy;
}

/*
 * The bounds of slot s over the parts: exact when every part has them (SIFT_PART_FLAG_HAS_TIMEs), else the slot is unknown (its flag cleared, the range is all of int64) and no query prunes by it.
 * The flag of a partition that has a slab or a hot part is set separately: the slab's rows are in no part.
 */
static void
fold_time(SiftPartitionEntry *e, const SiftPartDesc *parts, int n)
{
	entry_set_no_parts_time(e);
	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		uint32		has = s == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1;
		bool		known = true;

		for (int i = 0; i < n && known; i++)
		{
			known = (parts[i].flags & has) != 0;
			if (known)
			{
				e->time_min[s] = Min(e->time_min[s], parts[i].time_min[s]);
				e->time_max[s] = Max(e->time_max[s], parts[i].time_max[s]);
			}
		}
		if (!known)
		{
			e->flags &= ~(s == 0 ? SIFT_PARTENTRY_HAS_TIME0 : SIFT_PARTENTRY_HAS_TIME1);
			e->time_min[s] = PG_INT64_MIN;
			e->time_max[s] = PG_INT64_MAX;
		}
	}
	for (int i = 0; i < n; i++)
		e->hot_parts += (parts[i].flags & SIFT_PART_FLAG_TIME_HOT) != 0;
}

void
sift_mf_refresh_hot_active(SiftPartitionEntry *e)
{
	if (e->hot_parts > 0 || e->l0_slab_blk != InvalidBlockNumber)
		e->flags |= SIFT_PARTENTRY_HOT_ACTIVE;
	else
		e->flags &= ~SIFT_PARTENTRY_HOT_ACTIVE;
}

void
sift_mf_fold_entry(SiftPartitionEntry *e, const SiftPartDesc *parts, int n)
{
	e->nparts = n;
	e->total_rows = e->total_dead = e->total_bytes = 0;
	e->l0_count = 0;
	e->min_xid = e->max_xid = InvalidFullTransactionId;
	fold_time(e, parts, n);
	sift_mf_refresh_hot_active(e);
	for (int i = 0; i < n; i++)
	{
		FullTransactionId hi = FullTransactionIdFollows(parts[i].dv_max_xid, parts[i].max_xid) && FullTransactionIdIsValid(parts[i].dv_max_xid) ?
			parts[i].dv_max_xid : parts[i].max_xid;

		e->total_rows += parts[i].nrows;
		e->total_dead += parts[i].ndead;
		e->total_bytes += parts[i].nbytes;
		e->l0_count += parts[i].level == 0;
		if (FullTransactionIdIsValid(parts[i].min_xid) && (!FullTransactionIdIsValid(e->min_xid) || FullTransactionIdPrecedes(parts[i].min_xid, e->min_xid)))
			e->min_xid = parts[i].min_xid;
		if (FullTransactionIdIsValid(hi) && (!FullTransactionIdIsValid(e->max_xid) || FullTransactionIdFollows(hi, e->max_xid)))
			e->max_xid = hi;
	}
}

/* The anchor the first page of a list keeps behind its parts: none when the page does not say it has one */
static SiftColDeltaAnchor
partlist_anchor(Relation rel, const SiftPartListPageData *l, const char *page)
{
	SiftColDeltaAnchor a = sift_col_delta_anchor_none();
	uint32		at = offsetof(SiftPartListPageData, parts) + l->nparts * sizeof(SiftPartDesc);

	if ((l->flags & SIFT_PARTLIST_FLAG_COL_DELTA) == 0)
		return a;
	if (SiftPageGetUsed(page) < at + sizeof(a))
		SIFT_CORRUPT(rel, "The part list of partition %llu says it has a column delta anchor and is too short to hold it.", (unsigned long long) l->part_id);
	memcpy(&a, SiftPageGetData(page) + at, sizeof(a));
	if (!sift_col_delta_anchor_sane(&a))
		SIFT_CORRUPT(rel, "The column delta anchor of partition %llu is not one of this format.", (unsigned long long) l->part_id);
	return a;
}

static SiftPartDesc *
read_parts_chain(Relation rel, const SiftPartitionEntry *entry, int *nparts, BlkVec *blocks, int *pages, SiftColDeltaAnchor *anchor)
{
	SiftPartDesc *out = palloc(Max(entry->nparts, 1) * sizeof(SiftPartDesc));
	int			n = 0;

	if (anchor != NULL)
		*anchor = sift_col_delta_anchor_none();

	for (BlockNumber blk = entry->manifest_blk; blk != InvalidBlockNumber;)
	{
		Page		p = sift_mf_read_page(rel, blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, entry->manifest_gen, (uint32) entry->part_id, pages);
		SiftPartListPageData *l = PLIST(p);

		if (l->part_id != entry->part_id || l->manifest_gen != entry->manifest_gen || l->nparts == 0 || l->nparts > SIFT_PARTLIST_CAP || n + l->nparts > entry->nparts)
			SIFT_CORRUPT(rel, "Block %u: the part list of partition %llu holds %u parts, the directory says %u in all.", blk, (unsigned long long) entry->part_id, l->nparts, entry->nparts);
		memcpy(out + n, l->parts, l->nparts * sizeof(SiftPartDesc));
		if (n == 0 && anchor != NULL)
			*anchor = partlist_anchor(rel, l, p);
		n += l->nparts;
		if (blocks != NULL)
			sift_mf_blk_add(blocks, blk);
		blk = l->next_blk;
		pfree(p);
	}
	if (n != (int) entry->nparts)
		SIFT_CORRUPT(rel, "The part list of partition %llu holds %d parts, the directory says %u.", (unsigned long long) entry->part_id, n, entry->nparts);
	*nparts = n;
	return out;
}

SiftPartDesc *
sift_manifest_read_parts(Relation rel, const SiftPartitionEntry *entry, int *nparts)
{
	return read_parts_chain(rel, entry, nparts, NULL, NULL, NULL);
}

/* A list page is written once, under a generation no other page has, and replaced by a page of a new one: the anchor of an entry is what the backend read from its page, until the entry changes */
typedef struct AnchorMemo
{
	RelFileLocator loc;
	uint64		part_id;
	uint64		gen;
	BlockNumber blk;
	SiftColDeltaAnchor anchor;
} AnchorMemo;

#define ANCHOR_MEMOS	8

static AnchorMemo anchor_memo[ANCHOR_MEMOS];
static int	anchor_memo_next;

static const AnchorMemo *
anchor_memo_find(Relation rel, const SiftPartitionEntry *e)
{
	for (int i = 0; i < ANCHOR_MEMOS; i++)
	{
		const AnchorMemo *m = &anchor_memo[i];

		if (m->gen == e->manifest_gen && m->blk == e->manifest_blk && m->part_id == e->part_id && RelFileLocatorEquals(m->loc, rel->rd_locator))
			return m;
	}
	return NULL;
}

SiftColDeltaAnchor
sift_manifest_read_col_delta(Relation rel, const SiftPartitionEntry *entry)
{
	const AnchorMemo *m;
	Page		p;
	SiftColDeltaAnchor a;

	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return sift_col_delta_anchor_none();
	if (entry->manifest_gen != 0 && (m = anchor_memo_find(rel, entry)) != NULL)
		return m->anchor;
	p = sift_mf_read_page(rel, entry->manifest_blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, entry->manifest_gen, (uint32) entry->part_id, NULL);
	a = partlist_anchor(rel, PLIST(p), p);
	pfree(p);
	anchor_memo[anchor_memo_next] = (AnchorMemo) {.loc = rel->rd_locator,.part_id = entry->part_id,.gen = entry->manifest_gen,.blk = entry->manifest_blk,.anchor = a};
	anchor_memo_next = (anchor_memo_next + 1) % ANCHOR_MEMOS;
	return a;
}

bool
sift_manifest_overlaps(const uint8 *lo, const uint8 *hi, const uint8 *qlo, const uint8 *qhi)
{
	return (qhi == NULL || bound_inf(qhi) || bound_cmp(lo, qhi) < 0) && (qlo == NULL || bound_inf(hi) || bound_cmp(hi, qlo) > 0);
}

/* The entries of the inline summary of block 0 that overlap [lo, hi) */
static void
collect_inline(const SiftPartitionEntry *ents, int n, const uint8 *lo, const uint8 *hi, SiftManifestRead *out)
{
	for (int i = 0; i < n; i++)
		if (sift_manifest_overlaps(ents[i].key_lo, ents[i].key_hi, lo, hi))
			out->entries[out->n++] = ents[i];
}

SiftManifestRead
sift_manifest_read_range(Relation rel, const uint8 *lo, const uint8 *hi)
{
	SiftPartitionEntry *inl;
	int			ninl;
	SiftColMetaPageData meta = sift_col_meta_read_inline(rel, &inl, &ninl);
	/* MEM[MEM-CORE-13]: caller-scoped SiftManifestRead partition array (n * 192 B) */
	SiftManifestRead out = {.entries = palloc(Max(ninl, 16) * sizeof(SiftPartitionEntry)),.version = meta.manifest_version};
	int			cap = Max(ninl, 16);

	if (inl != NULL)
		collect_inline(inl, ninl, lo, hi, &out);
	else
		sift_mf_collect_range(rel, meta.partdir_root_blk, meta.partdir_level, lo, hi, &out, &cap);
	return out;
}

bool
sift_manifest_same_topology(const SiftManifestRead *a, const SiftManifestRead *b)
{
	if (a->n != b->n)
		return false;
	for (int i = 0; i < a->n; i++)
		if (a->entries[i].part_id != b->entries[i].part_id || memcmp(a->entries[i].key_lo, b->entries[i].key_lo, SIFT_BOUND_BYTES) != 0)
			return false;
	return true;
}

static BlockNumber
write_partlist(Relation rel, uint64 part_id, uint64 gen, const SiftPartDesc *parts, int n, BlkVec *created, const SiftColDeltaAnchor *anchor)
{
	int			npages = (n + SIFT_PARTLIST_CAP - 1) / SIFT_PARTLIST_CAP;
	BlockNumber *blks;
	PGAlignedBlock img;
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	if (n == 0)
		return InvalidBlockNumber;
	blks = palloc(npages * sizeof(BlockNumber));
	for (int p = 0; p < npages; p++)
		blks[p] = sift_mf_new_page_blk(rel, created);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * SIFT_PARTLIST_CAP,
					cnt = Min(n - from, (int) SIFT_PARTLIST_CAP);
		SiftPartListPageData *l;

		sift_page_init_part(img.data, SIFT_PT_PARTLIST, gen, (uint32) part_id);
		l = PLIST(img.data);
		l->magic = SIFT_PARTLIST_MAGIC;
		l->nparts = cnt;
		l->next_blk = p + 1 < npages ? blks[p + 1] : InvalidBlockNumber;
		l->part_id = part_id;
		l->manifest_gen = gen;
		memcpy(l->parts, parts + from, cnt * sizeof(SiftPartDesc));
		SiftPageSetUsed(img.data, offsetof(SiftPartListPageData, parts) + cnt * sizeof(SiftPartDesc));
		if (p == 0 && sift_col_delta_anchor_has_chain(anchor))
		{
			l->flags |= SIFT_PARTLIST_FLAG_COL_DELTA;
			memcpy((char *) l->parts + cnt * sizeof(SiftPartDesc), anchor, sizeof(*anchor));
			SiftPageSetUsed(img.data, SiftPageGetUsed(img.data) + sizeof(*anchor));
		}
		sift_page_seal(img.data);
		sift_mf_log_image(&log, blks[p], img.data);
	}
	sift_newlog_end(&log);
	return blks[0];
}

static void
key_widen(SiftPartDirKey *k, int64 tmin, int64 tmax)
{
	k->time0_min = Min(k->time0_min, tmin);
	k->time0_max = Max(k->time0_max, tmax);
}

/* A partition's contribution to the bounds above it: those of its parts, all of time when it has a slab (its rows are in no part) or does not know them */
static void
key_take_entry(SiftPartDirKey *k, const SiftPartitionEntry *e)
{
	if (e->l0_slab_blk != InvalidBlockNumber || !(e->flags & SIFT_PARTENTRY_HAS_TIME0))
		key_widen(k, PG_INT64_MIN, PG_INT64_MAX);
	else
		key_widen(k, e->time_min[0], e->time_max[0]);
}

/* The entries cut into ceil(n / cap) leaves of about the same size, written; the references to them */
static int
write_leaves(Relation rel, const SiftPartitionEntry *ents, int n, uint64 version, BlkVec *created, SiftPartDirKey *out)
{
	int			nl = (n + SIFT_PARTDIR_LEAF_CAP - 1) / SIFT_PARTDIR_LEAF_CAP;
	int			per = nl > 0 ? (n + nl - 1) / nl : 0;
	PGAlignedBlock img;
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	for (int i = 0; i < nl; i++)
	{
		int			from = i * per,
					cnt = Min(per, n - from);
		BlockNumber blk = sift_mf_new_page_blk(rel, created);

		dir_init(img.data, 0, version);
		PDIR(img.data)->nentries = cnt;
		memcpy(LEAF(img.data), ents + from, cnt * sizeof(SiftPartitionEntry));
		dir_seal(img.data);
		sift_mf_log_image(&log, blk, img.data);
		out[i] = (SiftPartDirKey) {.child_blk = blk,.npartitions = cnt,.time0_min = PG_INT64_MAX,.time0_max = PG_INT64_MIN};
		memcpy(out[i].key_lo, ents[from].key_lo, SIFT_BOUND_BYTES);
		for (int j = from; j < from + cnt; j++)
			key_take_entry(&out[i], &ents[j]);
	}
	sift_newlog_end(&log);
	return nl;
}

static BlockNumber
write_node(Relation rel, const SiftPartDirKey *keys, int n, uint16 level, uint64 version, BlkVec *created)
{
	PGAlignedBlock img;
	BlockNumber blk = sift_mf_new_page_blk(rel, created);

	dir_init(img.data, level, version);
	PDIR(img.data)->nentries = n;
	memcpy(INTK(img.data), keys, n * sizeof(SiftPartDirKey));
	dir_seal(img.data);
	sift_mf_write_image(rel, blk, img.data);
	return blk;
}

static SiftPartDirKey
node_ref(BlockNumber blk, const SiftPartDirKey *keys, int n)
{
	SiftPartDirKey ref = {.child_blk = blk,.time0_min = PG_INT64_MAX,.time0_max = PG_INT64_MIN};

	for (int i = 0; i < n; i++)
	{
		ref.npartitions += keys[i].npartitions;
		key_widen(&ref, keys[i].time0_min, keys[i].time0_max);
	}
	memcpy(ref.key_lo, keys[0].key_lo, SIFT_BOUND_BYTES);
	return ref;
}

static bool
same_node(const OldNode *old, const SiftPartDirKey *keys, int n)
{
	return old->n == n && memcmp(old->keys, keys, n * sizeof(SiftPartDirKey)) == 0;
}

/* The interior pages over the leaf references r[0..n): a node of an earlier version whose children did not change is kept; what is not kept is dead */
static void
write_interior(Relation rel, Tree *t, const SiftPartDirKey *r, int n, uint64 version, BlkVec *created, BlockNumber *root, int *level, BlkVec *dead)
{
	int			nn = (n + SIFT_PARTDIR_INT_CAP - 1) / SIFT_PARTDIR_INT_CAP;
	SiftPartDirKey *refs;

	if (n == 1)
		nn = 0;
	refs = palloc(Max(nn, 1) * sizeof(SiftPartDirKey));
	for (int j = 0; j < nn; j++)
	{
		int			from = j * SIFT_PARTDIR_INT_CAP,
					cnt = Min(SIFT_PARTDIR_INT_CAP, n - from);
		BlockNumber blk;

		if (j < t->nnodes && same_node(&t->nodes[j], r + from, cnt))
		{
			blk = t->nodes[j].blk;
			t->nodes[j].kept = true;
		}
		else
			blk = write_node(rel, r + from, cnt, 1, version, created);
		refs[j] = node_ref(blk, r + from, cnt);
	}
	if (nn == 0)
	{
		*root = r[0].child_blk;
		*level = 0;
	}
	else if (nn == 1)
	{
		*root = refs[0].child_blk;
		*level = 1;
	}
	else
	{
		if (nn > (int) SIFT_PARTDIR_INT_CAP)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift index \"%s\" has more partitions than three levels of the directory hold", RelationGetRelationName(rel)),
						errhint("Merge empty partitions, or REINDEX the index.")));
		if (t->has_top && same_node(&t->top, refs, nn))
		{
			*root = t->top.blk;
			t->top.kept = true;
		}
		else
			*root = write_node(rel, refs, nn, 2, version, created);
		*level = 2;
	}
	for (int i = 0; i < t->nnodes; i++)
		if (!t->nodes[i].kept)
			sift_mf_blk_add(dead, t->nodes[i].blk);
	if (t->has_top && !t->top.kept)
		sift_mf_blk_add(dead, t->top.blk);
}

/* The interior pages when the commit changed one leaf of the page nodes[0] and no other: that page and the one above it are written again, with the reference to the new leaf in place of the old */
static void
write_path(Relation rel, Tree *t, const SiftPartDirKey *leaf, int at, uint64 version, BlkVec *created, BlockNumber *root, int *level, BlkVec *dead)
{
	const OldNode *node = &t->nodes[0];
	SiftPartDirKey keys[SIFT_PARTDIR_INT_CAP];
	SiftPartDirKey upper[SIFT_PARTDIR_INT_CAP];
	BlockNumber blk;

	memcpy(keys, node->keys, node->n * sizeof(SiftPartDirKey));
	keys[at] = *leaf;
	blk = write_node(rel, keys, node->n, 1, version, created);
	sift_mf_blk_add(dead, node->blk);
	memcpy(upper, t->top.keys, t->top.n * sizeof(SiftPartDirKey));
	upper[t->top_pos] = node_ref(blk, keys, node->n);
	*root = write_node(rel, upper, t->top.n, 2, version, created);
	*level = 2;
	sift_mf_blk_add(dead, t->top.blk);
}

typedef void (*BuildFn) (Relation rel, Tree *t, Change *ch, BlkVec *created, void *arg);

static void
read_leaves(Relation rel, Tree *t, int first, int span, Change *ch, int slack)
{
	ch->first = first;
	ch->span = span;
	ch->ents = palloc((span * SIFT_PARTDIR_LEAF_CAP + slack) * sizeof(SiftPartitionEntry));
	for (int i = 0; i < span; i++)
	{
		Page		p = sift_mf_read_dir(rel, t->leaves[first + i].child_blk, 0, &t->pages);

		memcpy(ch->ents + ch->nents, LEAF(p), PDIR(p)->nentries * sizeof(SiftPartitionEntry));
		ch->nents += PDIR(p)->nentries;
	}
}

static int
find_entry(const Change *ch, uint64 part_id, const uint8 *key_lo)
{
	for (int i = 0; i < ch->nents; i++)
		if (ch->ents[i].part_id == part_id && bound_cmp(ch->ents[i].key_lo, key_lo) == 0)
			return i;
	ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu with that lower bound is not in the manifest", (unsigned long long) part_id)));
	pg_unreachable();
}

static void
account(Change *ch, const SiftPartitionEntry *before, const SiftPartitionEntry *after)
{
	ch->d_rows += (int64) after->total_rows - (int64) before->total_rows;
	ch->d_dead += (int64) after->total_dead - (int64) before->total_dead;
	ch->d_bytes += (int64) after->total_bytes - (int64) before->total_bytes;
}

/* P2: the one record that makes the change the manifest; false when another commit moved the version since the tree was read */
static bool
publish(Relation rel, Tree *t, Change *ch, BlkVec *created, uint64 *version)
{
	SiftPartDirKey out[4];
	SiftPartDirKey *r;
	int			nr = 0,
				nout;
	BlkVec		dead = {0};
	BlockNumber root;
	int			level;
	uint64		new_version = t->meta.manifest_version + 1;
	SiftExtent *rel_ext;
	int			nrel = 0;
	FullTransactionId safexid;
	Buffer		buf0;
	SiftXlog	x;
	Page		p0;
	SiftColMetaPageData *m;
	SiftColMetaPageData committed;
	SiftShmIndexMeta shm_meta;
	int			first_batch;

	nout = write_leaves(rel, ch->ents, ch->nents, new_version, created, out);
	if (t->partial)
	{
		SIFT_INVARIANT(nout == ch->span && ch->span == 1, "a change that read one path of the partition directory changed the number of its leaves");
		write_path(rel, t, &out[0], ch->first, new_version, created, &root, &level, &dead);
	}
	else
	{
		r = palloc((t->nleaves + 4) * sizeof(SiftPartDirKey));
		memcpy(r, t->leaves, ch->first * sizeof(SiftPartDirKey));
		nr = ch->first;
		memcpy(r + nr, out, nout * sizeof(SiftPartDirKey));
		nr += nout;
		memcpy(r + nr, t->leaves + ch->first + ch->span, (t->nleaves - ch->first - ch->span) * sizeof(SiftPartDirKey));
		nr += t->nleaves - ch->first - ch->span;
		write_interior(rel, t, r, nr, new_version, created, &root, &level, &dead);
	}

	rel_ext = palloc((ch->old.n + dead.n + ch->span + ch->nrelease + ch->nproj_release) * sizeof(SiftExtent));
	for (int i = 0; i < ch->old.n; i++)
		rel_ext[nrel++] = (SiftExtent) {ch->old.v[i], 1};
	for (int i = 0; i < dead.n; i++)
		rel_ext[nrel++] = (SiftExtent) {dead.v[i], 1};
	for (int i = 0; i < ch->span; i++)
		rel_ext[nrel++] = (SiftExtent) {t->leaves[ch->first + i].child_blk, 1};
	for (int i = 0; i < ch->nrelease; i++)
		rel_ext[nrel++] = ch->release[i];
	for (int i = 0; i < ch->nproj_release; i++)
		rel_ext[nrel++] = ch->proj_release[i];

	safexid = sift_manifest_waits_own_xid(rel, nrel) ? GetTopFullTransactionId() : InvalidFullTransactionId;
	buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	m = SIFT_COL_META(BufferGetPage(buf0));
	if (m->manifest_version != t->meta.manifest_version)
	{
		UnlockReleaseBuffer(buf0);
		return false;
	}
	if (nrel > 0 && !FullTransactionIdIsValid(safexid) && !sift_manifest_unshared(rel))
		safexid = ReadNextFullTransactionId();
	first_batch = Min(nrel, (int) SIFT_FSM_PAGE_CAP);
	sift_fsm_reserve_release(rel, buf0, first_batch);
	x = sift_xlog_start(rel);
	p0 = sift_xlog_register(&x, buf0, 0);
	m = SIFT_COL_META(p0);
	m->manifest_version = new_version;
	m->partdir_root_blk = root;
	m->partdir_level = level;
	m->npartitions += ch->d_partitions;
	sift_col_meta_set_inline(p0, ch->ents, !t->partial && ch->first == 0 && ch->span == t->nleaves && ch->nents <= SIFT_META_INLINE_MAX_PARTS ? ch->nents : -1);
	m->total_rows += ch->d_rows;
	m->total_dead += ch->d_dead;
	m->total_bytes += ch->d_bytes;
	if (ch->set_scopes)
	{
		m->scope_dir_blk = ch->scope_dir_blk;
		m->nscopes = ch->nscopes;
		m->scope_dir_gen = ch->scope_dir_gen;
	}
	if (ch->set_projs)
	{
		m->proj_dir_blk = ch->proj_dir_blk;
		m->nprojs = ch->nprojs;
		m->proj_dir_gen = ch->proj_dir_gen;
		if (ch->nprojs > 0)
			m->common.ro_compat_flags |= SIFT_ROCOMPAT_PROJECTIONS;
		else
			m->common.ro_compat_flags &= ~SIFT_ROCOMPAT_PROJECTIONS;
	}
	if (ch->set_cd_dir)
		m->common.compat_flags |= SIFT_COMPAT_COL_DELTA_DIR;
	if (ch->set_tier)
		sift_mf_apply_tier(m, ch);
	if (FullTransactionIdIsValid(ch->new_min_xid) && (!FullTransactionIdIsValid(m->oldest_active_xid) || FullTransactionIdPrecedes(ch->new_min_xid, m->oldest_active_xid)))
		m->oldest_active_xid = ch->new_min_xid;
	sift_meta_seal(p0, m->common.struct_size);
	committed = *m;
	sift_fsm_release_in(rel, &x, rel_ext, first_batch, safexid);
	sift_xlog_done(&x);
	shm_meta = sift_col_meta_shm(rel, &committed);
	(void) sift_shm_manifest_publish(&shm_meta, NULL);
	if (nrel > first_batch)
		sift_fsm_release(rel, rel_ext + first_batch, nrel - first_batch, safexid);
	*version = new_version;
	return true;
}

static void
free_created(Relation rel, BlkVec *created)
{
	SiftExtent *e = palloc(Max(created->n, 1) * sizeof(SiftExtent));

	for (int i = 0; i < created->n; i++)
		e[i] = (SiftExtent) {created->v[i], 1};
	sift_fsm_release(rel, e, created->n, InvalidFullTransactionId);
}

static uint64
run_change(Relation rel, const uint64 *lock_ids, int nlocks, BuildFn build, void *arg, const uint8 *path_key)
{
	uint64		version;

	for (int i = 0; i < nlocks; i++)
		LockPage(rel, SIFT_PART_LOCK_BLKNO(lock_ids[i]), ExclusiveLock);
	for (;;)
	{
		SiftColMetaPageData meta = sift_col_meta_read(rel);
		Tree	   *t = path_key != NULL ? sift_mf_read_tree_path(rel, &meta, path_key) : sift_mf_read_tree(rel, &meta);
		Change		ch = {0};
		BlkVec		created = {0};

		build(rel, t, &ch, &created, arg);
		if (publish(rel, t, &ch, &created, &version))
			break;
		free_created(rel, &created);
	}
	for (int i = nlocks - 1; i >= 0; i--)
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(lock_ids[i]), ExclusiveLock);
	return version;
}

static void
build_commit(Relation rel, Tree *t, Change *ch, BlkVec *created, void *arg)
{
	const SiftManifestEdit *e = arg;
	SiftPartitionEntry before, *ent;
	SiftPartDesc *parts;
	SiftColDeltaAnchor anchor;
	int			n;
	uint64		gen;
	int			nexpire;
	uint64	   *expire;
	SiftGenMap	map;

	read_leaves(rel, t, sift_mf_find_leaf(t, e->key_lo), 1, ch, 2);
	ent = &ch->ents[find_entry(ch, e->part_id, e->key_lo)];
	before = *ent;
	parts = read_parts_chain(rel, ent, &n, &ch->old, &t->pages, &anchor);
	parts = repalloc(parts, (n + e->nadd) * sizeof(SiftPartDesc));
	n = sift_mf_drop_parts(rel, parts, n, e, &expire, &nexpire);
	sift_gen_map_init(&map, parts, n, e->nadd);
	sift_mf_apply_updates(rel, ent, parts, &map, e);
	for (int i = 0; i < e->nadd; i++)
	{
		sift_mf_check_added(rel, ent, &e->add_parts[i]);
		if (sift_gen_map_find(&map, e->add_parts[i].part_gen) >= 0)
			ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("sift index \"%s\": partition %llu already has a part of generation %llu", RelationGetRelationName(rel),
																	  (unsigned long long) e->part_id, (unsigned long long) e->add_parts[i].part_gen)));
		parts[n] = e->add_parts[i];
		sift_gen_map_add(&map, n++);
		if (FullTransactionIdIsValid(e->add_parts[i].min_xid) && (!FullTransactionIdIsValid(ch->new_min_xid) || FullTransactionIdPrecedes(e->add_parts[i].min_xid, ch->new_min_xid)))
			ch->new_min_xid = e->add_parts[i].min_xid;
	}
	sift_gen_map_free(&map);
	if (e->nupdate_scopes > 0)
		sift_mf_build_scopes(rel, t, ch, e, created);
	if (e->update_projs != NULL || e->ndrop > 0)
		sift_mf_build_commit_projs(rel, t, ch, e->update_projs, e->drop_gens, e->ndrop, created);
	sift_mf_build_tier(rel, t, ch, e, expire, nexpire, created);
	if (e->set_col_delta)
	{
		anchor = e->new_col_delta;
		if (!sift_col_delta_anchor_sane(&anchor))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": the column delta anchor of partition %llu is not valid", RelationGetRelationName(rel),
																			(unsigned long long) e->part_id)));
		if (FullTransactionIdIsValid(anchor.min_xid) && (!FullTransactionIdIsValid(ch->new_min_xid) || FullTransactionIdPrecedes(anchor.min_xid, ch->new_min_xid)))
			ch->new_min_xid = anchor.min_xid;
		ch->set_cd_dir = (anchor.flags & SIFT_CDLA_INDEXED_DIR) != 0;
	}
	if (sift_col_delta_anchor_has_chain(&anchor) && n == 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("sift index \"%s\": partition %llu would keep a column delta chain and have no part", RelationGetRelationName(rel),
																				 (unsigned long long) e->part_id),
						errhint("The edit that drops the last part of a partition clears its chain.")));
	sift_mf_fold_entry(ent, parts, n);
	if (sift_col_delta_anchor_has_chain(&anchor))
		ent->flags |= SIFT_PARTENTRY_HAS_COL_DELTA;
	else
		ent->flags &= ~SIFT_PARTENTRY_HAS_COL_DELTA;
	gen = sift_manifest_new_gen(rel, 1);
	ent->manifest_gen = gen;
	ent->manifest_blk = write_partlist(rel, ent->part_id, gen, parts, n, created, &anchor);
	if (e->set_l0_slab)
		ent->l0_slab_blk = e->new_l0_slab_blk;
	if (e->set_pend_del)
		ent->pend_del_blk = e->new_pend_del_blk;
	sift_mf_refresh_hot_active(ent);
	account(ch, &before, ent);
	ch->release = e->release;
	ch->nrelease = e->nrelease;
}

uint64
sift_manifest_commit(Relation rel, const SiftManifestEdit *edit)
{
	SIFT_F2_GATE(SIFT_F2_GATE_MANIFEST);
	return run_change(rel, &edit->part_id, 1, build_commit, (void *) edit, edit->key_lo);
}

typedef struct SplitArg
{
	uint64		part_id;
	const uint8 *key_lo;
	const uint8 *split_key;
	uint64		new_id;
} SplitArg;

static void
build_split(Relation rel, Tree *t, Change *ch, BlkVec *created, void *arg)
{
	SplitArg   *a = arg;
	SiftPartitionEntry *ent, neu;
	SiftPartDesc *parts;
	int			n;

	read_leaves(rel, t, sift_mf_find_leaf(t, a->key_lo), 1, ch, 2);
	ent = &ch->ents[find_entry(ch, a->part_id, a->key_lo)];
	if (bound_cmp(a->split_key, ent->key_lo) <= 0 || (!bound_inf(ent->key_hi) && bound_cmp(a->split_key, ent->key_hi) >= 0))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": the split key is not inside partition %llu", RelationGetRelationName(rel),
																		 (unsigned long long) a->part_id)));
	parts = read_parts_chain(rel, ent, &n, NULL, &t->pages, NULL);
	for (int i = 0; i < n; i++)
		if (bound_cmp(parts[i].key_hi, a->split_key) >= 0)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("sift index \"%s\": part %llu of partition %llu reaches the split key", RelationGetRelationName(rel),
																					  (unsigned long long) parts[i].part_gen, (unsigned long long) a->part_id),
							errhint("Compact the partition so that no part spans the split key.")));
	neu = *ent;
	neu.part_id = a->new_id;
	memcpy(neu.key_lo, a->split_key, SIFT_BOUND_BYTES);
	neu.manifest_blk = neu.l0_slab_blk = neu.pend_del_blk = InvalidBlockNumber;
	neu.flags &= ~SIFT_PARTENTRY_HAS_COL_DELTA;
	neu.nparts = neu.l0_count = 0;
	neu.manifest_gen = 0;
	neu.total_rows = neu.total_dead = neu.total_bytes = 0;
	neu.min_xid = neu.max_xid = InvalidFullTransactionId;
	entry_set_no_parts_time(&neu);
	memcpy(ent->key_hi, a->split_key, SIFT_BOUND_BYTES);
	memmove(ent + 2, ent + 1, (ch->ents + ch->nents - (ent + 1)) * sizeof(SiftPartitionEntry));
	ent[1] = neu;
	ch->nents++;
	ch->d_partitions = 1;
}

uint64
sift_manifest_split(Relation rel, uint64 part_id, const uint8 *key_lo, const uint8 *split_key)
{
	SplitArg	a = {.part_id = part_id,.key_lo = key_lo,.split_key = split_key,.new_id = sift_manifest_new_part_id(rel)};
	uint64		ids[2] = {part_id, a.new_id};

	run_change(rel, ids, 2, build_split, &a, NULL);
	return a.new_id;
}

typedef struct MergeArg
{
	uint64		keep_id;
	const uint8 *keep_lo;
	uint64		drop_id;
	const uint8 *drop_lo;
} MergeArg;

static void
merge_parts(Relation rel, Tree *t, Change *ch, BlkVec *created, SiftPartitionEntry *keep, const SiftPartitionEntry *drop)
{
	SiftPartitionEntry zero = {0};
	const SiftPartitionEntry *first = keep < drop ? keep : drop,
			   *second = keep < drop ? drop : keep;
	int			nf,
				ns;
	SiftPartDesc *pf = read_parts_chain(rel, first, &nf, &ch->old, &t->pages, NULL),
			   *ps = read_parts_chain(rel, second, &ns, &ch->old, &t->pages, NULL);
	SiftPartDesc *parts = repalloc(pf, (nf + ns) * sizeof(SiftPartDesc));
	SiftPartitionEntry before = *keep;

	memcpy(parts + nf, ps, ns * sizeof(SiftPartDesc));
	account(ch, drop, &zero);
	sift_mf_fold_entry(keep, parts, nf + ns);
	keep->manifest_gen = sift_manifest_new_gen(rel, 1);
	keep->manifest_blk = nf + ns > 0 ? write_partlist(rel, keep->part_id, keep->manifest_gen, parts, nf + ns, created, &(SiftColDeltaAnchor) {.delta_head_blkno = InvalidBlockNumber}) : InvalidBlockNumber;
	account(ch, &before, keep);
}

static void
build_merge(Relation rel, Tree *t, Change *ch, BlkVec *created, void *arg)
{
	MergeArg   *a = arg;
	int			lk = sift_mf_find_leaf(t, a->keep_lo),
				ld = sift_mf_find_leaf(t, a->drop_lo);
	SiftPartitionEntry *keep, *drop;

	if (abs(lk - ld) > 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": partitions %llu and %llu are not neighbours", RelationGetRelationName(rel),
																		 (unsigned long long) a->keep_id, (unsigned long long) a->drop_id)));
	read_leaves(rel, t, Min(lk, ld), lk == ld ? 1 : 2, ch, 0);
	keep = &ch->ents[find_entry(ch, a->keep_id, a->keep_lo)];
	drop = &ch->ents[find_entry(ch, a->drop_id, a->drop_lo)];
	if (keep->l0_slab_blk != InvalidBlockNumber || keep->pend_del_blk != InvalidBlockNumber ||
		drop->l0_slab_blk != InvalidBlockNumber || drop->pend_del_blk != InvalidBlockNumber || ((keep->flags | drop->flags) & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("sift index \"%s\": partitions %llu and %llu still hold a slab, pending deletes or a column delta chain", RelationGetRelationName(rel),
																				  (unsigned long long) a->keep_id, (unsigned long long) a->drop_id),
								errhint("Flush their slabs, fold their deletes and their column deltas first: only partitions made of parts merge.")));
	if (drop->nparts != 0 && keep->total_bytes + drop->total_bytes >= SIFT_PARTITION_MERGE_BYTES)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("sift index \"%s\": partitions %llu and %llu hold %llu bytes together, past the %llu a merge may join", RelationGetRelationName(rel),
																				  (unsigned long long) a->keep_id, (unsigned long long) a->drop_id,
																				  (unsigned long long) (keep->total_bytes + drop->total_bytes), (unsigned long long) SIFT_PARTITION_MERGE_BYTES),
								errhint("Merge neighbours that together hold less than the limit.")));
	if (drop != keep + 1 && drop + 1 != keep)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": partitions %llu and %llu are not neighbours", RelationGetRelationName(rel),
																		 (unsigned long long) a->keep_id, (unsigned long long) a->drop_id)));
	if (drop->nparts != 0)
		merge_parts(rel, t, ch, created, keep, drop);
	if (drop == keep + 1 && memcmp(keep->key_hi, drop->key_lo, SIFT_BOUND_BYTES) == 0)
		memcpy(keep->key_hi, drop->key_hi, SIFT_BOUND_BYTES);
	else if (drop + 1 == keep && memcmp(drop->key_hi, keep->key_lo, SIFT_BOUND_BYTES) == 0)
		memcpy(keep->key_lo, drop->key_lo, SIFT_BOUND_BYTES);
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift index \"%s\": partitions %llu and %llu are not neighbours", RelationGetRelationName(rel),
																		 (unsigned long long) a->keep_id, (unsigned long long) a->drop_id)));
	memmove(drop, drop + 1, (ch->ents + ch->nents - (drop + 1)) * sizeof(SiftPartitionEntry));
	ch->nents--;
	ch->d_partitions = -1;
}

void
sift_manifest_merge(Relation rel, uint64 keep_id, const uint8 *keep_lo, uint64 drop_id, const uint8 *drop_lo)
{
	MergeArg	a = {keep_id, keep_lo, drop_id, drop_lo};
	uint64		ids[2] = {Min(keep_id, drop_id), Max(keep_id, drop_id)};

	run_change(rel, ids, 2, build_merge, &a, NULL);
}
