/*
 * col/maint/side.c - writes the scope sidecar of the entity segments of an index (col/side.h)
 *
 * The pages of the sidecar go after the segment's own stream, as the net pre-aggregates of VACUUM do (col/maint/net.c): the segment keeps its generation, its header page is written
 * again at a new block with the extents it has now, and the directory entry and the old header page are switched and released together (sift_wt_publish). A crash before the switch
 * leaves the new pages unreferenced; after it the segment has the sidecar whole. The sidecar belongs to its segment: a merge or a rewrite writes a segment without it and the old one goes
 * with the old segment, so no merge can leave a sidecar that names units that are gone.
 */
/* check-memory: allow-allocs the extents of one segment and its sidecar */
#include "postgres.h"
#include "access/table.h"
#include "catalog/index.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"
#include "bm25/maint_int.h"
#include "core/membudget.h"
#include "core/ops.h"
#include "bm25/build.h"
#include "col/build.h"
#include "col/side.h"
#include "core/store.h"

typedef struct SideStep
{
	const SiftSegEntry *entry;
	const SiftColRel *r;
	const SideScopeInput *in;
	int			nin;
	SideCounts *cnt;
	uint64		stream_pages;	/* the pages of the segment's own stream: those after them are a sidecar the new one replaces (0: it has none) */
	uint64		written;		/* pages */
} SideStep;

/* The sidecar of the segment, put to the pages of the step as it is made: its extents. Its memory (scope col_side_build) is that of cx, which the caller deletes */
static SiftExtent *
side_build(SiftWriteTarget *wt, const SideStep *st, SiftColSegment *seg, MemoryContext cx, int *nadd)
{
	SiftSideImage *im;
	SiftExtent *add,
			   *own;
	MemoryContext old = MemoryContextSwitchTo(cx);

	SIFT_MEM_SCOPE(col_side_build, cx);
	im = sift_side_image_begin(wt, st->entry->gen, seg->nsb, st->nin, SIFT_PAYLOAD_BYTES(st->stream_pages > 0 ? st->stream_pages : seg->sr.npages));
	for (int k = 0; k < st->nin; k++)
		sift_side_image_scope(im, seg, st->r, &st->in[k], st->cnt);
	add = sift_side_image_finish(im, st->cnt, nadd);
	MemoryContextSwitchTo(old);
	own = palloc(*nadd * sizeof(SiftExtent));
	memcpy(own, add, *nadd * sizeof(SiftExtent));
	return own;
}

static void
side_write(Relation index, SiftWriteTarget *wt, const void *arg)
{
	SideStep   *st = (SideStep *) arg;
	SiftColSegment *seg = sift_cseg_open(index, st->entry, CurrentMemoryContext);
	const SiftSegHeaderData *old = seg->hdr;
	SiftSegEntry fresh = *st->entry;
	SiftMaintSwitch sw_dir = {.old = &st->entry->gen,.nold = 1,.neu = &fresh};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift col side image", ALLOCSET_DEFAULT_SIZES);
	SiftExtent *add,
			   *list,
			   *release = palloc((old->nextents + 1) * sizeof(SiftExtent));
	int			nadd,
				nlist = 0,
				nrelease = 0;
	uint32		mirror_len;
	const void *mirror;
	uint64		kept = 0,
				blocks = 0,
				dropped = 0;

	add = side_build(wt, st, seg, cx, &nadd);
	MemoryContextDelete(cx);
	list = palloc((old->nextents + nadd) * sizeof(SiftExtent));
	for (uint32 e = 0; e < old->nextents; e++)
		if (st->stream_pages > 0 && kept >= st->stream_pages)
		{
			release[nrelease++] = old->extents[e];
			dropped += old->extents[e].nblocks;
		}
		else
		{
			list[nlist++] = old->extents[e];
			kept += old->extents[e].nblocks;
		}
	if (st->stream_pages > 0 && kept != st->stream_pages)
		SIFT_SEG_CORRUPT(index, st->entry->gen, "The scope sidecar of the segment does not start at an extent of it (%llu of %llu pages).", (unsigned long long) kept, (unsigned long long) st->stream_pages);
	for (int i = 0; i < nadd; i++)
	{
		list[nlist++] = add[i];
		blocks += add[i].nblocks;
	}
	if (nlist > (int) SIFT_MAX_EXTENTS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the scope sidecar needs %d extents, the header holds %d", nlist, (int) SIFT_MAX_EXTENTS),
						errhint("Merge the segments first (sift.col_merge) so that the segment is written in fewer extents.")));
	mirror = sift_cseg_mirror_for(seg, nlist, &mirror_len);
	fresh.header_blk = sift_wt_write_seghdr(wt, old, list, nlist, mirror, mirror_len);
	release[nrelease++] = (SiftExtent) {st->entry->header_blk, 1};
	fresh.format = Max(fresh.format, SIFT_COL_FORMAT_SIDE);
	fresh.flags &= ~SIFT_SEG_NET;
	fresh.bytes = st->entry->bytes + (blocks - dropped) * BLCKSZ;
	sift_wt_finish(wt);
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw_dir,.nadd = 1,.release = release,.nrelease = nrelease,.extra = InvalidBuffer});
	st->written = blocks + 1;
	sift_cseg_close(seg);
}

/* An entity segment of rows and units enough */
static bool
side_eligible(const SiftSegEntry *e)
{
	return e->kind == SIFT_SEG_IMMUTABLE && !(e->flags & SIFT_SEG_SCOPE) && e->sb_hi - e->sb_lo + 1 >= SIDE_MIN_UNITS && e->ndocs > 0;
}

/* Does a sidecar made now get other stamps than the ones of the scope's record: the same horizon of every journal (a transaction that is still open holds it back) leaves its runs as stale as they are */
static bool
side_stamp_moves(Relation index, const SideScope *sc, Snapshot snap)
{
	SiftConnStamp now[SIFT_SCOPE_JOURNALS];

	return sift_scope_take(index, sc->ordinal, snap, now) == SIFT_CONN_OK && memcmp(now, sc->stamp, sizeof(now)) != 0;
}

/*
 * More than sift.col_scope_refresh_pct of the rows of the runs of the scope in the segment's sidecar sit in a run with a member whose pair changed since its stamp (or the journal cannot say
 * which), and a sidecar made now is stamped later: the runs are read by reference until the sidecar is made again.
 */
static bool
side_stale(Relation index, SiftColSegment *seg, const SideScope *sc, Snapshot snap)
{
	const SiftSide *sd = sift_cseg_side(seg);
	SiftConnKeys changed = {0};
	uint64		rows = 0,
				touched = 0;
	const int64 *keys;

	if (sc->nruns == 0)
		return false;
	if (sc->stamp[0].lineage == 0 || sift_scope_changed(index, sc->ordinal, sc->stamp, snap, SIDE_CHANGED_CAP, NULL, 0, NULL, &changed) != SIFT_CONN_OK)
		return side_stamp_moves(index, sc, snap);
	keys = sift_side_keys(seg, sd, sc, NULL);
	for (uint32 k = 0; changed.n > 0 && k < sc->nkeys; k++)
	{
		uint32		n;
		const SideRun *runs = sift_side_runs(seg, sd, sc, keys[k], &n);

		for (uint32 i = 0; i < n; i++)
		{
			rows += runs[i].rows;
			touched += sift_side_run_untouched(seg, sd, sc, &changed, &runs[i]) ? 0 : runs[i].rows;
		}
		pfree((void *) runs);
	}
	sift_conn_keys_free(&changed);
	return rows > 0 && touched * 100 > rows * sift_col_scope_refresh_pct && side_stamp_moves(index, sc, snap);
}

/* The pages of the segment's own stream when a sidecar or net pre-aggregates follow it, which a new sidecar replaces (0: neither) */
static uint64
side_stream_pages(SiftColSegment *seg)
{
	uint64		net_at;
	uint64		end;

	sift_cseg_net_pages(seg, &net_at);
	end = sift_cseg_side(seg) != NULL ? sift_side_at(seg) : net_at;
	return end == SIFT_PAYLOAD_BYTES(seg->sr.npages) ? 0 : SIFT_PAYLOAD_PAGE(end);
}

/* What the segment needs: a sidecar (nothing yet), a new one (a scope of the index has no record in it, or the stamp of one no longer vouches for its runs: *stream_pages is then the pages of the segment's stream before the old), or nothing */
static bool
side_needs(Relation index, const SiftSegEntry *e, const int *scopes, int nscopes, Snapshot snap, uint64 *stream_pages)
{
	SiftColSegment *seg;
	const SiftSide *sd;
	bool		needs;

	*stream_pages = 0;
	if (!side_eligible(e))
		return false;
	seg = sift_cseg_open(index, e, CurrentMemoryContext);
	sd = sift_cseg_side(seg);
	needs = sd == NULL;
	for (int i = 0; i < nscopes && !needs; i++)
	{
		const SideScope *sc = sift_side_scope(sd, scopes[i]);

		needs = sc == NULL || side_stale(index, seg, sc, snap);
	}
	*stream_pages = needs ? side_stream_pages(seg) : 0;
	sift_cseg_close(seg);
	return needs;
}

/*
 * The entity segments of the scopes that need a sidecar get one made from the pairs of the scopes (each ascending by key, then member): those with none, and those whose runs the journal no
 * longer vouches for. A segment is made again for all its scopes together. Returns the segments written. The caller holds the maintenance lock.
 */
int
sift_col_side_fill(Relation index, const SideScopeInput *in, int nin, SideCounts *cnt)
{
	SiftDirSnapshot *snap;
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	int			scopes[SIFT_SCOPE_MAX];
	int			done = 0;
	bool		net_dropped = false;

	for (int i = 0; i < nin; i++)
		scopes[i] = in[i].scope;
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SideStep	st = {.entry = &snap->segs[i],.r = r,.in = in,.nin = nin,.cnt = cnt};
		TimestampTz start;

		if (!side_needs(index, &snap->segs[i], scopes, nin, GetActiveSnapshot(), &st.stream_pages))
			continue;
		start = GetCurrentTimestamp();
		if (run_step(index, true, side_write, &st) == SIFT_STEP_DONE && st.written > 0)
		{
			net_dropped |= (snap->segs[i].flags & SIFT_SEG_NET) != 0;
			sift_log_maint(index, st.stream_pages > 0 ? "made the scope sidecar again of" : "wrote the scope sidecar of", start, snap->segs[i].ndocs, st.written * BLCKSZ);
			done++;
		}
	}
	pfree(snap);
	if (net_dropped)
		maint_net_refresh(index);
	return done;
}

/* The scopes that get sidecars: those whose journals vouch now (the time leaf is the index's), with the stamps a sidecar made now takes */
static int
side_inputs(Relation index, Snapshot snap, SideScopeInput *in)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	const SiftScopeSpecs *specs = sift_scope_specs(index);
	int			n = 0;

	if (sift_scope_time_leaf(r->schema->cols, r->schema->ncols) < 0)
		return 0;
	for (int s = 0; s < r->nscope; s++)
		if (sift_scope_take(index, s, snap, in[n].stamp) == SIFT_CONN_OK)
		{
			in[n].name = specs->s[s]->key_name;
			in[n++].scope = s;
		}
	return n;
}

static bool
side_due(Relation index, const SideScopeInput *in, int nin)
{
	SiftDirSnapshot *snap;
	int			scopes[SIFT_SCOPE_MAX];
	bool		due = false;
	uint64		stream_pages;

	for (int i = 0; i < nin; i++)
		scopes[i] = in[i].scope;
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs && !due; i++)
		due = side_needs(index, &snap->segs[i], scopes, nin, GetActiveSnapshot(), &stream_pages);
	pfree(snap);
	return due;
}

/* Does an entity segment of the index lack the sidecar it should have, or hold one whose runs are read by reference? */
bool
sift_col_side_due(Relation index)
{
	SideScopeInput in[SIFT_SCOPE_MAX];
	int			n = side_inputs(index, GetActiveSnapshot(), in);

	return n > 0 && side_due(index, in, n);
}

/*
 * The sidecars the index lacks, made from the pairs of its scopes under the snapshot of the caller, who holds the maintenance lock, without the changes of the caller's own transaction: the
 * stamps vouch for the runs against the journals, which forget the changes of a transaction that rolls back; the segments written. A scope whose journals cannot vouch has no record.
 */
int
sift_col_side_run(Relation index)
{
	Snapshot	snap = GetActiveSnapshot();
	SnapshotData committed;
	Relation	heap;
	SideScopeInput in[SIFT_SCOPE_MAX] = {{0}};
	SideCounts	cnt = {0};
	int			n = side_inputs(index, snap, in),
				done = 0;

	if (n == 0 || !side_due(index, in, n))
		return 0;
	heap = table_open(IndexGetRelation(RelationGetRelid(index), false), AccessShareLock);
	committed = *snap;
	committed.curcid = FirstCommandId;
	for (int i = 0; i < n; i++)
	{
		uint64		npairs;

		in[i].pairs = sift_scope_all_pairs(heap, index, in[i].scope, &committed, &npairs, &in[i].mm);
		in[i].npairs = npairs;
	}
	table_close(heap, AccessShareLock);
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_GATE_SIDE);
#endif
	done = sift_col_side_fill(index, in, n, &cnt);
	for (int i = 0; i < n; i++)
		if (in[i].pairs != NULL)
			pfree((void *) in[i].pairs);
	return done;
}

/* The pairs are read under a snapshot taken now: it sees what the writers of the pairs did before the journal's validation waited for them, and the journal this statement made */
void
sift_col_side_build(Relation index)
{
	Snapshot	snap;

	if (!sift_maint_lock(index, true))
		return;
	snap = RegisterSnapshot(GetLatestSnapshot());
	PushActiveSnapshot(snap);
	sift_col_side_run(index);
	PopActiveSnapshot();
	UnregisterSnapshot(snap);
	sift_maint_unlock(index);
}
