/*
 * table/load/write.c - the groups of a run written as segments (by the backend or by a parallel context) and published in the directory
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/seg.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "table/load/load_int.h"

void
load_publish_groups(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const Published *p = arg;

	for (int i = 0; i < p->n; i++)
	{
		dir->segs[dir->nsegs++] = p->segs[i];
		meta->ndocs += p->segs[i].ndocs;
		meta->sum_dl += p->segs[i].sum_dl;
	}
}

/* How many times a load settles a full directory again before it fails */
#define LOAD_ROOM_TRIES 8

/* A directory that is full (or past sift.table_backpressure) is made room in before the group is written, as a new write buffer does */
static void
make_room(Relation rel, SiftTblRel *st)
{
	sift_tbl_refresh(rel, st);
	if (st->dir.nsegs >= sift_tbl_max_segments - 2 || dir_backpressure(st))
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), true,
							   sift_tbl_backpressure ? SIFT_TBL_ROOM_STEPS : SIFT_TBL_SETTLE_STEPS);
}

/*
 * The lock of the active segment, held with room in the directory for n segments or, when the room stays short (rows of running transactions fill it),
 * for the fewest the directory has: returns how many segments may be published under this hold, at least one (room made by settling, as often as
 * writers that waited for it take it)
 */
int
load_lock_with_room(Relation rel, SiftTblRel *st, int n)
{
	int			room;

	/* the room made is taken by the writers that were waiting for it before the lock is: settle again, and take what there is only when it stays short */
	for (int tries = 0;; tries++)
	{
		make_room(rel, st);
		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		room = sift_tbl_max_segments - 2 - st->dir.nsegs;
		if (room >= n || tries == LOAD_ROOM_TRIES)
			break;
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	if (room < 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
				 errdetail("The groups of a load cannot be merged as fast as it writes them: rows of running transactions are in the way."),
				 errhint("Run VACUUM on the table, or end the transaction that wrote them.")));
	return Min(n, room);
}

/* The segments of groups already written (the caller keeps the orphan sweep from them), published in as few changes as the room of the directory allows */
void
load_publish_all(Relation rel, SiftTblRel *st, Published *pub)
{
	for (int done = 0; done < pub->n;)
	{
		Published	part = {pub->segs + done, load_lock_with_room(rel, st, pub->n - done)};

		sift_dir_update(rel, &(SiftDirChange) {.fn = load_publish_groups,.arg = &part,.extra = InvalidBuffer});
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		done += part.n;
		sift_tbl_refresh(rel, st);
		if (sift_tbl_inline_promote)
			(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
	}
}

#ifdef SIFT_TESTING
/* the phase a participant waits in for the advisory lock of sift.test_phase_gate + this, so that a test finds a worker that is writing a group */
#define SIFT_TBL_PHASE_LOAD	4
#endif

/* The units of group g of the run as one segment: the rows of a unit are one batch, written as the unit of its superblock */
void
load_write_group(const LoadRun *r, const char *rows, Relation rel, int g, SiftSegEntry *seg)
{
	const SiftTblLayout *l = layout_of(rel, sift_tbl_rel(rel));
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table load group", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table load row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL,
		(uint64) LOAD_SBS * sift_tbl_write_buffer_limit * 1024, tbl_col_format(r->vers)};
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));

#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_LOAD);
#endif
	for (int u = g * r->per; u < Min(r->units, (g + 1) * r->per); u++)
	{
		uint64		vblock = r->first + (uint64) u * SIFT_TBL_VBLOCKS_SB;
		const char *p = rows + r->uoff[u];

		CHECK_FOR_INTERRUPTS();
		sift_cbatch_reset(sw.batch, vblock / SIFT_SB_PAGES);
		for (uint32 n = 0; n < r->urows[u]; n++)
		{
			const LoadRec *rec = (const LoadRec *) p;
			HeapTupleData tup = {.t_len = rec->len - sizeof(LoadRec),.t_data = (HeapTupleHeader) (p + sizeof(LoadRec))};
			ItemPointerData tid;
			SiftColRow	row;

			ItemPointerSet(&tid, vblock + n / SIFT_TBL_MAX_OFF, n % SIFT_TBL_MAX_OFF + 1);
			MemoryContextSwitchTo(rowcx);
			heap_deform_tuple(&tup, desc, values, isnull);
			row_in(l, desc, &tid, values, isnull, &row);
			if (r->vers)
				sift_cbatch_add_ver(sw.batch, &row, r->xid, rec->cid);
			else
				sift_cbatch_add(sw.batch, &row);
			MemoryContextSwitchTo(cx);
			MemoryContextReset(rowcx);
			p += MAXALIGN(rec->len);
		}
		seal_flush(&sw);
	}
	sift_colw_finish(sw.w, seg);
	sift_wt_finish(sw.wt);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* What the participants of a parallel write share: the run with the buffer of its rows after it, and the next group nobody has taken */
typedef struct LoadShared
{
	LoadRun		run;
	int			ngroups;
	pg_atomic_uint32 next;
} LoadShared;

static void
take_groups(LoadShared *sh, SiftSegEntry *segs, Relation rel)
{
	uint32		g;

	while ((g = pg_atomic_fetch_add_u32(&sh->next, 1)) < sh->ngroups)
		load_write_group(&sh->run, (const char *) (sh + 1), rel, g, &segs[g]);
}

/* False, with nothing written, when no worker could be had: the backend then writes the groups alone */
bool
load_write_parallel(const LoadRun *r, const char *rows, Relation rel, int ngroups, SiftSegEntry *segs)
{
	int			nworkers = Min(ngroups - 1, max_parallel_maintenance_workers);
	Size		rowsz = r->uoff[r->units];
	SiftParLeader *ldr;
	LoadShared *sh;

	(void) GetCurrentTransactionId();	/* parallel mode assigns none */
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_tbl_load_worker", nworkers, sizeof(LoadShared) + rowsz, ngroups * sizeof(SiftSegEntry), false))
		return false;
	sh = ldr->shared;
	sh->run = *r;
	sh->run.common = (SiftParCommon) {RelationGetRelid(rel), InvalidOid, false};
	memcpy(sh + 1, rows, rowsz);
	sh->ngroups = ngroups;
	pg_atomic_init_u32(&sh->next, 0);
	if (!sift_par_launch(ldr, "sift_table", rel, nworkers))
		return false;
	take_groups(sh, ldr->parts, rel);
	sift_par_wait(ldr);
	memcpy(segs, ldr->parts, ngroups * sizeof(SiftSegEntry));
	sift_par_end(ldr);
	return true;
}

PGDLLEXPORT void sift_tbl_load_worker(dsm_segment *seg, shm_toc *toc);

void
sift_tbl_load_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;

	sift_par_worker_begin(&w, toc);
	take_groups(w.shared, w.parts, w.heap);
	sift_par_worker_end(&w);
}

/* The groups of a run: LOAD_SBS units each; a run the participants share is cut into groups of the same size, one for each of them */
int
load_share_units(LoadRun *r, Size bytes, int participants)
{
	r->per = LOAD_SBS;
	if (participants > 1 && bytes >= LOAD_PARALLEL_BYTES)
	{
		int			groups = Max((r->units + LOAD_SBS - 1) / LOAD_SBS, Min(participants, r->units / LOAD_MIN_UNITS));

		r->per = (r->units + groups - 1) / groups;
	}
	return (r->units + r->per - 1) / r->per;
}
