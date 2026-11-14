/*
 * col/scope_floor.c - the floor of the stamps of a scope index (col/scope.h)
 *
 * The entries of a scope index are exact as of the journal position of the pairs they were expanded under, and so is the absence of an entry (a row whose member had no pair then). The reader
 * takes the members whose pairs changed since the oldest of those positions as changed. That position cannot live in the scope segments: a promotion of rows with no pair, a refill that finds
 * none and a VACUUM that drops the segments with no live entry all leave an index with no scope segment, or with one that is newer than rows it knows nothing of. The index keeps it in its
 * metapage (SiftMetaPageData.scope_floor), changed in the same switch of the directory as the segments it describes:
 * - the build, and the first promotion of an index with no entity segment, set it (nothing stored is older);
 * - a promotion lowers it to its own stamp when that is older (one that started before a refill ended and was published after it);
 * - the refill that rewrites every scope segment under one stamp raises it to that stamp, unless a promotion published an entity segment it did not read meanwhile;
 * - a merge, VACUUM and the drop of a segment leave it alone: the floor is only ever older than the truth, which a read pays for in patched members and never in a wrong row.
 * Lineage 0 is no stamp: the index is not filled (a concurrent build, a journal that could not vouch), and the maintenance fills it.
 */
#include "postgres.h"
#include "col/seg.h"
#include "col/scope.h"
#include "core/store.h"

StaticAssertDecl(sizeof(SiftMetaStamp) == sizeof(SiftConnStamp), "the floor of the metapage holds the stamps of col/api/meta.h");
StaticAssertDecl(SIFT_META_STAMPS == SIFT_SCOPE_MAX * SIFT_SCOPE_JOURNALS, "the metapage has room for the stamps of every journal of every scope");

void
sift_scope_floor_get(const SiftMetaPageData *meta, int scope, SiftConnStamp *out)
{
	memcpy(out, &meta->scope_floor[scope * SIFT_SCOPE_JOURNALS], SIFT_SCOPE_JOURNALS * sizeof(SiftConnStamp));
}

static void
floor_put(SiftMetaPageData *meta, int scope, const SiftConnStamp *stamp)
{
	memcpy(&meta->scope_floor[scope * SIFT_SCOPE_JOURNALS], stamp, SIFT_SCOPE_JOURNALS * sizeof(SiftConnStamp));
}

/* The entity segments of a directory: a stored row some stamp must say the entries of */
static bool
has_entities(const SiftSegEntry *segs, int nsegs)
{
	for (int i = 0; i < nsegs; i++)
		if (segs[i].kind == SIFT_SEG_IMMUTABLE && !sift_seg_is_scope(&segs[i]))
			return true;
	return false;
}

/*
 * The floor of scope s once a segment written under mine is published: mine when no entity segment is stored (nothing is older), else the older of the two, and an unset floor stays unset (the
 * entity segments there are of no known stamp: the maintenance fills the index). Whether it differs from the floor now.
 */
static bool
lowered(const SiftMetaPageData *meta, bool entities, const SiftScopeStamps *mine, int s, SiftConnStamp *out)
{
	SiftConnStamp now[SIFT_SCOPE_JOURNALS];

	sift_scope_floor_get(meta, s, now);
	memcpy(out, now, sizeof(now));
	if (!entities)
		memcpy(out, mine->stamp[s], mine->nj[s] * sizeof(SiftConnStamp));
	else if (now[0].lineage != 0)
		for (int j = 0; j < mine->nj[s]; j++)
			out[j] = sift_scope_stamp_older(now[j], mine->stamp[s][j]);
	return memcmp(out, now, sizeof(now)) != 0;
}

static void
lower_fn(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const SiftScopeStamps *mine = arg;
	bool		entities = has_entities(dir->segs, dir->nsegs);

	for (int s = 0; s < mine->n; s++)
	{
		SiftConnStamp next[SIFT_SCOPE_JOURNALS];

		(void) lowered(meta, entities, mine, s, next);
		floor_put(meta, s, next);
	}
}

/* A segment written under the stamps mine is about to be published: the floor goes down to them. A switch of the directory only when it changes (the steady state of promotions leaves it as it is) */
void
sift_scope_floor_lower(Relation index, const SiftScopeStamps *mine)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	bool		entities,
				change = false;

	sift_dir_read(index, dir);
	entities = has_entities(dir->segs, dir->nsegs);
	for (int s = 0; s < mine->n; s++)
	{
		SiftConnStamp next[SIFT_SCOPE_JOURNALS];

		change |= lowered(&dir->meta, entities, mine, s, next);
	}
	pfree(dir);
	if (change)
		sift_dir_update(index, &(SiftDirChange) {.fn = lower_fn,.arg = (void *) mine,.extra = InvalidBuffer});
}

/*
 * In the switch of the directory of a refill: every entity segment was read and every scope segment written under mine, so the floor is mine, unless an entity segment that is not among
 * ents[0..nents) was published since (a promotion: its entries are those of its own stamp, which the floor was lowered to and must stay at).
 */
void
sift_scope_floor_raise(SiftMetaPageData *meta, const SiftDirectoryData *dir, const SiftScopeStamps *mine, const uint64 *ents, int nents)
{
	for (int i = 0; i < dir->nsegs; i++)
	{
		bool		read = false;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || sift_seg_is_scope(&dir->segs[i]))
			continue;
		for (int k = 0; k < nents; k++)
			read |= ents[k] == dir->segs[i].gen;
		if (!read)
			return;
	}
	for (int s = 0; s < mine->n; s++)
	{
		SiftConnStamp next[SIFT_SCOPE_JOURNALS] = {{0}};

		memcpy(next, mine->stamp[s], mine->nj[s] * sizeof(SiftConnStamp));
		floor_put(meta, s, next);
	}
}

static void
put_fn(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const SiftScopeStamps *fs = arg;

	for (int s = 0; s < fs->n; s++)
	{
		SiftConnStamp next[SIFT_SCOPE_JOURNALS] = {{0}};

		memcpy(next, fs->stamp[s], fs->nj[s] * sizeof(SiftConnStamp));
		floor_put(meta, s, next);
	}
}

/*
 * The end of a build: the floor is the oldest stamp of the segments it wrote, entity and scope (the participants of a parallel build each took their own; the entity segments carry the stamp
 * even when no scope segment was written). A build that wrote no segment (an empty table) takes the stamp of a fill now: what a promotion finds in its place is exact as of it. A segment of
 * no stamp (a concurrent build, whose journal began after it: sift_scope_stamp_concurrent fills it) leaves the floor unset.
 */
void
sift_scope_floor_after_build(Relation index)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col scope floor", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftScopeStamps *fs = palloc0(sizeof(SiftScopeStamps));
	const SiftScopeSpecs *specs = sift_scope_specs(index);
	bool		first = true;

	sift_dir_read(index, dir);
	fs->n = specs->n;
	for (int s = 0; s < fs->n; s++)
		fs->nj[s] = sift_scope_njournals(specs->s[s]);
	if (!has_entities(dir->segs, dir->nsegs))
	{
		struct SiftScopeFill *f = sift_scope_fill_begin(index, cx, 0);

		sift_scope_fill_stamps(f, fs);
		sift_scope_fill_end(f);
	}
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], cx);
		for (int s = 0; s < fs->n; s++)
			for (int j = 0; j < fs->nj[s]; j++)
			{
				SiftConnStamp st = s < seg->nscope && seg->scope_journals[s] == fs->nj[s] ? seg->scope[s][j] : (SiftConnStamp) {0};

				fs->stamp[s][j] = first ? st : sift_scope_stamp_older(fs->stamp[s][j], st);
			}
		first = false;
		sift_cseg_close(seg);
	}
	sift_dir_update(index, &(SiftDirChange) {.fn = put_fn,.arg = fs,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}
