/*
 * col/delete.c - deleting rows by TID (col/delete.h).
 */
#include "core/host.h"

#include "commands/vacuum.h"
#include "storage/lmgr.h"

#include "col/f2/del/delete.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/col_delta/idx.h"
#include "col/f2/del/slab.h"
#include "col/f2/part/idx.h"
#include "col/f2/vacuum/run.h"
#include "core/compat.h"
#include "core/hash.h"
#include "core/maint/sched.h"
#include "core/memtrack.h"
#include "core/page_io.h"

/* The rows that are dead already, in the stream of the vector of the part (a bulk delete) or in the vector in memory (a compaction), either may be NULL */
typedef struct DeadRows
{
	const SiftDvImage *img;
	const SiftDv *dv;
} DeadRows;

/* A row that is dead already is an older one whose address a later row took after a VACUUM: a delete of that address is for another row */
static bool
row_dead(const DeadRows *dead_rows, uint32 g, uint16 row)
{
	const SiftDvGran *dg;
	bool		dead = false;

	if (dead_rows->img != NULL)
		return sift_dv_image_dead(dead_rows->img, g, row);
	dg = dead_rows->dv ? sift_dv_granule(dead_rows->dv, g) : NULL;
	if (dg != NULL)
		(void) sift_dv_find(dg, row, &dead);
	return dead;
}

/* The marks of a part, grown as its rows are found in cx: a part that holds none of the targets allocates none */
typedef struct Marks
{
	SiftDvMark *v;
	int			n;
	int			cap;
	MemoryContext cx;
} Marks;

static void
marks_add(Marks *mk, SiftDvMark m)
{
	if (mk->n == mk->cap)
	{
		mk->cap = Max(16, mk->cap * 2);
		mk->v = mk->v ? repalloc(mk->v, mk->cap * sizeof(SiftDvMark)) : MemoryContextAlloc(mk->cx, mk->cap * sizeof(SiftDvMark));
	}
	mk->v[mk->n++] = m;
}

/* The rows the deletes name by the address their row has now (an address change of the column delta chain): the row is where the chain puts it, whatever raw address the part holds */
static void
locate_remapped(const SiftPart *part, const SiftCdRemapMap *rm, const SiftPendDelRec *recs, int n, bool *done, Marks *mk, const DeadRows *dead_rows)
{
	for (int i = 0; i < n; i++)
	{
		const SiftCdRemap *m = done[i] ? NULL : sift_cd_remap_by_new(rm, recs[i].tid);

		if (m == NULL || m->part_gen != part->hdr.part_gen || row_dead(dead_rows, m->granule_id, m->row))
			continue;
		marks_add(mk, (SiftDvMark) {m->granule_id, m->row, recs[i].xmax, recs[i].cmax});
		done[i] = true;
	}
}

/* The targets by address, ties by their place in recs: what a granule or a part finds the ones in its range of addresses in */
typedef struct DelOrder
{
	const SiftPendDelRec *recs;
	uint32	   *ord;
	int			n;
} DelOrder;

static int
ord_cmp(const void *a, const void *b, void *arg)
{
	const SiftPendDelRec *recs = arg;
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	if (recs[x].tid != recs[y].tid)
		return recs[x].tid < recs[y].tid ? -1 : 1;
	return x < y ? -1 : x > y;
}

static DelOrder
del_order(const SiftPendDelRec *recs, int n)
{
	DelOrder	o = {.recs = recs,.ord = palloc(Max(n, 1) * sizeof(uint32)),.n = n};

	for (int i = 0; i < n; i++)
		o.ord[i] = i;
	qsort_arg(o.ord, n, sizeof(uint32), ord_cmp, (void *) recs);
	return o;
}

/* The places in ord of the first target above lo, less one, and of the first above hi: the targets of [lo, hi] */
static void
ord_range(const DelOrder *o, uint64 lo, uint64 hi, int *from, int *to)
{
	int			a = 0,
				b = o->n;

	while (a < b)
	{
		int			mid = a + ((b - a) >> 1);

		if (o->recs[o->ord[mid]].tid < lo)
			a = mid + 1;
		else
			b = mid;
	}
	*from = a;
	b = o->n;
	while (a < b)
	{
		int			mid = a + ((b - a) >> 1);

		if (o->recs[o->ord[mid]].tid <= hi)
			a = mid + 1;
		else
			b = mid;
	}
	*to = a;
}

/* The targets of the granule not yet found whose address is in its range and whose block its filter may hold, by address, ties by place in recs */
static int
granule_candidates(const DelOrder *o, const SiftTidIndexEntry *ix, const bool *done, uint32 *cand)
{
	int			from,
				to,
				k = 0;

	ord_range(o, ix->tid_min, ix->tid_max, &from, &to);
	for (int j = from; j < to; j++)
		if (!done[o->ord[j]] && (ix->blk_bloom & sift_tid_block_bit(o->recs[o->ord[j]].tid)) != 0)
			cand[k++] = o->ord[j];
	return k;
}

/* More candidates than this are looked up in a table of their addresses; fewer are compared with each row */
#define DEL_LINEAR_CANDIDATES	16

typedef struct CandSet
{
	const SiftPendDelRec *recs;
	const uint32 *cand;
	int			n;
	uint32	   *slots;			/* the place in cand of the first of an address, plus one (NULL: the candidates are compared in turn) */
	uint32		mask;
} CandSet;

static void
cand_set_init(CandSet *cs, const SiftPendDelRec *recs, const uint32 *cand, int n)
{
	*cs = (CandSet) {.recs = recs,.cand = cand,.n = n};
	if (n <= DEL_LINEAR_CANDIDATES)
		return;
	cs->mask = pg_nextpower2_32(2 * (uint32) n) - 1;
	cs->slots = palloc0((cs->mask + 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
	{
		uint32		h = (uint32) (sift_fmix64(recs[cand[i]].tid) & cs->mask);

		while (cs->slots[h] != 0 && recs[cand[cs->slots[h] - 1]].tid != recs[cand[i]].tid)
			h = (h + 1) & cs->mask;
		if (cs->slots[h] == 0)
			cs->slots[h] = i + 1;
	}
}

/* The first of the candidates (they ascend by address) that has the address, or -1 */
static int
cand_first(const CandSet *cs, uint64 tid)
{
	if (cs->slots == NULL)
	{
		for (int i = 0; i < cs->n; i++)
			if (cs->recs[cs->cand[i]].tid == tid)
				return i;
		return -1;
	}
	for (uint32 h = (uint32) (sift_fmix64(tid) & cs->mask); cs->slots[h] != 0; h = (h + 1) & cs->mask)
		if (cs->recs[cs->cand[cs->slots[h] - 1]].tid == tid)
			return cs->slots[h] - 1;
	return -1;
}

static int
by_target_cmp(const void *a, const void *b, void *arg)
{
	const uint32 *cand = arg;
	uint32		x = cand[*(const int *) a],
				y = cand[*(const int *) b];

	return x < y ? -1 : x > y;
}

/* The rows of the granule that the candidates name, marked in the order of the targets; a row that moved to another address has left its raw one, a dead one is an older row the heap gave the address to again */
static void
mark_granule(const SiftPart *part, uint32 g, const uint64 *tids, int nrows, const DelOrder *o, const uint32 *cand, int nc, bool *done, Marks *mk, const DeadRows *dead_rows, const SiftCdRemapMap *rm)
{
	CandSet		cs;
	int		   *row = palloc(nc * sizeof(int));
	int		   *emit = palloc(nc * sizeof(int));
	int			ne = 0;

	cand_set_init(&cs, o->recs, cand, nc);
	for (int c = 0; c < nc; c++)
		row[c] = -1;
	for (int r = 0; r < nrows; r++)
		for (int c = cand_first(&cs, tids[r]); c >= 0 && c < nc && o->recs[cand[c]].tid == tids[r]; c++)
			if (row[c] < 0 && (rm == NULL || !sift_cd_remap_supersedes(rm, part->hdr.part_gen, g, r, tids[r])) && !row_dead(dead_rows, g, r))
				row[c] = r;
	for (int c = 0; c < nc; c++)
		if (row[c] >= 0)
			emit[ne++] = c;
	qsort_arg(emit, ne, sizeof(int), by_target_cmp, (void *) cand);
	for (int e = 0; e < ne; e++)
	{
		uint32		i = cand[emit[e]];

		marks_add(mk, (SiftDvMark) {g, (uint16) row[emit[e]], o->recs[i].xmax, o->recs[i].cmax});
		done[i] = true;
	}
	if (cs.slots != NULL)
		pfree(cs.slots);
	pfree(row);
	pfree(emit);
}

static void
locate_ordered(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const DelOrder *o, bool *done, Marks *mk, const DeadRows *dead_rows, const SiftCdRemapMap *rm)
{
	uint64		len;
	const SiftTidIndexEntry *tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);
	uint32	   *cand = palloc(Max(o->n, 1) * sizeof(uint32));

	if (rm != NULL)
		locate_remapped(part, rm, o->recs, o->n, done, mk, dead_rows);
	if (tix == NULL || len != (uint64) part->hdr.ngranules * sizeof(*tix))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The part has no usable TID index.");
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		int			nc = granule_candidates(o, &tix[g], done, cand);

		if (nc > 0)
		{
			uint64	   *tids = sift_part_read_tids(rel, part, dir, g);

			mark_granule(part, g, tids, dir[g].nrows, o, cand, nc, done, mk, dead_rows, rm);
			pfree(tids);
		}
	}
	pfree(cand);
}

/* Marks the rows of the part that the unresolved TIDs name; the TIDs it marked are set in done. rm: the address changes of the partition's column delta chain, NULL when it has none */
int
sift_del_locate(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const SiftPendDelRec *recs, int n, bool *done, SiftDvMark *marks, const SiftDv *base, const SiftCdRemapMap *rm)
{
	DelOrder	o = del_order(recs, n);
	Marks		mk = {.cx = CurrentMemoryContext};
	const DeadRows dead_rows = {.dv = base};

	locate_ordered(rel, part, dir, &o, done, &mk, &dead_rows, rm);
	if (mk.n > 0)
		memcpy(marks, mk.v, mk.n * sizeof(SiftDvMark));
	pfree(o.ord);
	if (mk.v != NULL)
		pfree(mk.v);
	return mk.n;
}

/* Whether a target not yet found has an address of the envelope of the part */
static bool
part_may_hold(const DelOrder *o, const SiftPartDesc *d, const bool *done)
{
	int			from,
				to;

	if (d->tid_max == 0)
		return true;
	ord_range(o, d->tid_min, d->tid_max, &from, &to);
	for (int j = from; j < to; j++)
		if (!done[o->ord[j]])
			return true;
	return false;
}

static uint16 *
granule_rows(const SiftGranuleDesc *dir, uint32 ng)
{
	uint16	   *rows = palloc(Max(ng, 1) * sizeof(uint16));

	for (uint32 g = 0; g < ng; g++)
		rows[g] = dir[g].nrows;
	return rows;
}

typedef struct PartMarks
{
	SiftDvMark *marks;
	int			nm;
	SiftDvImage *base;
	uint16	   *grows;
	uint32		ngranules;
	bool		unaborted;
} PartMarks;

static int
mark_cmp(const void *a, const void *b)
{
	const SiftDvMark *x = a,
			   *y = b;

	if (x->granule != y->granule)
		return x->granule < y->granule ? -1 : 1;
	return (int) x->row - (int) y->row;
}

/* The marks, ascending, as the chunks of a delta, a granule in as many chunks as SIFT_DV_DELTA_MAX_MARKS asks: rows is the array they point into */
static int
delta_grans(SiftDvMark *marks, int nm, SiftDvDeltaGran *gran, uint16 *rows)
{
	int			ng = 0;

	qsort(marks, nm, sizeof(SiftDvMark), mark_cmp);
	for (int i = 0; i < nm; i++)
	{
		rows[i] = marks[i].row;
		if (i == 0 || marks[i].granule != marks[i - 1].granule || gran[ng - 1].nrows == SIFT_DV_DELTA_MAX_MARKS)
			gran[ng++] = (SiftDvDeltaGran) {.granule_id = marks[i].granule,.rows = rows + i};
		gran[ng - 1].nrows++;
	}
	return ng;
}

static bool
one_deleter(const SiftPendDelRec *recs, int n)
{
	for (int i = 1; i < n; i++)
		if (recs[i].xmax != recs[0].xmax || recs[i].cmax != recs[0].cmax)
			return false;
	return n > 0;
}

/* The marks of the statement appended in place to the vectors that take them; the parts they leave are in taken[] as false */
static void
append_deltas(Relation rel, uint64 part_id, SiftPartDesc *parts, PartMarks *pm, int nparts, const SiftPendDelRec *recs, int n, bool *taken)
{
	SiftDvDeltaPart *dp = palloc(Max(nparts, 1) * sizeof(SiftDvDeltaPart));
	int		   *at = palloc(Max(nparts, 1) * sizeof(int));
	bool	   *took = palloc(Max(nparts, 1) * sizeof(bool));
	int			nd = 0;

	if (!one_deleter(recs, n))
		return;
	for (int p = 0; p < nparts; p++)
	{
		SiftDvDeltaGran *gran;
		uint16	   *rows;

		if (pm[p].nm == 0 || pm[p].unaborted || !sift_dvd_part_takes(&parts[p]))
			continue;
		gran = palloc0((pm[p].ngranules + pm[p].nm / SIFT_DV_DELTA_MAX_MARKS + 1) * sizeof(SiftDvDeltaGran));
		rows = palloc(pm[p].nm * sizeof(uint16));
		dp[nd] = (SiftDvDeltaPart) {.desc = &parts[p],.gran = gran,.ngran = delta_grans(pm[p].marks, pm[p].nm, gran, rows)};
		at[nd++] = p;
	}
	sift_dv_append_delta_batch(rel, (uint32) part_id, dp, nd, recs[0].xmax, recs[0].cmax, took);
	for (int i = 0; i < nd; i++)
		taken[at[i]] = took[i];
}

/* An index that CREATE INDEX CONCURRENTLY has not made valid yet is still given the rows the heap showed its validation: a delete that names a row it has not inserted is kept for it, not dropped as the delete of a row compacted away */
static bool
index_unbuilt(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX && !rel->rd_index->indisvalid;
}

static int
del_bulk(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs_in, int n_in, bool fold, const SiftPartSpec *spec, SiftMemTracker *mem)
{
	SiftManifestRead mr;
	SiftPartitionEntry entry;
	SiftPartDesc *parts;
	int			nparts,
				n = n_in,
				ncow = 0,
				nrel = 0,
				npend = 0,
				npend_pages = 0,
				nkeep,
				nsettle,
				npin,
				nin_recs,
				resolved = 0;
	BlockNumber settled = InvalidBlockNumber;
	SiftPendDelRec *recs,
			   *keep,
			   *pend = NULL;
	BlockNumber *pend_pages = NULL;
	bool	   *done,
			   *taken;
	PartMarks  *pm;
	DelOrder	order;
	SiftU64Set	remapped;
	int			remaining = 0;
	SiftCdRemapMap *rm;
	SiftPartDesc *updated;
	SiftExtent *rel_ext;
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo};
	MemoryContext part_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift del bulk part");

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	entry = mr.entries[0];
	if (fold && entry.pend_del_blk != InvalidBlockNumber)
		pend = sift_pend_del_read_unsettled(rel, part_id, entry.pend_del_blk, &settled, &npend, &pend_pages, &npend_pages);
	recs = palloc((n_in + npend + 1) * sizeof(SiftPendDelRec));
	if (npend > 0)
		memcpy(recs, pend, npend * sizeof(SiftPendDelRec));
	memcpy(recs + npend, recs_in, n_in * sizeof(SiftPendDelRec));
	n = sift_pend_del_unpair(recs, npend + n_in, &keep, &nkeep);
	nsettle = fold ? nkeep : 0;
	npin = sift_pend_del_settleable(rel, part_id, pend_pages, fold ? npend_pages : 0, &nin_recs);
	done = palloc0(Max(n, 1) * sizeof(bool));
	if (entry.l0_slab_blk != InvalidBlockNumber)
	{
		int			absorbed = sift_del_slab_flush(rel, part_id, key_lo, spec, entry.l0_slab_blk, recs, n, done);

		if (absorbed >= 0)
		{
			resolved += absorbed;
			mr = sift_manifest_read_range(rel, key_lo, NULL);
			entry = mr.entries[0];
		}
	}
	rm = sift_cdi_remap_load(rel, &entry);
	parts = sift_manifest_read_parts(rel, &entry, &nparts);
	pm = palloc0(Max(nparts, 1) * sizeof(PartMarks));
	taken = palloc0(Max(nparts, 1) * sizeof(bool));
	order = del_order(recs, n);
	sift_u64_set_init(&remapped, rm != NULL ? rm->n : 0);
	for (int i = 0; rm != NULL && i < rm->n; i++)
		sift_u64_set_add(&remapped, rm->slot[i].part_gen);
	for (int i = 0; i < n; i++)
		remaining += !done[i];
	for (int p = 0; p < nparts && remaining > 0; p++)
	{
		MemoryContext outer = CurrentMemoryContext;
		SiftPart   *part;
		SiftGranuleDesc *dir;
		Marks		mk = {.cx = outer};
		DeadRows	dead_rows;

		if (!sift_u64_set_has(&remapped, parts[p].part_gen) && !part_may_hold(&order, &parts[p], done))
			continue;
		sift_vacuum_delay_point();
		MemoryContextSwitchTo(part_cx);
		part = sift_part_open_desc(rel, &parts[p], (uint32) part_id);
		dir = sift_part_granule_dir(rel, part);
		MemoryContextSwitchTo(outer);
		pm[p].ngranules = part->hdr.ngranules;
		pm[p].grows = granule_rows(dir, part->hdr.ngranules);
		if (parts[p].dv_blk != InvalidBlockNumber)
			pm[p].base = sift_dv_image_read(rel, &parts[p], pm[p].grows, &pm[p].unaborted);
		dead_rows.img = pm[p].base;
		dead_rows.dv = NULL;
		MemoryContextSwitchTo(part_cx);
		locate_ordered(rel, part, dir, &order, done, &mk, &dead_rows, rm);
		MemoryContextSwitchTo(outer);
		MemoryContextReset(part_cx);
		pm[p].marks = mk.v;
		pm[p].nm = mk.n;
		if (mk.n == 0)
		{
			pfree(pm[p].grows);
			pm[p].grows = NULL;
			pm[p].base = NULL;
		}
		resolved += pm[p].nm;
		remaining -= pm[p].nm;
	}
	MemoryContextDelete(part_cx);
	(void) sift_mem_used(mem);
	sift_u64_set_free(&remapped);
	sift_cd_remap_free(rm);
	append_deltas(rel, part_id, parts, pm, nparts, recs, n, taken);
	if (index_unbuilt(rel))
		for (int i = 0; i < n; i++)
			if (!done[i])
				keep[nkeep++] = recs[i];
	for (int p = 0; p < nparts; p++)
		ncow += pm[p].nm > 0 && !taken[p];
	updated = palloc(Max(ncow, 1) * sizeof(SiftPartDesc));
	rel_ext = palloc((ncow + npend_pages + 1) * sizeof(SiftExtent));
	if (ncow > 0)
	{
		uint64		gen = sift_manifest_new_gen(rel, ncow);
		SiftDvStream *streams = palloc(ncow * sizeof(SiftDvStream));
		SiftPartDesc **descs = palloc(ncow * sizeof(SiftPartDesc *));
		int			k = 0;

		for (int p = 0; p < nparts; p++)
		{
			if (pm[p].nm == 0 || taken[p])
				continue;
			streams[k].dv_gen = gen + k;
			streams[k].bytes = sift_dv_image_merge(pm[p].base, pm[p].marks, pm[p].nm, parts[p].part_gen, gen + k, pm[p].ngranules, pm[p].grows, &streams[k].len);
			updated[k] = parts[p];
			descs[k] = &updated[k];
			if (parts[p].dv_blk != InvalidBlockNumber)
				rel_ext[nrel++] = (SiftExtent) {parts[p].dv_blk, parts[p].dv_nblocks};
			k++;
		}
		sift_dv_write_streams(rel, (uint32) part_id, streams, descs, ncow);
	}
	for (int i = npin; i < npend_pages; i++)
		rel_ext[nrel++] = (SiftExtent) {pend_pages[i], 1};
	e.update_parts = updated;
	e.nupdate = ncow;
	e.new_pend_del_blk = fold ? sift_pend_del_settle(rel, part_id, npin > 0 ? pend_pages[npin - 1] : settled, keep + nin_recs, nsettle - nin_recs) : InvalidBlockNumber;
	e.set_pend_del = fold && e.new_pend_del_blk != entry.pend_del_blk;
	e.release = rel_ext;
	e.nrelease = nrel;
	if (e.nupdate > 0 || e.set_pend_del)
		sift_manifest_commit(rel, &e);
	sift_pend_del_settle_pages(rel, part_id, pend_pages, npin);
	if (nkeep > nsettle)
		(void) sift_pend_del_append(rel, part_id, key_lo, keep + nsettle, nkeep - nsettle);
	(void) sift_mem_used(mem);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return resolved;
}

/* The rows are located and the vectors made in a context of their own, under the maintenance budget: what a flush of a bulk delete allocates is gone with it, whatever context the caller is in */
int
sift_del_bulk(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs_in, int n_in, bool fold, const SiftPartSpec *spec)
{
	SiftMemTracker mem;
	MemoryContext old;
	int			resolved;

	sift_mem_init(&mem, "f2_bulkdelete", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 1, 1);
	old = MemoryContextSwitchTo(mem.root_cx);
	resolved = del_bulk(rel, part_id, key_lo, recs_in, n_in, fold, spec, &mem);
	MemoryContextSwitchTo(old);
	sift_mem_end(&mem);
	return resolved;
}

/* The deletes of a partition may have left the rows of a part dead enough to rewrite: the maintenance is asked for it (a long chain or a slab of its own are asked for where they grow) */
static void
note_dead(Relation rel, uint64 part_id, const uint8 *key_lo)
{
	SiftPartitionEntry e;

	if (sift_manifest_find_entry(rel, part_id, key_lo, &e))
		sift_f2_maint_note_parts(rel, &e, 1, NULL, NULL);
}

/* The records do not fit the pages of the chain, and a page for them is the one that makes it SIFT_PEND_DEL_FOLD_PAGES long */
static bool
fills_last_page(Relation rel, uint64 part_id, const uint8 *key_lo, int n)
{
	SiftPartitionEntry e;
	SiftChainStamp st;

	if (!sift_manifest_find_entry(rel, part_id, key_lo, &e) || e.pend_del_blk == InvalidBlockNumber)
		return false;
	st = sift_pend_del_stamp(rel, part_id, e.pend_del_blk);
	return st.pages + 1 >= SIFT_PEND_DEL_FOLD_PAGES && st.count + n > (uint64) st.pages * SIFT_PEND_DEL_CAP;
}

/*
 * A buffer of pending deletes that has SIFT_PEND_DEL_FOLD_PAGES pages is folded by the maintenance when it is there, not by the statement that filled it; a statement folds it itself when the queue
 * is not there or the buffer has grown to four times that. A statement that folds the buffer anyway does not write the page that makes it long enough first: the records go into the fold.
 */
void
sift_del_route(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n, const SiftPartSpec *spec)
{
	if (n >= SIFT_BULK_DEL_MIN || (!sift_maint_here_available() && fills_last_page(rel, part_id, key_lo, n) && sift_pend_del_foldable(rel, part_id, key_lo)))
		(void) sift_del_bulk(rel, part_id, key_lo, recs, n, true, spec);
	else
	{
		int			pages = sift_pend_del_append(rel, part_id, key_lo, recs, n);
		bool		forced = pages >= 4 * SIFT_PEND_DEL_FOLD_PAGES;

		if ((forced || (pages >= SIFT_PEND_DEL_FOLD_PAGES && sift_pend_del_foldable(rel, part_id, key_lo))) &&
			(forced || !sift_f2_maint_request(rel, SIFT_MK_PEND_DEL_FOLD, part_id)))
			(void) sift_del_bulk(rel, part_id, key_lo, NULL, 0, true, spec);
	}
	note_dead(rel, part_id, key_lo);
}
