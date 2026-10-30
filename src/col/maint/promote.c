/*
 * col/maint/promote.c - the promotion of the write buffer to an immutable segment
 * which segments are sealed and in which order
 * the floors that keep small segments from being promoted
 */
#include "postgres.h"
#include "col/maint/maint_int.h"

#ifdef SIFT_TESTING
int			sift_test_col_floor_sbs = 0;
#endif

int			sift_col_unit_floor = 2048;

/*
 * Churn lands a few dozen rows in each superblock it touches, and a unit costs a scan about as much as a thousand rows, however few it holds. Rows that
 * reach sbs superblocks (16 or more) with under half of a unit's rows each are therefore cut by TID range into units of sift.col_unit_floor rows (no more
 * than a superblock of the table holds), the units of a key-cut segment without a key, where the superblock cut would give a unit of a few dozen. Rows
 * that fill half a unit in every superblock, as those of a merge of everything do, and a table of fewer superblocks keep a unit for each. 0: the rule is off.
 */
uint32
maint_floor_rows(Relation index)
{
	uint32		aim;

	if (sift_col_unit_floor == 0 || !sift_col_is_index(index) || sift_col_key_cut(sift_col_rel(index)) || sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return 0;
	aim = sift_colk_aim_rows(index);
	return Max(aim > 0 ? Min(aim, (uint32) sift_col_unit_floor) : (uint32) sift_col_unit_floor, 64);
}

bool
maint_rows_sparse(uint64 rows, uint64 sbs, uint32 unit)
{
	return unit > 0 && rows < sbs * unit / 2;
}

bool
maint_unit_floor_applies(uint32 unit, uint64 rows, uint64 sbs)
{
	uint64		least = 16;

#ifdef SIFT_TESTING
	if (sift_test_col_floor_sbs > 0)
		least = sift_test_col_floor_sbs;
#endif
	return sbs >= least && maint_rows_sparse(rows, sbs, unit);
}

/* the segments in snap that are immutable, in a new array */
static SiftSegEntry *
immutable_segs(const SiftDirSnapshot *snap, int *n)
{
	SiftSegEntry *in = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));

	*n = 0;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			in[(*n)++] = snap->segs[i];
	return in;
}

static void
promote_one(Relation index, const SiftSegEntry *sealed, const SiftSegEntry *in, int nin)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftColRel *r = sift_col_rel_private(index, cx);	/* the floor reads the heap's statistics, a catalog read that reloads the cached state */
	const SiftColSchema *schema = r->schema;
	SiftColBatch **batches;
	int			nb;
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();
	uint64		rows = 0;
	SiftCPaggSpec rewrite = r->pagg;

	sift_cwbuf_flag_sealed(index, sealed);
	batches = sift_cwbuf_batches(index, sealed, schema, cx, &nb);
	if (nb > 0)
	{
		int			format = sift_col_format_minor;
		uint32		unit = maint_floor_rows(index);
		uint64		nbrows = 0;

		/* the rows of a write buffer written before the kinds were kept may lack them: such a segment is not minor 6, 7 or 8 */
		for (int i = 0; i < nb; i++)
		{
			nbrows += batches[i]->nrows;
			if (!sift_cbatch_kinds_exact(batches[i]))
				format = Min(format, SIFT_COL_FORMAT_TAGS);
		}
		wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		w = sift_colw_begin(wt, sift_store_new_gen(index), schema, r->policy, format, cx);
		{
			uint64		idx_rows = nbrows;

			for (int i = 0; i < nin; i++)
				idx_rows += in[i].ndocs;
			sift_colw_set_index_rows(w, idx_rows);
		}
		/* the lists the segments hold, as a merge writes them: a promotion that wrote the option's would leave the new segment without */
		if (sift_col_is_index(index))
			sift_col_pagg_rewrite(index, in, nin, &rewrite);
		sift_colw_set_preagg(w, &rewrite);
		if (sift_col_key_cut(r))
		{
			/* the rows of the sealed buffer come out in units of a key range, so that they overlay the older units in as few of them as their keys span */
			sift_colw_set_keyed(w, sift_colk_aim_rows(index));
			sift_colk_write(w, batches, nb, schema, r->cut, r->ncut, sift_colk_unit_rows(sift_colk_aim_rows(index)), cx);
		}
		else if (maint_unit_floor_applies(unit, nbrows, nb))
		{
			sift_colw_set_keyed(w, sift_colk_aim_rows(index));
			sift_colk_write(w, batches, nb, schema, NULL, 0, unit, cx);
		}
		else
			for (int i = 0; i < nb; i++)
				sift_colw_add(w, batches[i]);
		rows = sift_colw_rows(w);
		sift_colw_finish(w, &neu);
		sift_wt_finish(wt);
	}
	pages = sift_cwbuf_pages(index, sealed, &npages);
	sift_maint_replace(index, &sealed->gen, 1, nb > 0 ? &neu : NULL, pages, npages, "promoted a sealed column segment of", start, rows);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static const SiftSegEntry *
first_sealed(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_SEALED)
			return &snap->segs[i];
	return NULL;
}

SiftStepResult
maint_promote_next(Relation index, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *s;

	sift_dir_read(index, snap);
	if ((s = first_sealed(snap)) != NULL)
	{
		int			nin;
		SiftSegEntry *in = immutable_segs(snap, &nin);

		promote_one(index, s, in, nin);
		pfree(in);
	}
	pfree(snap);
	return s ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}
