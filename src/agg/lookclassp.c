/*
 * agg/lookclassp.c - the classes of a big table's keys, made by workers together
 * The index of a table of millions of keys is read by workers (as many as max_parallel_workers_per_gather allows; sift.agg_par_min_rows decides, as for a scan) before the scan starts: each takes superblocks
 * as they come, classes their rows and gives each key the class it made, in an array of all the keys with the first writer winning (a key twice is a table that keeps its rows,
 * as in the load of one process). The leader then puts the classes of the workers in the order of their first rows, which is the order a single reader meets them in, so that the
 * classes are the same, with the same numbers, as a load by one process made.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "optimizer/cost.h"
#include "storage/shm_toc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "agg/agg.h"
#include "agg/lookclass_int.h"
#include "col/am.h"
#include "col/dictshm.h"

#define KEY_CHDR	UINT64CONST(0x53494654A101)
#define KEY_CSNAP	UINT64CONST(0x53494654A102)
#define KEY_CLUT	UINT64CONST(0x53494654A103)
#define KEY_COUT	UINT64CONST(0x53494654A104)
#define KEY_CVIEW	UINT64CONST(0x53494654A105)
#define KEY_CDICT	UINT64CONST(0x53494654A106)

#define GANG_MAX	14				/* the ids of a participant's classes are idbase + 1 .. idbase + DENSE_MAX_CLASSES in 16 bits: 15 participants, the leader one */
#define GANG_STRCAP	(512 * 1024)	/* the bytes of the strings of the classes a worker hands over */

typedef struct ClassShared
{
	Oid			heap;
	Oid			index;
	int			nneed;
	int			leaf[MAX_NEED];
	int			key;
	int			ncol;
	int			col[SIFT_AGG_MAX_JCOLS];
	uint32		cap;			/* the keys the array holds: the largest + 1 the index says there is (a hint), and no more than a table may span */
	uint32		limit;			/* the most keys a table may span */
	pg_atomic_uint32 next;		/* the next superblock to take */
	pg_atomic_uint32 bad;		/* a worker met what makes the table not one of classes: the others stop */
} ClassShared;

/* a worker's result: this, then tv[], tnull[], ckey[], cpos[] of its classes, the offsets of its strings and the strings (class_out) */
typedef struct ClassOut
{
	bool		bad;
	bool		over;			/* a key beyond the array: the index's hint of its largest key was short, the leader loads alone */
	bool		big;			/* the strings of the classes do not fit the area: the leader loads alone */
	uint32		ncls;
	uint32		n;
	uint32		nstr;
	uint32		strlen;
	uint64		rows;
} ClassOut;

typedef struct OutView
{
	int64	   *tv;
	uint32	   *tnull;
	int32	   *ckey;
	uint64	   *cpos;
	uint32	   *stroff;
	char	   *strbuf;
} OutView;

static Size
out_size(int ncol)
{
	Size		nstr = (Size) (DENSE_MAX_CLASSES + 1) * ncol;

	return MAXALIGN(sizeof(ClassOut)) + DENSE_MAX_CLASSES * ((Size) ncol * 8 + 4 + 4 + 8) + MAXALIGN(nstr * 4) + GANG_STRCAP;
}

static OutView
out_view(ClassOut *o, int ncol)
{
	char	   *p = (char *) o + MAXALIGN(sizeof(ClassOut));
	OutView		v;

	v.tv = (int64 *) p;
	p += (Size) DENSE_MAX_CLASSES * ncol * 8;
	v.cpos = (uint64 *) p;
	p += DENSE_MAX_CLASSES * 8;
	v.tnull = (uint32 *) p;
	p += DENSE_MAX_CLASSES * 4;
	v.ckey = (int32 *) p;
	p += DENSE_MAX_CLASSES * 4;
	v.stroff = (uint32 *) p;
	p += MAXALIGN((Size) (DENSE_MAX_CLASSES + 1) * ncol * 4);
	v.strbuf = p;
	return v;
}

static bool
gang_claim(void *carg, uint32 nsb, uint32 *lo, uint32 *hi)
{
	ClassShared *sh = carg;

	if (pg_atomic_read_u32(&sh->bad) != 0)
		return false;
	*lo = pg_atomic_fetch_add_u32(&sh->next, 1);
	*hi = *lo + 1;
	return *lo < nsb;
}

/* one participant's share: it takes superblocks until there are none, and leaves its classes and what it found in out */
static void
gang_read(ClassShared *sh, uint16 *lut, int slot, const UnitScope *sc, Relation rel, Relation ix, ColNeed *need, ClassOut *out)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "lookup class part", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	DenseLoad	d = {.cxt = cx,.lastsb = PG_UINT32_MAX,.shared = true,.idbase = slot * DENSE_MAX_CLASSES,.key = sh->key,.ncol = sh->ncol,
	.cap = sh->cap,.limit = sh->limit,.lut = lut,.strs.cxt = cx};
	OutView		v = out_view(out, sh->ncol);
	bool		ok;

	memcpy(d.col, sh->col, sizeof(d.col));
	dense_tables(&d, true);
	ok = lookcol_read_units(sc, rel, ix, NULL, need, sh->nneed, dense_unit, &d);
	out->over = d.over;
	out->bad = d.bad || !ok;
	if (out->bad)
		pg_atomic_write_u32(&sh->bad, 1);
	out->ncls = d.ncls;
	out->n = d.n;
	out->rows = d.rows;
	out->big = d.strs.len > GANG_STRCAP || d.strs.n > (uint32) (DENSE_MAX_CLASSES + 1) * sh->ncol;
	if (!out->bad && !out->big)
	{
		memcpy(v.tv, d.tv, (Size) d.ncls * sh->ncol * sizeof(int64));
		memcpy(v.tnull, d.tnull, d.ncls * sizeof(uint32));
		memcpy(v.ckey, d.ckey, d.ncls * sizeof(int32));
		memcpy(v.cpos, d.cpos, d.ncls * sizeof(uint64));
		out->nstr = d.strs.n;
		out->strlen = (uint32) d.strs.len;
		if (d.strs.n > 0)
		{
			memcpy(v.stroff, d.strs.off, d.strs.n * sizeof(uint32));
			memcpy(v.strbuf, d.strs.buf, d.strs.len);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

void
sift_class_worker_main(dsm_segment *seg, shm_toc *toc)
{
	ClassShared *sh = shm_toc_lookup(toc, KEY_CHDR, false);
	Snapshot	snap = RegisterSnapshot(RestoreSnapshot(shm_toc_lookup(toc, KEY_CSNAP, false)));
	ClassOut   *out = (ClassOut *) ((char *) shm_toc_lookup(toc, KEY_COUT, false) + (Size) ParallelWorkerNumber * out_size(sh->ncol));
	UnitScope	sc = {.snap = snap,.view = shm_toc_lookup(toc, KEY_CVIEW, true),.claim = gang_claim,.carg = sh};
	ColNeed		need[MAX_NEED] = {{0}};
	Relation	rel,
				ix;

	if (shm_toc_lookup(toc, KEY_CDICT, true) != NULL)
		sift_dictshm_join();
	PushActiveSnapshot(snap);
	rel = table_open(sh->heap, AccessShareLock);
	ix = index_open(sh->index, AccessShareLock);
	for (int p = 0; p < sh->nneed; p++)
		need[p].lf = &sift_colsrc_layout(ix)->leaf[sh->leaf[p]];
	gang_read(sh, shm_toc_lookup(toc, KEY_CLUT, false), ParallelWorkerNumber, &sc, rel, ix, need, out);
	index_close(ix, AccessShareLock);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
	UnregisterSnapshot(snap);
}

typedef struct Mix
{
	uint64		pos;
	int			w;
	uint32		k;
} Mix;

static int
mix_cmp(const void *a, const void *b)
{
	uint64		x = ((const Mix *) a)->pos,
				y = ((const Mix *) b)->pos;

	return x < y ? -1 : x > y;
}

/* the classes of the workers, in the order of their first rows, are the classes of d; the array of the keys, with the numbers of the workers' classes, becomes d's */
static void
gang_merge(DenseLoad *d, const ColNeed *need, ClassOut **outs, int nw, const uint16 *shlut)
{
	Mix		   *mix = palloc(nw * DENSE_MAX_CLASSES * sizeof(Mix));
	uint16	   *remap = palloc0((nw * DENSE_MAX_CLASSES + 1) * sizeof(uint16));
	int			nmix = 0;

	for (int w = 0; w < nw; w++)
	{
		OutView		v = out_view(outs[w], d->ncol);

		for (uint32 k = 0; k < outs[w]->ncls; k++)
			mix[nmix++] = (Mix) {v.cpos[k], w, k};
		d->n = Max(d->n, outs[w]->n);
		d->rows += outs[w]->rows;
	}
	qsort(mix, nmix, sizeof(Mix), mix_cmp);
	for (int m = 0; m < nmix; m++)
	{
		OutView		v = out_view(outs[mix[m].w], d->ncol);
		int64		tv[SIFT_AGG_MAX_JCOLS];
		uint32		nulls = v.tnull[mix[m].k],
					cls;

		memcpy(tv, &v.tv[(size_t) mix[m].k * d->ncol], d->ncol * sizeof(int64));
		for (int c = 0; c < d->ncol; c++)
			if (d->col[c] >= 0 && need[d->col[c]].lf->vtype == SCV_CODE && !((nulls >> c) & 1))
			{
				const char *b = v.strbuf + v.stroff[tv[c] - 1];
				uint32		len;

				memcpy(&len, b, sizeof(len));
				tv[c] = strids_id(&d->strs, b + sizeof(len), len);
			}
		if ((cls = dense_intern(d, tv, nulls, v.ckey[mix[m].k], mix[m].pos)) == 0)
		{
			d->bad = true;
			return;
		}
		remap[(size_t) mix[m].w * DENSE_MAX_CLASSES + mix[m].k + 1] = cls;
	}
	d->cap = d->n;
	d->lut = MemoryContextAllocHuge(d->cxt, (Size) d->n * sizeof(uint16));
	for (uint32 key = 0; key < d->n; key++)
		d->lut[key] = remap[shlut[key]];
	pfree(mix);
	pfree(remap);
}

/* may workers class this table (a read-only statement outside parallel mode, as many as a scan may have, sift.agg_par_min_rows as for a scan), and how many keys does its array hold? */
static bool
gang_wanted(AggExec *ex, Relation ix, const ColNeed *need, const DenseLoad *d, uint32 *cap)
{
	SiftSrcStats st;
	int			leaf = need[d->key].lf->id;
	PlannedStmt *ps = ex->css.ss.ps.state->es_plannedstmt;

	if (IsInParallelMode() || max_parallel_workers_per_gather <= 0 || d->ncomp > 0 || ps->commandType != CMD_SELECT || ps->hasModifyingCTE)
		return false;
	sift_colsrc_stats(ix, &st);
	if (st.nrows < 8.0 * sift_agg_par_min_rows || st.max[leaf] <= st.min[leaf])
		return false;
	*cap = (uint32) Min(st.max[leaf] + 1, (double) d->limit);
	return true;
}

/*
 * Makes the classes of d with workers when the table is big enough: true when it did (d is as a load by one process leaves it, d.bad set when the table is not one
 * of classes), false when the load is for the caller: no worker started, a key lay beyond the largest the index said, or the strings of the classes were many. A worker that did not
 * start leaves its superblocks to the others.
 */
bool
dense_gang(AggExec *ex, Relation rel, Relation ix, ColNeed *need, int nneed, DenseLoad *d)
{
	ParallelContext *pcxt;
	SiftSrcSource *src;
	ClassShared *sh;
	ClassOut  **outs;
	uint16	   *lut;
	char	   *p,
			   *view = NULL;
	Size		vlen;
	uint32		cap;
	int			nw;
	bool		dictshm,
				ok;
	MemoryContext cx;

	if (!gang_wanted(ex, ix, need, d, &cap))
		return false;
	nw = Min(max_parallel_workers_per_gather, GANG_MAX);
	cx = AllocSetContextCreate(CurrentMemoryContext, "lookup class gang", ALLOCSET_DEFAULT_SIZES);
	/* the workers read the index of the table, and the rows this transaction holds back for it are in no page for them */
	sift_col_flush_held();
	src = sift_colsrc_open(rel, ix, ex->snap, cx, NULL);
	vlen = sift_colsrc_view_size(src);
	dictshm = sift_dictshm_prepare();
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_class_worker_main", nw);
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(ClassShared));
	shm_toc_estimate_chunk(&pcxt->estimator, EstimateSnapshotSpace(ex->snap));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(cap, sizeof(uint16)));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(out_size(d->ncol), nw + 1));
	if (vlen > 0)
		shm_toc_estimate_chunk(&pcxt->estimator, vlen);
	if (dictshm)
		shm_toc_estimate_chunk(&pcxt->estimator, 1);
	shm_toc_estimate_keys(&pcxt->estimator, 4 + (vlen > 0) + dictshm);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		sift_colsrc_close(src);
		MemoryContextDelete(cx);
		return false;
	}
	if (dictshm)
		shm_toc_insert(pcxt->toc, KEY_CDICT, shm_toc_allocate(pcxt->toc, 1));
	sh = shm_toc_allocate(pcxt->toc, sizeof(ClassShared));
	*sh = (ClassShared) {.heap = RelationGetRelid(rel),.index = RelationGetRelid(ix),.nneed = nneed,.key = d->key,.ncol = d->ncol,.cap = cap,.limit = d->limit};
	for (int p2 = 0; p2 < nneed; p2++)
		sh->leaf[p2] = need[p2].lf->id;
	memcpy(sh->col, d->col, sizeof(sh->col));
	pg_atomic_init_u32(&sh->next, 0);
	pg_atomic_init_u32(&sh->bad, 0);
	shm_toc_insert(pcxt->toc, KEY_CHDR, sh);
	p = shm_toc_allocate(pcxt->toc, EstimateSnapshotSpace(ex->snap));
	SerializeSnapshot(ex->snap, p);
	shm_toc_insert(pcxt->toc, KEY_CSNAP, p);
	lut = shm_toc_allocate(pcxt->toc, mul_size(cap, sizeof(uint16)));
	memset(lut, 0, mul_size(cap, sizeof(uint16)));
	shm_toc_insert(pcxt->toc, KEY_CLUT, lut);
	p = shm_toc_allocate(pcxt->toc, mul_size(out_size(d->ncol), nw + 1));
	for (int w = 0; w <= nw; w++)
		memset(p + (Size) w * out_size(d->ncol), 0, sizeof(ClassOut));
	shm_toc_insert(pcxt->toc, KEY_COUT, p);
	if (vlen > 0)
	{
		view = shm_toc_allocate(pcxt->toc, vlen);
		sift_colsrc_view_export(src, view);
		shm_toc_insert(pcxt->toc, KEY_CVIEW, view);
	}
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		sift_colsrc_close(src);
		MemoryContextDelete(cx);
		return false;
	}
	gang_read(sh, lut, nw, &(UnitScope) {.snap = ex->snap,.view = vlen > 0 ? view : NULL,.claim = gang_claim,.carg = sh}, rel, ix, need, (ClassOut *) (p + (Size) nw * out_size(d->ncol)));
	WaitForParallelWorkersToFinish(pcxt);
	outs = palloc((nw + 1) * sizeof(ClassOut *));
	ok = true;
	for (int w = 0; w <= nw; w++)
	{
		outs[w] = (ClassOut *) (p + (Size) w * out_size(d->ncol));
		ok = ok && !outs[w]->big && !outs[w]->over;
	}
	if (ok)
	{
		for (int w = 0; w <= nw; w++)
			d->bad = d->bad || outs[w]->bad;
		if (!d->bad)
			gang_merge(d, need, outs, nw + 1, lut);
		ex->lookup_class_workers = pcxt->nworkers_launched;
	}
	pfree(outs);
	DestroyParallelContext(pcxt);
	ExitParallelMode();
	sift_colsrc_close(src);
	MemoryContextDelete(cx);
	return ok;
}
