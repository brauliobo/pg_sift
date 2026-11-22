/*
 * col/f2/f2_scope_over.c - what the summary of a scope lacks, or counts too many, for a snapshot. The rows of the parts it does not name and of the slabs are added to the cube of each key their
 * member belongs to; the rows of the parts it names that the snapshot does not see (a deletion vector, a pending delete, an inserter that is not settled) are taken out of it. Both are rows read
 * once, each granule that holds some decoded once for every key, and kept with the keys their members belong to, so that a key's cube of them is a read of its own rows.
 * A row is seen by the rules of sift_part_count_visible: its inserter (the version vector, the slab batch), the deletion vector of its part and the pending deletes of its partition.
 */
/* check-memory: allow-allocs the rows kept are the overlay of one snapshot, which sift_f2s_overlay_open and gone_open bound */
#include "postgres.h"

#include "col/f2/f2_scope.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/f2_scope_snap.h"
#include "col/f2/l0_slab.h"
#include "col/f2/part_vis.h"
#include "core/cmp.h"
#include "core/memtrack.h"

#include "col/f2/f2_scope_int.h"

struct GoneGranule
{
	SiftF2ScopeSlot *s;
	uint32		g;
	uint32		n;
	uint16	   *rows;
};

struct SiftF2ScopeRowSet
{
	uint32		n;
	uint32		cap;
	int64	   *member;
	int64	   *time;
	uint8	   *time_null;
	int64	   *dim[SIFT_SCOPE_DIMS_CAP];
	int64	   *meas[SIFT_SCOPE_MEAS_CAP];
	uint8	   *meas_null[SIFT_SCOPE_MEAS_CAP];
	SiftScopeRows rows;
	uint32		nruns;
	uint32	   *run_first;		/* the rows are ordered by member: run j is rows run_first[j] .. run_first[j + 1] - 1, those of one member */
	uint32		nkeys;			/* the keys of the pairs of the object, ascending (key_val), each with the runs of the members that hold it: key_run[key_off[k] .. key_off[k + 1] - 1] */
	int64	   *key_val;
	uint32	   *key_off;
	uint32	   *key_run;
	SiftScopeShape shape;		/* the rows are kept for other statements: nothing here points into the one that read them */
	const SiftF2ScopeCfg *sc;
};

static SiftTidIndexEntry *
slot_tix(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	if (s->tix == NULL)
	{
		SiftPart   *part = sift_f2s_slot_part(p, s);
		uint64		len;
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		s->tix = sift_part_read_section(p->rel, part, SEC_TID_INDEX, &len);
		MemoryContextSwitchTo(old);
		if (s->tix == NULL || len != (uint64) part->hdr.ngranules * sizeof(SiftTidIndexEntry))
			SIFT_SEG_CORRUPT(p->rel, part->hdr.part_gen, "The part has no usable TID index.");
	}
	return s->tix;
}

uint32
sift_f2s_unseen_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, uint16 *rows, uint32 n)
{
	const SiftGranuleDesc *dir;
	SiftPart   *part;
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);
	uint64	   *tids = NULL;
	SiftRowVer *vers = NULL;
	uint16	   *own,
			   *merged;
	uint32		nown = 0;

	if (!s->unseen)
		return n;
	(void) sift_f2s_slot_px(p, s);
	dir = sift_f2s_slot_gdir(p, s);
	part = sift_f2s_slot_part(p, s);
	if (s->px != NULL && sift_pdel_idx_may_hit_granule(s->px, &slot_tix(p, s)[g]))
		tids = sift_part_read_tids(p->rel, part, dir, g);
	if (!sift_vis_granule_inserts_settled(&s->d, &dir[g], xmin))
		vers = sift_part_read_vers(p->rel, part, dir, g);
	if (tids == NULL && vers == NULL)
		return n;
	own = palloc(Max(dir[g].nrows, 1) * sizeof(uint16));
	for (uint32 r = 0; r < dir[g].nrows; r++)
		if ((tids != NULL && sift_pdel_idx_has(s->px, tids[r])) || (vers != NULL && !sift_vis_inserted_cached(&p->xc, vers[r].xmin, vers[r].cmin)))
			own[nown++] = (uint16) r;
	merged = palloc(SIFT_GRANULE_ROWS * sizeof(uint16));
	n = sift_scope_excl_union(rows, n, own, nown, merged, SIFT_GRANULE_ROWS);
	memcpy(rows, merged, n * sizeof(uint16));
	pfree(own);
	pfree(merged);
	return n;
}

static struct SiftF2ScopeRowSet *
rowset_new(const SiftF2ScopeCfg *sc, uint32 cap)
{
	struct SiftF2ScopeRowSet *a = palloc0(sizeof(*a));

	a->sc = sc;
	a->shape = sc->shape;
	a->cap = cap;
	a->member = palloc(cap * sizeof(int64));
	if (sc->time_leaf >= 0)
	{
		a->time = palloc(cap * sizeof(int64));
		a->time_null = palloc(cap);
	}
	for (int d = 0; d < sc->shape.ndims; d++)
		a->dim[d] = palloc(cap * sizeof(int64));
	for (int m = 0; m < sc->shape.nmeas; m++)
	{
		a->meas[m] = palloc(cap * sizeof(int64));
		a->meas_null[m] = palloc(cap);
	}
	return a;
}

/* Row i of a decoded granule, after the rows of a (room for it made); a row of no member counts for no key and is left out */
static void
rowset_append(struct SiftF2ScopeRowSet *a, const SiftScopeRows *r, uint32 i)
{
	const uint32 k = a->n;

	if (r->member_null[i])
		return;
	a->member[k] = r->member[i];
	if (a->time != NULL)
	{
		a->time[k] = r->time[i];
		a->time_null[k] = r->time_null[i];
	}
	for (int d = 0; d < a->sc->shape.ndims; d++)
		a->dim[d][k] = r->dim[d][i];
	for (int m = 0; m < a->sc->shape.nmeas; m++)
	{
		a->meas[m][k] = r->meas[m][i];
		a->meas_null[m][k] = r->meas_null[m][i];
	}
	a->n++;
}

/* The rows of a decoded granule but those in gone (ascending) */
static void
added_granule(struct SiftF2ScopeRowSet *a, const SiftScopeRows *r, const uint16 *gone, uint32 ngone)
{
	uint32		at = 0;

	for (uint32 i = 0; i < r->n; i++)
	{
		if (at < ngone && gone[at] == i)
			at++;
		else
			rowset_append(a, r, i);
	}
}

static void
added_slab(struct SiftF2ScopeRowSet *a, SiftF2ScopeParts *p, const SiftRowBuf *rb, const SiftPendDelIndex *px)
{
	const SiftF2ScopeCfg *sc = a->sc;

	for (int r = 0; r < rb->n; r++)
	{
		const uint32 k = a->n;

		if (!sift_vis_inserted_cached(&p->xc, rb->xmin[r], rb->cmin[r]) || (px != NULL && sift_pdel_idx_has(px, rb->tid[r])) ||
			(p->excl_leaf >= 0 && !rb->isnull[p->excl_leaf][r]) || rb->isnull[sc->member_leaf][r])
			continue;
		a->member[k] = rb->i[sc->member_leaf][r];
		if (a->time != NULL)
		{
			a->time[k] = rb->i[sc->time_leaf][r] * sc->time_unit;
			a->time_null[k] = rb->isnull[sc->time_leaf][r];
		}
		for (int d = 0; d < sc->shape.ndims; d++)
			a->dim[d][k] = rb->isnull[sc->dim_leaf[d]][r] ? -1 : rb->i[sc->dim_leaf[d]][r];
		for (int m = 0; m < sc->shape.nmeas; m++)
		{
			const int	leaf = sc->meas_leaf[m];

			if (sc->shape.float_meas >> m & 1)
				memcpy(&a->meas[m][k], &rb->f[leaf][r], sizeof(double));
			else
				a->meas[m][k] = rb->i[leaf][r];
			a->meas_null[m][k] = rb->isnull[leaf][r];
		}
		a->n++;
	}
}

static void rowset_index(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a);

static int
by_member(const void *a, const void *b, void *arg)
{
	const int64 *m = arg;
	const uint32 x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return m[x] < m[y] ? -1 : m[x] > m[y] ? 1 : x < y ? -1 : x > y;
}

/* Orders the rows by member and tells the runs: a key's cube is the rows of the runs of the members it owns, whatever the pairs are by then */
static void
rowset_finish(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a)
{
	const uint32 n = a->n;
	uint32	   *perm = palloc(Max(n, 1) * sizeof(uint32));

	for (uint32 i = 0; i < n; i++)
		perm[i] = i;
	qsort_arg(perm, n, sizeof(uint32), by_member, a->member);
#define GATHER(arr, T) do { T *o = palloc(Max(n, 1) * sizeof(T)); for (uint32 i = 0; i < n; i++) o[i] = (arr)[perm[i]]; pfree(arr); (arr) = o; } while (0)
	GATHER(a->member, int64);
	if (a->time != NULL)
	{
		GATHER(a->time, int64);
		GATHER(a->time_null, uint8);
	}
	for (int d = 0; d < a->sc->shape.ndims; d++)
		GATHER(a->dim[d], int64);
	for (int m = 0; m < a->sc->shape.nmeas; m++)
	{
		GATHER(a->meas[m], int64);
		GATHER(a->meas_null[m], uint8);
	}
#undef GATHER
	a->run_first = palloc((n + 1) * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
		if (i == 0 || a->member[i] != a->member[i - 1])
			a->run_first[a->nruns++] = i;
	a->run_first[a->nruns] = n;
	pfree(perm);
	a->rows = (SiftScopeRows) {.n = n,.member = a->member,.time = a->time,.time_null = a->time_null};
	for (int d = 0; d < a->sc->shape.ndims; d++)
		a->rows.dim[d] = a->dim[d];
	for (int m = 0; m < a->sc->shape.nmeas; m++)
	{
		a->rows.meas[m] = a->meas[m];
		a->rows.meas_null[m] = a->meas_null[m];
	}
	rowset_index(p, a);
	a->sc = NULL;
}

static const SiftScopePairMap *
object_pairs(SiftF2ScopeParts *p)
{
	return p->pm_object != NULL ? p->pm_object : sift_f2s_pairs(p);
}

/* The runs of the members each key of the pairs of the object holds, by key, so that a key's rows are read without a pass over the runs of the others */
static void
rowset_index(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a)
{
	SiftScopePairMap *pm = (SiftScopePairMap *) object_pairs(p);
	uint32	   *fill = palloc0((pm->nkeys + 1) * sizeof(uint32));

	a->nkeys = pm->nkeys;
	a->key_val = memcpy(palloc(Max(pm->nkeys, 1) * sizeof(int64)), pm->scope_keys, pm->nkeys * sizeof(int64));
	a->key_off = palloc0((pm->nkeys + 1) * sizeof(uint32));
	for (int pass = 0; pass < 2; pass++)
	{
		for (uint32 j = 0; j < a->nruns; j++)
		{
			uint32		lo,
						hi;

			if (!sift_spm_find(pm, a->member[a->run_first[j]], &lo, &hi))
				continue;
			for (uint32 q = lo; q < hi; q++)
			{
				const uint32 ord = pm->key_ordinals[q];

				if (pass == 0)
					a->key_off[ord + 1]++;
				else
					a->key_run[a->key_off[ord] + fill[ord]++] = j;
			}
		}
		if (pass == 0)
		{
			for (uint32 k = 0; k < pm->nkeys; k++)
				a->key_off[k + 1] += a->key_off[k];
			a->key_run = palloc(Max(a->key_off[pm->nkeys], 1) * sizeof(uint32));
		}
	}
	pfree(fill);
}

static const int64 *
find_i64(const int64 *v, uint32 n, int64 x)
{
	return n > 0 ? bsearch(&x, v, n, sizeof(int64), sift_cmp_i64) : NULL;
}

/* The run of the member, UINT32_MAX when the set has none */
static uint32
run_of(const struct SiftF2ScopeRowSet *a, int64 member)
{
	uint32		lo = 0,
				hi = a->nruns;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (a->member[a->run_first[mid]] < member)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < a->nruns && a->member[a->run_first[lo]] == member ? lo : UINT32_MAX;
}

/* The runs of the members the key owns under the pairs of the read: those the object's pairs give it, but for the members the pairs of the read changed, and those of them it holds now */
static uint32 *
key_runs(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a, int64 key, uint32 *n)
{
	const int64 *at = find_i64(a->key_val, a->nkeys, key);
	const uint32 ord = at != NULL ? (uint32) (at - a->key_val) : 0;
	const uint32 first = at != NULL ? a->key_off[ord] : 0,
				last = at != NULL ? a->key_off[ord + 1] : 0;
	uint32	   *runs = palloc(Max(last - first + p->nchanged, 1) * sizeof(uint32));

	*n = 0;
	for (uint32 i = first; i < last; i++)
		if (find_i64(p->changed, p->nchanged, a->member[a->run_first[a->key_run[i]]]) == NULL)
			runs[(*n)++] = a->key_run[i];
	for (uint32 c = 0; c < p->nchanged; c++)
	{
		const uint32 j = run_of(a, p->changed[c]);

		if (j != UINT32_MAX && sift_f2s_owns(p, p->changed[c], key))
			runs[(*n)++] = j;
	}
	return runs;
}

static bool
rowset_cube(SiftF2ScopeParts *p, struct SiftF2ScopeRowSet *a, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	uint32		nruns;
	uint32	   *runs = key_runs(p, a, scope_key, &nruns);

	sift_scube_init(out, a->shape);
	for (uint32 r = 0; r < nruns; r++)
		for (uint32 i = a->run_first[runs[r]]; i < a->run_first[runs[r] + 1]; i++)
			if (month == SIFT_SCOPE_NO_MONTH || (a->time != NULL && !a->time_null[i] && sift_scope_month_of(a->time[i]) == month))
				sift_scube_add_row(out, &a->rows, i, NULL);
	pfree(runs);
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

bool
sift_f2s_added_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	SiftF2ScopeParts *p = arg;

	return rowset_cube(p, p->added, scope_key, month, out);
}

uint32
sift_f2s_added_months(void *arg, int64 scope_key, const int32 **months)
{
	SiftF2ScopeParts *p = arg;
	struct SiftF2ScopeRowSet *a = p->added;
	int32	   *all = palloc(Max(a->n, 1) * sizeof(int32));
	uint32		nruns,
				n = 0;
	uint32	   *runs = key_runs(p, a, scope_key, &nruns);

	for (uint32 r = 0; a->time != NULL && r < nruns; r++)
		for (uint32 i = a->run_first[runs[r]]; i < a->run_first[runs[r] + 1]; i++)
			if (!a->time_null[i])
				all[n++] = sift_scope_month_of(a->time[i]);
	pfree(runs);
	qsort(all, n, sizeof(int32), sift_cmp_i32);
	*months = all;
	return n;
}

/* The rows of the overlay a read is worth: a tenth of those of the parts the summary names, within bounds; for rows the backend keeps (settled) the reads that follow pay nothing for them, so more */
static uint64
overlay_cap(const SiftF2ScopeParts *p)
{
	const bool	kept = p->settled_ok;
	uint64		object_rows = 0;

	for (int i = 0; i < p->n; i++)
		object_rows += p->s[i].d.nrows;
	return Min(Max(object_rows / 100 * (kept ? SIFT_SCOPE_OVERLAY_KEPT_PCT : SIFT_SCOPE_OVERLAY_PCT), SIFT_SCOPE_OVERLAY_MIN_ROWS), SIFT_SCOPE_OVERLAY_MAX_ROWS);
}

/* The pending deletes of each partition of the manifest the snapshot sees, built when first asked for */
const SiftPendDelIndex *
sift_f2s_slot_px(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	if (!s->px_lazy)
		return s->px;
	for (int k = 0; k < p->mr.n; k++)
	{
		if (p->mr.entries[k].part_id != s->part_id)
			continue;
		if (!p->px_built[k])
		{
			int			ndead = 0;
			uint64	   *dead = sift_f2_pending_dead(p->rel, &p->mr.entries[k], p->snap, &ndead);
			MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

			p->px_part[k] = ndead > 0 ? sift_pdel_idx_build(dead, ndead) : NULL;
			p->px_built[k] = true;
			MemoryContextSwitchTo(old);
		}
		s->px = p->px_part[k];
	}
	s->px_lazy = false;
	return s->px;
}

/* The partitions of the manifest the snapshot sees: the pending deletes of each (set on its parts) and its slab, read; false when they are more than a read is worth */
static bool
load_partitions(SiftF2ScopeParts *p, const SiftManifestRead *mr, SiftRowBuf **slab, uint64 max_rows)
{
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);
	const SiftF2Rel *f = sift_f2_rel(p->rel);
	uint64		npend = 0,
				rows = 0;

	for (int k = 0; k < mr->n; k++)
	{
		const SiftPartitionEntry *e = &mr->entries[k];
		int			ndead = 0;
		uint64	   *dead = sift_f2_pending_dead(p->rel, e, p->snap, &ndead);

		npend += ndead;
		p->px_part[k] = ndead > 0 ? sift_pdel_idx_build(dead, ndead) : NULL;
		p->px_built[k] = true;
		for (int i = 0; i < p->n + p->nadd; i++)
		{
			SiftF2ScopeSlot *s = &p->s[i];
			const bool	settled = sift_vis_part_inserts_settled(&s->d, xmin);

			if (s->part_id != e->part_id)
				continue;
			s->px = p->px_part[k];
			s->unseen = p->px_part[k] != NULL || !settled;
			rows += i >= p->n || !settled ? s->d.nrows : 0;
		}
		if (e->l0_slab_blk != InvalidBlockNumber)
		{
			BlockNumber *pages;
			int			npages;

			slab[k] = sift_rowbuf_new(&f->spec);
			sift_l0_slab_read(p->rel, e->part_id, e->l0_slab_blk, slab[k], &f->spec, p->snap, &pages, &npages);
			rows += slab[k]->n;
		}
		if (rows > max_rows || npend > (p->settled_ok ? max_rows : SIFT_SCOPE_OVERLAY_PEND_MAX))
			return false;
	}
	return true;
}

/* The slots of the partitions that have pending deletes may hold rows the snapshot does not see; which of them is left to the first read of their rows (sift_f2s_slot_px) */
static void
slots_unseen_lazy(SiftF2ScopeParts *p, const SiftManifestRead *mr)
{
	const FullTransactionId xmin = sift_vis_snap_xmin(p->snap);

	for (int k = 0; k < mr->n; k++)
		for (int i = 0; i < p->n + p->nadd; i++)
		{
			SiftF2ScopeSlot *s = &p->s[i];

			if (s->part_id != mr->entries[k].part_id)
				continue;
			s->px_lazy = mr->entries[k].pend_del_blk != InvalidBlockNumber;
			s->unseen = s->px_lazy || !sift_vis_part_inserts_settled(&s->d, xmin);
		}
}

/* The parts of the manifest the summary does not name, after the ones it does */
static void
add_unnamed_parts(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m)
{
	for (int k = 0; k < mr->n; k++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(p->rel, &mr->entries[k], &np);

		for (int j = 0; j < np; j++)
		{
			SiftPart   *part;

			if (sift_f2s_meta_names(m, parts[j].part_gen))
				continue;
			part = sift_part_open(p->rel, parts[j].header_blk, parts[j].part_gen, (uint32) mr->entries[k].part_id);
			if (part->hdr.ngranules == 0)
				continue;
			if (p->n + p->nadd == p->cap)
				p->s = repalloc(p->s, (p->cap = Max(p->cap * 2, 16)) * sizeof(SiftF2ScopeSlot));
			p->s[p->n + p->nadd++] = (SiftF2ScopeSlot) {.part_gen = parts[j].part_gen,.part_id = mr->entries[k].part_id,.d = parts[j],.ng = part->hdr.ngranules,.part = part};
		}
	}
}

static void
added_parts_rows(struct SiftF2ScopeRowSet *a, SiftF2ScopeParts *p)
{
	for (int i = p->n; i < p->n + p->nadd; i++)
		for (uint32 g = 0; g < p->s[i].ng; g++)
		{
			SiftScopeRows r = {0};
			const uint16 *gone;
			const uint32 ngone = sift_f2s_granule_excl(p, &p->s[i], g, &gone);
			MemoryContext old;

			MemoryContextReset(p->gcx);
			old = MemoryContextSwitchTo(p->gcx);
			sift_f2s_decode_rows(p, &p->s[i], g, NULL, 0, false, &r);
			MemoryContextSwitchTo(old);
			added_granule(a, &r, gone, ngone);
		}
}

/* The rows the summary lacks that the snapshot sees, in the context of the change the backend keeps when there is one */
static struct SiftF2ScopeRowSet *
added_open(SiftF2ScopeParts *p, const SiftManifestRead *mr, SiftRowBuf **slab, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint64		cap = 0;
	struct SiftF2ScopeRowSet *a;

	for (int i = p->n; i < p->n + p->nadd; i++)
		cap += p->s[i].d.nrows;
	for (int k = 0; k < mr->n; k++)
		cap += slab[k] != NULL ? slab[k]->n : 0;
	a = cap == 0 ? NULL : rowset_new(p->sc, (uint32) cap);
	if (a != NULL)
	{
		added_parts_rows(a, p);
		for (int k = 0; k < mr->n; k++)
			if (slab[k] != NULL)
				added_slab(a, p, slab[k], p->px_part[k]);
		rowset_finish(p, a);
	}
	MemoryContextSwitchTo(old);
	return a;
}

bool
sift_f2s_overlay_open(SiftF2ScopeParts *p, const SiftManifestRead *mr, const SiftScopeMeta *m, const SiftScopeMeta *cached)
{
	SiftRowBuf **slab = palloc0(Max(mr->n, 1) * sizeof(SiftRowBuf *));
	uint64		stamp;
	MemoryContext rows_cx = GetMemoryChunkContext(p);
	bool		ok;

	p->mr = *mr;
	p->px_part = palloc0(Max(mr->n, 1) * sizeof(SiftPendDelIndex *));
	p->px_built = palloc0(Max(mr->n, 1));
	sift_xid_cache_init(&p->xc, p->snap);
	add_unnamed_parts(p, mr, m);
	p->settled_ok = cached != NULL && p->excl_leaf < 0 && sift_f2s_overlay_stamp(p, mr, &stamp);
	if (p->settled_ok && (p->settled = sift_f2_scope_snap_find(cached, stamp)) != NULL)
	{
		slots_unseen_lazy(p, mr);
		p->added = p->settled->added;
		p->gone = p->settled->gone;
		p->gone_many = p->settled->gone_many;
		return true;
	}
	if (p->settled_ok)
	{
		p->settled = sift_f2_scope_snap_start(cached, stamp);
		rows_cx = p->settled->cx;
	}
	ok = load_partitions(p, mr, slab, overlay_cap(p));
	if (ok)
	{
		p->added = added_open(p, mr, slab, rows_cx);
		if (p->settled != NULL)
		{
			p->settled->added = p->added;
			sift_f2_scope_snap_keep(cached, p->settled);
			sift_f2_scope_snap_grown(p->settled, GetMemoryChunkContext(p));
		}
	}
	else if (p->settled != NULL)
	{
		sift_f2_scope_snap_drop(p->settled);
		p->settled = NULL;
	}
	for (int k = 0; k < mr->n; k++)
		if (slab[k] != NULL)
			sift_rowbuf_free(slab[k]);
	return ok;
}

/* The granules of the parts the summary names that hold rows the snapshot does not see, each with those rows; false when they are more than max */
static bool
gone_granules(SiftF2ScopeParts *p, struct GoneGranule **out, uint32 *ngran, uint64 max, uint32 *total)
{
	uint32		cap = 0;
	struct GoneGranule *gran = NULL;

	*total = 0;
	*ngran = 0;
	for (int i = 0; i < p->n; i++)
	{
		SiftF2ScopeSlot *s = &p->s[i];

		for (uint32 g = 0; s->part_gen != 0 && sift_f2s_slot_dirty(p, s) && g < s->ng; g++)
		{
			const uint16 *rows;
			const uint32 n = sift_f2s_granule_excl(p, s, g, &rows);

			if (n == 0)
				continue;
			if (*total + n > max)
				return false;
			if (*ngran == cap)
			{
				cap = Max(cap * 2, 16);
				gran = gran == NULL ? palloc(cap * sizeof(struct GoneGranule)) : repalloc(gran, cap * sizeof(struct GoneGranule));
			}
			gran[(*ngran)++] = (struct GoneGranule) {s, g, n, memcpy(palloc(n * sizeof(uint16)), rows, n * sizeof(uint16))};
			*total += n;
		}
	}
	*out = gran;
	return true;
}

/* The rows of every granule of gran, decoded once: which keys they count for is told by the pairs of each read */
static struct SiftF2ScopeRowSet *
gone_rows(SiftF2ScopeParts *p, const struct GoneGranule *gran, uint32 ngran, uint32 total)
{
	struct SiftF2ScopeRowSet *a = rowset_new(p->sc, Max(total, 1));

	for (uint32 k = 0; k < ngran; k++)
	{
		SiftScopeRows r = {0};
		MemoryContext old;

		MemoryContextReset(p->gcx);
		old = MemoryContextSwitchTo(p->gcx);
		sift_f2s_decode_rows(p, gran[k].s, gran[k].g, gran[k].rows, gran[k].n, false, &r);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < gran[k].n; i++)
			rowset_append(a, &r, i);
	}
	rowset_finish(p, a);
	return a;
}

static void
gone_open(SiftF2ScopeParts *p)
{
	struct GoneGranule *gran = NULL;
	uint32		ngran,
				total;

	if (gone_granules(p, &gran, &ngran, overlay_cap(p), &total))
	{
		MemoryContext old = MemoryContextSwitchTo(p->settled != NULL ? p->settled->cx : GetMemoryChunkContext(p));

		p->gone = gone_rows(p, gran, ngran, total);
		MemoryContextSwitchTo(old);
	}
	else
		p->gone_many = true;
	if (p->settled != NULL)
	{
		p->settled->gone = p->gone;
		p->settled->gone_many = p->gone_many;
		p->settled->gone_read = true;
		sift_f2_scope_snap_grown(p->settled, GetMemoryChunkContext(p));
	}
}

SiftScopeGone
sift_f2s_gone_cube(void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out)
{
	SiftF2ScopeParts *p = arg;

	if (p->gone == NULL && !p->gone_many && !(p->settled != NULL && p->settled->gone_read))
		gone_open(p);
	if (p->gone_many)
		return SIFT_GONE_MANY;
	return rowset_cube(p, p->gone, scope_key, month, out) ? SIFT_GONE_ROWS : SIFT_GONE_NONE;
}
