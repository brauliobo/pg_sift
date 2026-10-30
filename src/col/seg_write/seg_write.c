/*
 * col/seg_write/seg_write.c - the writer of a segment: begin, add a batch, rows, bytes, finish
 * the sections are built by the files next to it
 * the order of the calls is the order of the file format
 */
#include "core/host.h"
#include "core/limits.h"
#include "col/seg_write/seg_write_int.h"

#define LST_SHARE_DIV		33		/* the leaf statistics take at most 1/33 (3%) of the segment: units of a few rows and many leaves keep none */

bool		sift_col_table_lookup = true;

bool		sift_col_table_key_table = true;

void
segw_sw_pad8(SiftStreamWriter *sw)
{
	static const char zeros[8] = {0};

	sift_sw_write(sw, zeros, ALIGN8(sift_sw_tell(sw)) - sift_sw_tell(sw));
}

int
sift_col_write_minor(const SiftColSchema *schema, int setting, bool kinds_exact)
{
	int			minor = setting;

	if (minor >= SIFT_COL_FORMAT_KINDS)
		minor = schema->kinds && kinds_exact ? SIFT_COL_FORMAT_KINDS : SIFT_COL_FORMAT_TAGS;
	if (schema->wide)
		minor = Max(minor, SIFT_COL_FORMAT_WIDE);	/* a wide schema has no older form */
	if (schema->contain)
		minor = Max(minor, SIFT_COL_FORMAT_CONTAIN);
	return minor;
}

SiftColSegWriter *
sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol, int format, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegWriter *w = palloc0(sizeof(SiftColSegWriter));

	w->wt = wt;
	w->gen = gen;
	w->schema = schema;
	w->pol = pol;
	w->mcxt = mcxt;
	w->format = sift_col_write_minor(schema, format, true);
	w->setting = format;
	w->sw = sift_sw_begin(wt, gen);
	w->dict = palloc0(schema->ncols * sizeof(SiftCDict *));
	w->dictmap = palloc0(schema->ncols * sizeof(uint32 *));
	w->inl = palloc0(schema->ncols * sizeof(bool));
	for (int c = 0; c < schema->ncols; c++)
		if (schema->cols[c].store == SIFT_COLS_STR)
			w->dict[c] = sift_cdict_create(mcxt);
	w->capsb = 64;
	w->vunit = palloc0(w->capsb * sizeof(VersUnit));
	initStringInfo(&w->vblob);
	w->zones = palloc(w->capsb * schema->ncols * sizeof(SiftCZone));
	w->lagg = palloc(w->capsb * schema->ncols * sizeof(SiftCLeafAgg));
	w->cubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->ecubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->xcubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->xecubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->pmask = palloc(w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	initStringInfo(&w->cube);
	initStringInfo(&w->ecube);
	initStringInfo(&w->xcube);
	initStringInfo(&w->xecube);
	w->sbdir = palloc(w->capsb * sizeof(SiftColSBRef));
	initStringInfo(&w->buf);
	MemoryContextSwitchTo(old);
	return w;
}

/* Interns the batch dictionary of each STR leaf not stored inline into the segment's: dictmap[col][batch code] = segment code */
static void
map_dicts(SiftColSegWriter *w, const SiftColBatch *b, const bool *inl)
{
	for (int c = 0; c < w->schema->ncols; c++)
	{
		const SiftCDict *bd = b->dict[c];

		if (w->dict[c] == NULL || inl[c])
			continue;
		w->dictmap[c] = w->dictmap[c] ? repalloc(w->dictmap[c], Max(bd->n, 1) * sizeof(uint32)) :
			MemoryContextAlloc(w->mcxt, Max(bd->n, 1) * sizeof(uint32));
		for (uint32 i = 0; i < bd->n; i++)
			w->dictmap[c][i] = sift_cdict_intern(w->dict[c], bd->bytes + bd->off[i], bd->off[i + 1] - bd->off[i]);
	}
}

/* The rows of each segment code of the leaves that count them (option count_columns): a unit that stores its strings inline, has an overflow value or belongs to a child leaf makes the leaf's counts unusable */
static void
count_rows(SiftColSegWriter *w, const SiftColBatch *b)
{
	for (int c = 0; w->cnt != NULL && c < w->schema->ncols; c++)
	{
		CountAcc   *a = &w->cnt[c];
		const SiftCVals *v = &b->col[c];
		uint32		n = w->dict[c] != NULL ? w->dict[c]->n : 0;

		if (!w->pagg.count[c] || a->bad)
			continue;
		if (w->dict[c] == NULL || w->inl[c] || v->ovf != NULL || v->n != b->nrows)
		{
			a->bad = true;
			continue;
		}
		if (a->cap < n)
		{
			uint32		cap = Max(n, 2 * a->cap);

			a->rows = a->rows ? repalloc(a->rows, (Size) cap * sizeof(uint32)) : MemoryContextAlloc(w->mcxt, (Size) cap * sizeof(uint32));
			memset(a->rows + a->cap, 0, (Size) (cap - a->cap) * sizeof(uint32));
			a->cap = cap;
		}
		for (uint32 i = 0; i < v->n; i++)
			if (!(v->nulls && sift_col_bit_test(v->nulls, i)))
				a->rows[w->dictmap[c][v->key[i]]]++;
	}
}

/* The accumulators of the leaves flagged in want, NULL when there are none */
static SetAcc *
acc_new(SiftColSegWriter *w, const bool *want)
{
	SetAcc	   *acc = NULL;
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	for (int c = 0; c < w->schema->ncols; c++)
		if (want[c])
		{
			if (acc == NULL)
				acc = palloc0(w->schema->ncols * sizeof(SetAcc));
			initStringInfo(&acc[c].blobs);
			acc[c].off = palloc0(((Size) w->capsb + 1) * sizeof(uint64));
		}
	MemoryContextSwitchTo(old);
	return acc;
}

void
sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec)
{
	w->pagg = *spec;
	w->excepts = w->setting >= SIFT_COL_FORMAT_PRESENCE && !spec->table;
	/* the cube asks for min and max: the segment is minor 7, which the caller may not have allowed (sift.col_format_minor) or the kinds of its rows forbid */
	w->pagg.minmax = spec->minmax && spec->ndims > 0 && w->setting >= SIFT_COL_FORMAT_MINMAX;
	if (w->pagg.minmax)
		w->format = SIFT_COL_FORMAT_MINMAX;
	else if (w->format == SIFT_COL_FORMAT_CALENDAR)
		w->format = sift_col_write_minor(w->schema, w->setting, true);	/* the calendar dims came with the default spec this one replaces */
	/* an index that names calendar columns (calendar_columns): minor 8, where the setting allows it; under a lower one the segment is written as before, without them */
	w->pagg.ndims = 0;
	for (int d = 0; d < spec->ndims; d++)
	{
		if (spec->dim[d].cal && w->setting < SIFT_COL_FORMAT_CALENDAR)
			continue;
		w->pagg.dim[w->pagg.ndims++] = spec->dim[d];
		if (spec->dim[d].cal)
			w->format = Max(w->format, SIFT_COL_FORMAT_CALENDAR);
	}
	/* the rollup of the rows is minor 9: under a lower setting the segment is written as before, without it (the entry is -1, the first of the sorted rollups) */
	if (w->pagg.nrollups > 0 && w->pagg.rollup[0].list < 0)
	{
		if (w->setting >= SIFT_COL_FORMAT_ROWS)
			w->format = SIFT_COL_FORMAT_ROWS;
		else
		{
			w->pagg.nrollups--;
			memmove(w->pagg.rollup, w->pagg.rollup + 1, w->pagg.nrollups * sizeof(SiftCPaggRollup));
		}
	}
	/* the rollups carry the length of each measure (minor 36) under the setting of the compact forms of the 3.7 minors (15), else as before */
	w->pagg.measlen = w->setting >= SIFT_COL_FORMAT_PRESENCE;
	/* a leaf asks for its keys (a summary, or a key list or bitmap) or its sorted run: minor 10, 15 or 13 when some unit gets one (sift_colw_finish) */
	if (w->setting < SIFT_COL_FORMAT_PRESENCE)
		memset(w->pagg.pres, 0, sizeof(w->pagg.pres));
	if (w->setting >= SIFT_COL_FORMAT_SETS && w->set == NULL)
	{
		bool		want[SIFT_COL_MAX_COLS];

		for (int c = 0; c < w->schema->ncols; c++)
			want[c] = spec->set[c] || w->pagg.pres[c];
		w->set = acc_new(w, want);
	}
	/* the superunit summaries are minor 12, which the caller may not have allowed (sift.col_format_minor) or the kinds of its rows forbid */
	w->pagg.super = spec->super >= SIFT_SUPER_MIN_FAN && w->setting >= SIFT_COL_FORMAT_SUPER ? Min(spec->super, SIFT_SUPER_MAX_FAN) : 0;
	if (w->pagg.super)
		w->format = SIFT_COL_FORMAT_SUPER;
	if (w->setting >= SIFT_COL_FORMAT_RUNS && w->run == NULL)
		w->run = acc_new(w, spec->run);
	/* the option is what stamps the minor 18: the setting stays at 15 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->hll == NULL)
		w->hll = acc_new(w, spec->hll);
	/* the key tables stamp minor 22 themselves, as the setting says: under 15 the segment stays readable by 3.3 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->kt == NULL && spec->key_table)
	{
		w->kt = sift_ktab_begin(w->schema, spec->set, w->mcxt);
	}
	/* the rows of each string are written with the order of its dictionary: the option stamps minor 24, the setting stays at 15 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->cnt == NULL)
		for (int c = 0; c < w->schema->ncols; c++)
			if (spec->count[c])
			{
				if (w->cnt == NULL)
					w->cnt = MemoryContextAllocZero(w->mcxt, w->schema->ncols * sizeof(CountAcc));
			}
	/* the point directories are written with the dictionaries, minor 14 */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && w->pt == NULL)
		for (int c = 0; c < w->schema->ncols; c++)
			if (spec->point[c] || spec->point_tid[c] || spec->gorder[c] || segw_elem_point(w, c))
			{
				MemoryContext old = MemoryContextSwitchTo(w->mcxt);

				if (w->pt == NULL)
					w->pt = palloc0(w->schema->ncols * sizeof(PointAcc));
				MemoryContextSwitchTo(old);
				w->pt[c].want = spec->point[c] || spec->point_tid[c] || segw_elem_point(w, c);
				w->pt[c].ord = spec->gorder[c] && w->setting >= SIFT_COL_FORMAT_PRESENCE;	/* stamps minor 21 itself, as the lists of addresses do 16 */
				w->pt[c].tids = spec->point_tid[c] && w->setting >= SIFT_COL_FORMAT_PRESENCE && !w->keyed;	/* the option is what stamps minor 16: the setting stays at 15 */
			}
}

/*
 * The units of a key-cut segment are not block ranges, so a heap address does not name its unit: its point directories list the rows of the segment
 *, whatever point_tid_columns says, and a lookup reads the unit of each row.
 */
void
sift_colw_set_keyed(SiftColSegWriter *w, uint32 aim_rows)
{
	SIFT_INVARIANT(w->nsb == 0, "sift_col: a segment is keyed before its first unit");
	w->keyed = true;
	w->aim_rows = aim_rows;
	if (w->pt)
		for (int c = 0; c < w->schema->ncols; c++)
			w->pt[c].tids = false;
}

/* the element of a list of strings (not a map): its directory lists the rows that own the value, so that an element test finds them without reading the units */
bool
segw_elem_point(const SiftColSegWriter *w, int c)
{
	const SiftColSchema *s = w->schema;
	const SiftColDef *d = &s->cols[c];

	return w->setting >= SIFT_COL_FORMAT_PRESENCE && d->parent >= 0 && d->kind == SIFT_COLK_SCALAR &&
		(d->store == SIFT_COLS_STR || d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_INT8) &&
		s->cols[d->parent].kind == SIFT_COLK_GROUP && strstr(s->cols[d->parent].path, ".*") == NULL && strstr(d->path, "#key") == NULL;
}

/* The leaf c of the segment gets a global order block sorted by (group, key) as well (the group of order_array_group: the build's own choice groups a time column only, and keeps the plain order beside it in the second slot; a group of the user's groups every block and replaces the plain order) */
bool
segw_gorder_grouped(const SiftColSegWriter *w, int c)
{
	return w->pagg.gorder_group > 0 && !sift_cgord_leaf_text(&w->schema->cols[c]) && (!w->pagg.gorder_group_auto || (w->setting >= SIFT_COL_FORMAT_PRESENCE && sift_cgord_leaf_time(&w->schema->cols[c])));
}

/* ... and the plain order too */
bool
segw_gorder_plain(const SiftColSegWriter *w, int c)
{
	return w->pagg.gorder_group == 0 || w->pagg.gorder_group_auto || sift_cgord_leaf_text(&w->schema->cols[c]);
}

void
sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b)
{
	int			ncols = w->schema->ncols;
	Size		start;
	uint64		off;

	if (!sift_cbatch_kinds_exact(b))
	{
		/* the callers lower the setting before they begin when a row of the segment may lack its kind: minor 7, 9, 10 and 12 promise complete kinds */
		SIFT_INVARIANT(!((w->format >= SIFT_COL_FORMAT_MINMAX && !(w->format == SIFT_COL_FORMAT_CALENDAR && !w->schema->kinds)) || w->set || w->run || w->hll),
					   "sift_col: a unit without its kinds in a segment of column format %d", w->run ? SIFT_COL_FORMAT_RUNS : w->set ? SIFT_COL_FORMAT_SETS : w->format);
		w->inexact = true;
		/* a schema without shredded leaves has no kinds to lose: the minor stays */
		if (w->format != SIFT_COL_FORMAT_CALENDAR || w->schema->kinds)
			w->format = sift_col_write_minor(w->schema, Min(w->format, SIFT_COL_FORMAT_TAGS), false);
	}
	Assert(b->nrows > 0 && b->schema->ncols == ncols);
	SIFT_INVARIANT(w->nsb == 0 || b->sb > w->sbdir[w->nsb - 1].id, "sift_col: superblock %u added after %u", b->sb,
				   w->nsb > 0 ? w->sbdir[w->nsb - 1].id : 0);
	if (w->nsb == w->capsb)
	{
		w->capsb *= 2;
		w->zones = repalloc_huge(w->zones, (Size) w->capsb * ncols * sizeof(SiftCZone));
		w->lagg = repalloc_huge(w->lagg, (Size) w->capsb * ncols * sizeof(SiftCLeafAgg));
		w->cubeoff = repalloc_huge(w->cubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->ecubeoff = repalloc_huge(w->ecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->xcubeoff = repalloc_huge(w->xcubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->xecubeoff = repalloc_huge(w->xecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		for (int c = 0; c < ncols; c++)
		{
			if (w->set && w->set[c].off)
				w->set[c].off = repalloc_huge(w->set[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
			if (w->run && w->run[c].off)
				w->run[c].off = repalloc_huge(w->run[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
			if (w->hll && w->hll[c].off)
				w->hll[c].off = repalloc_huge(w->hll[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
		}
		w->pmask = repalloc_huge(w->pmask, (Size) w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
		w->sbdir = repalloc_huge(w->sbdir, w->capsb * sizeof(SiftColSBRef));
		w->vunit = repalloc_huge(w->vunit, w->capsb * sizeof(VersUnit));
		memset(w->vunit + w->capsb / 2, 0, w->capsb / 2 * sizeof(VersUnit));
	}
	for (int c = 0; c < ncols; c++)
		w->inl[c] = w->dict[c] != NULL && sift_cbatch_str_inline(b, c, w->dict[c], SIFT_COL_DICT_MAX_BYTES);
	map_dicts(w, b, w->inl);
	count_rows(w, b);
	resetStringInfo(&w->buf);
	sift_cunit_encode(b, w->dictmap, w->inl, w->pol, w->format >= SIFT_COL_FORMAT_TAGS, w->keyed && w->setting >= SIFT_COL_FORMAT_PRESENCE, &w->buf, &start, w->zones + (Size) w->nsb * ncols);
	segw_add_preagg(w, b, w->buf.len - start);
	if (b->keyed)
	{
		/* a key-cut unit has no page mask: its slot holds the first and last block of the unit (its head), which a group of units tests against the visibility map at once */
		memset(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
		memcpy(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, w->buf.data + start + 4 * sizeof(uint32), 2 * sizeof(uint32));
	}
	else
		memcpy(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, w->buf.data + start + 4 * sizeof(uint32), SIFT_PAGEMASK_WORDS * sizeof(uint64));	/* after the unit's head */
	segw_add_vers(w, b);
	segw_sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, w->buf.data + start, w->buf.len - start);
	w->sbdir[w->nsb] = (SiftColSBRef) {b->sb, b->nrows, off, w->rows, w->buf.len - start, 0};
	if (w->nsb == 0)
		w->sb_lo = b->sb;
	w->sb_hi = b->sb;
	w->nsb++;
	w->rows += b->nrows;
}

uint64
sift_colw_rows(const SiftColSegWriter *w)
{
	return w->rows;
}

void
sift_colw_set_index_rows(SiftColSegWriter *w, uint64 rows)
{
	w->index_rows = rows;
}

uint64
sift_colw_bytes(const SiftColSegWriter *w)
{
	return sift_sw_tell(w->sw);
}

Size
sift_colw_memory(const SiftColSegWriter *w)
{
	Size		m = (Size) w->nsb * (w->schema->ncols * (sizeof(SiftCZone) + sizeof(SiftCLeafAgg)) + sizeof(SiftColSBRef) + 2 * sizeof(uint64)) + w->cube.len + w->ecube.len + w->xcube.len + w->xecube.len;

	for (int c = 0; c < w->schema->ncols; c++)
		if (w->dict[c])
			m += w->dict[c]->nbytes + (Size) w->dict[c]->n * 3 * sizeof(uint32);
	return m;
}

void
sift_colw_finish(SiftColSegWriter *w, SiftSegEntry *entry)
{
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);
	SiftSegHeaderData h = {0};
	StringInfoData schema;
	SiftExtent *ext;
	int			n;
	uint64		blocks;

	SIFT_INVARIANT(w->nsb != 0, "sift_col: a segment needs at least one unit");
	Assert(!w->anyver || w->format >= SIFT_COL_FORMAT_VERS);
	if (w->anysets)
		w->format = Max(w->format, w->anypres ? SIFT_COL_FORMAT_PRESENCE : SIFT_COL_FORMAT_SETS);
	if (w->anyrunl)
		w->format = Max(w->format, SIFT_COL_FORMAT_RUNL);
	if (w->anyruns)
		w->format = Max(w->format, w->anyheads ? SIFT_COL_FORMAT_HEADS : SIFT_COL_FORMAT_RUNS);
	if (w->anyexc)
		w->format = Max(w->format, SIFT_COL_FORMAT_EXCEPT);
	if (w->anyhll)
		w->format = Max(w->format, SIFT_COL_FORMAT_HLL);
	segw_build_points(w);
	if (w->anypoints)
		w->format = Max(w->format, w->setting >= SIFT_COL_FORMAT_PRESENCE ? SIFT_COL_FORMAT_EXACTSLOT : w->anytids ? SIFT_COL_FORMAT_POINTTID : SIFT_COL_FORMAT_DICTIDX);
	if (w->anypacked)
		w->format = Max(w->format, SIFT_COL_FORMAT_PACKED);
	if ((w->anypoints || w->anyord) && segw_rooted_ok(w))
		w->format = Max(w->format, SIFT_COL_FORMAT_ROOT);
	if (w->anyord)
		w->format = Max(w->format, sift_cgord_rows() == SIFT_CGORD_ROWS_FINE ? SIFT_COL_FORMAT_FINEORD : SIFT_COL_FORMAT_GORDER);
	if (w->anydual)
		w->format = Max(w->format, SIFT_COL_FORMAT_GROUPORD);
	if (w->anytextord)
		w->format = Max(w->format, SIFT_COL_FORMAT_TEXTORD);
	if (w->keyed)
		w->format = Max(w->format, w->setting >= SIFT_COL_FORMAT_PRESENCE ? SIFT_COL_FORMAT_TABFIRST : SIFT_COL_FORMAT_KEYED);
	if (w->schema->contain)
		w->format = Max(w->format, SIFT_COL_FORMAT_CONTAIN);
	if (w->anymeaslen)
		w->format = Max(w->format, SIFT_COL_FORMAT_MEASLEN);
	/* a dictionary of many strings is written with the order to be searched in place when the setting allows it, the leaf asks for it (spec.lookup) and the kinds are complete */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact)
	{
		Size		order_bytes = 0,		/* the orders kept so far, for the cap of a sift_table */
					seg_bytes = sift_sw_tell(w->sw);	/* the segment before its dictionaries, then with the compact form of them */

		w->dform = palloc0(w->schema->ncols * sizeof(uint8));
		if (w->pagg.lookup_pct != 0)
			for (int c = 0; c < w->schema->ncols; c++)
				if (w->dict[c] != NULL)
					seg_bytes += w->dict[c]->nbytes + ((Size) w->dict[c]->n + 1) * sizeof(uint32);
		for (int c = 0; c < w->schema->ncols; c++)
			if (w->dict[c] != NULL && w->pagg.lookup[c] && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN) &&
				(w->pagg.lookup_pct == 0 || (order_bytes + (Size) w->dict[c]->n * sizeof(uint32)) * 100 <= (Size) w->pagg.lookup_pct * seg_bytes))
			{
				order_bytes += (Size) w->dict[c]->n * sizeof(uint32);
				w->dform[c] = DICT_LOOKUP;
				w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
				if (w->setting >= SIFT_COL_FORMAT_PRESENCE)
				{
					w->dict[c]->hashed = true;
					w->format = Max(w->format, SIFT_COL_FORMAT_HASH);
				}
				if (w->cnt != NULL && w->pagg.count[c] && !w->cnt[c].bad && w->cnt[c].cap >= w->dict[c]->n)
				{
					w->dict[c]->rows = w->cnt[c].rows;
					w->format = Max(w->format, SIFT_COL_FORMAT_COUNT);
				}
			}
	}
	h.sections[SIFT_SEC_POSTINGS] = (SiftSectionRef) {0, sift_sw_tell(w->sw)};
	segw_write_dicts(w, &h.sections[SIFT_SEC_DICT]);
	initStringInfo(&w->ktab);
	if (w->kt)
		/* 3% of the segment as written so far: the chunks and the dictionaries */
		sift_ktab_finish(w->kt, sift_sw_tell(w->sw), &w->ktab);
	if (w->ktab.len > 0)
		w->format = Max(w->format, SIFT_COL_FORMAT_KTAB);
	segw_write_zones(w, &h.sections[SIFT_SEC_DICTIDX]);
	initStringInfo(&schema);
	sift_cschema_serialize(w->schema, &schema);
	segw_sw_pad8(w->sw);
	h.sections[SIFT_SEC_DOCMAP].off = sift_sw_tell(w->sw);
	h.dictroot_off = schema.len;
	sift_sw_write(w->sw, schema.data, schema.len);
	segw_put8(w, w->sbdir, (Size) w->nsb * sizeof(SiftColSBRef));
	segw_sw_pad8(w->sw);
	if (w->anyver)
		segw_write_vers(w);
	h.sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(w->sw) - h.sections[SIFT_SEC_DOCMAP].off;
	/* the leaf statistics follow the last section, where no reader of an older minor looks (the directory entry says they are there); 3% of the segment at most */
	if (w->lst != NULL && w->lstlen * sift_limit(lst_share_div, LST_SHARE_DIV) <= sift_sw_tell(w->sw))
	{
		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, w->lst, w->lstlen);
	}
	else
		w->lst = NULL;
	ext = sift_sw_finish(w->sw, &n);
	sift_live_write(w->wt, w->gen, w->sb_hi - w->sb_lo + 1, w->rows, &h.live_first, &h.live_npages);
	h.gen = w->gen;
	h.ndocs = w->rows;
	h.nterms = w->schema->ncols;
	h.sb_lo = w->sb_lo;
	h.sb_hi = w->sb_hi;
	h.dictroot_n = w->nsb;
	h.nextents = n;
	sift_page_init(page.data, SIFT_PT_SEGHEADER, w->gen);
	memcpy(hdr, &h, offsetof(SiftSegHeaderData, extents));
	memcpy(hdr->extents, ext, n * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + n * sizeof(SiftExtent));
	blocks = 1 + h.live_npages;
	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = w->gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->format = w->format;
	entry->header_blk = sift_wt_write_page(w->wt, page.data);
	entry->sb_lo = w->sb_lo;
	entry->sb_hi = w->sb_hi;
	if (w->anyver)
	{
		entry->flags |= SIFT_SEG_VERS;
		entry->reserved = w->vmin;
	}
	if (w->keyed)
		entry->flags |= SIFT_SEG_KEYED;
	if (w->lst != NULL)
		entry->flags |= SIFT_SEG_LSTATS;
	entry->ndocs = w->rows;
	entry->bytes = blocks * BLCKSZ;
	pfree(schema.data);
}
