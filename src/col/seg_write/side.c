/*
 * col/seg_write/side.c - the scope sidecar of an entity segment (col/side.h), made from the segment's finished units and the pairs of its scope
 *
 * Group by group: for one scope key the members come in ascending order, the units that hold them are found by the zone of the member leaf (the units of a segment cut by the member
 * follow it), and the key's rows in them, at most SIDE_RUN_UNITS units and SIDE_RUN_ROWS rows at a time, are decoded for the leaves the cube reads and cut by calendar month. The cube
 * of the rows of each month is what a unit's cube would be had the unit held those rows alone; the cube of all of them is the merge of the months. No row is kept and nothing is sorted.
 *
 * The bytes go to the segment's pages in the order they are made (SideSink): a run's cubes, its cells and its units when the run is closed, so the offsets in them are
 * known as they are put and nothing is patched, and the keys and the runs of a scope when it ends; the records of the scopes, the header and the trailer end the sidecar. No temporary file is written.
 *
 * A text, varchar or uuid member has no zone to find units by: the members of the pairs are codes of a map of the statement (sift_scope_all_pairs), the strings of the member leaf of each
 * unit (its dictionary's, or its own when it stores them inline) are looked up in that map, and one pass over the units makes the index from a member to its units (8 bytes for each member
 * of each unit, at most maintenance_work_mem: a segment that needs more has no sidecar and is read by reference). A run then keeps, besides its cubes, a Bloom filter of the journal words
 * of every member its units hold, which is what the journal's changed members are tested against (col/side_valid.c).
 */
/* check-memory: allow-allocs the rows of one run (at most five eighths of maintenance_work_mem: side_run_rows), the buffer of the sidecar's pages (a quarter: side_sink_bytes) and the tables of a scope (a quarter at most: side_tables_bytes) */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"
#include "col/am.h"
#include "col/seg_write/side_int.h"
#include "core/limits.h"
#include "core/membudget.h"

#define SIDE_ZEROS	1024

/* The sidecar of a segment: the bodies of its scopes are put as they are made, then the records, the header and the trailer */
struct SiftSideImage
{
	SideSink	sink;
	uint32		nsb;
	uint32		nscope;			/* the scopes it is made for */
	uint32		n;				/* ... those added */
	uint64		at;
	MemoryContext cx;
	SideScope	sc[SIFT_SCOPE_MAX];
};

static inline uint32
words(uint32 n)
{
	return SIFT_COL_NWORDS(n) + 1;
}

/* The bytes of the sidecar that wait in memory, a quarter of maintenance_work_mem (the buffer is one allocation of that size) */
static Size
side_sink_bytes(void)
{
	return Min(Min((Size) maintenance_work_mem * 1024 / 4, (Size) sift_limit(side_sink_max, SIFT_ARRAY_UNLIMITED)), MaxAllocSize);
}

/* What the tables of a scope may hold, an eighth of maintenance_work_mem (their buffers double: a quarter) */
static Size
side_tables_bytes(void)
{
	return (Size) maintenance_work_mem * 1024 / 8;
}

/* Puts n bytes to the sidecar; returns the offset they are at */
uint64
side_put(SideSink *k, const void *p, Size n)
{
	uint64		at = k->pos;

	k->pos += n;
	if (k->sw == NULL)
	{
		if (k->len + n <= k->cap)
		{
			memcpy(k->buf + k->len, p, n);
			k->len += n;
			return at;
		}
		{
			MemoryContext old = MemoryContextSwitchTo(k->cx);

			k->sw = sift_sw_begin(k->wt, k->gen);
			MemoryContextSwitchTo(old);
		}
		sift_sw_write(k->sw, k->buf, k->len);
		pfree(k->buf);
		k->buf = NULL;
	}
	sift_sw_write(k->sw, p, n);
	return at;
}

static void
put_zeros(SideSink *k, uint64 n)
{
	char		zeros[SIDE_ZEROS] = {0};

	for (; n > 0; n -= Min(n, sizeof(zeros)))
		side_put(k, zeros, Min(n, sizeof(zeros)));
}

/* The bytes a row of the open run takes: its value in each leaf and in the time, the null and overflow bits of those, its month and its place in the order of the months; and while the cube of a month
 * is made, the rows of the month again (the one with most rows holds them all) and a group and a position for each, and the day and the cell of a calendar dim */
static Size
side_row_bytes(int nleaf)
{
	return (nleaf + 1) * sizeof(int64) + (4 * nleaf + 8) / 8 + 2 * sizeof(int32) + nleaf * sizeof(int64) + 2 * sizeof(uint32) + 2 * sizeof(int64);
}

/* The rows of a run: what five eighths of maintenance_work_mem hold of them, but a run holds whole units (the largest of the segment) and at most SIDE_RUN_ROWS */
static uint32
side_run_rows(int nleaf, uint32 max_unit_rows)
{
	return (uint32) Min((Size) SIDE_RUN_ROWS, Max((Size) max_unit_rows, (Size) maintenance_work_mem * 1024 / 8 * 5 / side_row_bytes(nleaf)));
}

static void
rows_init(SideRows *r, const SiftColSchema *s, int time, const int *leaf, int nleaf, uint32 max_unit_rows)
{
	Size		pad = SIFT_COL_PAD / sizeof(int64);

	r->cap = side_run_rows(nleaf, max_unit_rows);
	r->nleaf = nleaf;
	memcpy(r->leaf, leaf, nleaf * sizeof(int));
	r->vals = palloc(nleaf * sizeof(int64 *));
	r->nulls = palloc(nleaf * sizeof(uint64 *));
	r->ovf = palloc(nleaf * sizeof(uint64 *));
	r->anynull = palloc0(nleaf * sizeof(bool));
	r->anyovf = palloc0(nleaf * sizeof(bool));
	r->ready = palloc(nleaf * sizeof(bool));
	for (int i = 0; i < nleaf; i++)
	{
		r->vals[i] = palloc((r->cap + pad) * sizeof(int64));
		r->nulls[i] = palloc0(words(r->cap) * sizeof(uint64));
		r->ovf[i] = palloc0(words(r->cap) * sizeof(uint64));
		r->ready[i] = true;
	}
	r->time = palloc((r->cap + pad) * sizeof(int64));
	r->tnull = palloc0(words(r->cap) * sizeof(uint64));
	r->tmp = palloc((max_unit_rows + pad) * sizeof(int64));
	r->tmpnull = palloc(words(max_unit_rows) * sizeof(uint64));
	r->tmpovf = palloc(words(max_unit_rows) * sizeof(uint64));
	r->sel = palloc(max_unit_rows * sizeof(uint32));
	r->cellix = palloc(r->cap * sizeof(int32));
}

static void
rows_reset(SideRows *r)
{
	for (int i = 0; i < r->nleaf; i++)
	{
		if (r->anynull[i])
			memset(r->nulls[i], 0, words(r->rows) * sizeof(uint64));
		if (r->anyovf[i])
			memset(r->ovf[i], 0, words(r->rows) * sizeof(uint64));
		r->anynull[i] = r->anyovf[i] = false;
		r->ready[i] = true;
	}
	if (r->tany)
		memset(r->tnull, 0, words(r->rows) * sizeof(uint64));
	r->tany = false;
	r->rows = 0;
	r->ncells = 0;
	r->cells_over = false;
}

/* The month cell of a time key (the first day of its month, since 2000-01-01), SIDE_CELL_NONE for one no calendar cell holds */
static int64
cell_of(bool date, int type, int64 key)
{
	int64		day = date ? key : (key >= 0 ? key / USECS_PER_DAY : -((-key + USECS_PER_DAY - 1) / USECS_PER_DAY));

	if (key == PG_INT64_MIN || key == PG_INT64_MAX || !sift_cpagg_cal_key_ok(type, SIFT_CAL_DAY, day))
		return SIDE_CELL_NONE;
	return sift_cpagg_cal_cell(SIFT_CAL_MONTH, day);
}

/* The id of a month in the run (ids are in order of first sight and stay; order[] lists them by month); -1 when the run has more months than it keeps */
static int32
cell_index(SideRows *r, int64 cell)
{
	uint32		lo = 0,
				hi = r->ncells;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (r->cells[r->order[mid]] < cell)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < r->ncells && r->cells[r->order[lo]] == cell)
		return (int32) r->order[lo];
	if (r->ncells == SIDE_MAX_CELLS)
	{
		r->cells_over = true;
		return -1;
	}
	memmove(r->order + lo + 1, r->order + lo, (r->ncells - lo) * sizeof(uint16));
	r->order[lo] = (uint16) r->ncells;
	r->cells[r->ncells] = cell;
	return (int32) r->ncells++;
}

/*
 * The rows of unit u that belong to the key (members m[0..nm), ascending) go to the open run; false: none does. A unit whose member zone is made of members alone and holds no NULL
 * is taken whole, else the member leaf is decoded and its rows tested, once for each value (the rows of a member follow each other in a unit cut by it). A row with no member is no key's;
 * a unit of such rows only takes the place between its neighbours in the zones.
 */
static bool
add_unit(SiftSideBuild *b, uint32 u, const SiftScopePair *m, uint64 nm, uint32 *impure)
{
	SideRows   *r = &b->rows;
	SiftCUnitView v;
	uint32		n,
				nsel = 0;
	const uint64 *nulls,
			   *ovf;
	const int64 *mem = r->tmp;
	const MemberCodes *mc = NULL;
	int64		last = PG_INT64_MIN;
	uint32		words_before = b->nwords;
	bool		in = false,
				whole;

	SIFT_INVARIANT(sift_cseg_unit_view(b->seg, u, &v), "the scope sidecar cannot parse a unit");
	n = v.nrows;
	if (b->mm != NULL)
		mc = side_unit_members(b, u, &v, &nulls);
	else
		SIFT_INVARIANT(sift_side_unit_leaf(b->seg, u, &v, b->member, r->tmp, &nulls, &ovf), "the member leaf of the scope sidecar is stored inline");
	for (uint32 i = 0; i < n; i++)
	{
		if (nulls != NULL && sift_col_bit_test(nulls, i))
			continue;
		if (i == 0 || mem[i] != last)
		{
			last = mem[i];
			in = side_member_in(m, nm, mc != NULL ? mc->member[last] : last);
			if (mc != NULL)
				b->words[b->nwords++] = mc->word[last];
		}
		if (in)
			r->sel[nsel++] = i;
	}
	if (mc != NULL && mc != b->segmap)
		MemoryContextReset(b->ucx);
	whole = nsel == n;
	*impure += !whole;
	if (nsel == 0)
	{
		b->nwords = words_before;
		return false;
	}
	SIFT_INVARIANT(r->rows + nsel <= r->cap, "a run of the scope sidecar holds more rows than it may");
	for (int i = 0; i <= r->nleaf; i++)
	{
		bool		time = i == r->nleaf;
		int			leaf = time ? b->time : r->leaf[i];
		int64	   *dst = time ? r->time : r->vals[i];
		uint64	   *dnull = time ? r->tnull : r->nulls[i];
		bool		ok;

		if (whole)
		{
			ok = sift_side_unit_leaf(b->seg, u, &v, leaf, dst + r->rows, &nulls, &ovf);
			if (!ok)
				memset(dst + r->rows, 0, n * sizeof(int64));
		}
		else
		{
			ok = sift_side_unit_leaf(b->seg, u, &v, leaf, r->tmp, &nulls, &ovf);
			for (uint32 j = 0; j < nsel; j++)
				dst[r->rows + j] = ok ? r->tmp[r->sel[j]] : 0;
		}
		if (!time)
			r->ready[i] &= ok;
		if (nulls != NULL)
		{
			if (whole)
				seg_bits_put(dnull, r->rows, nulls, n);
			else
				for (uint32 j = 0; j < nsel; j++)
					if (sift_col_bit_test(nulls, r->sel[j]))
						dnull[(r->rows + j) / 64] |= UINT64CONST(1) << ((r->rows + j) % 64);
			if (time)
				r->tany = true;
			else
				r->anynull[i] = true;
		}
		if (!time && ovf != NULL)
		{
			if (whole)
				seg_bits_put(r->ovf[i], r->rows, ovf, n);
			else
				for (uint32 j = 0; j < nsel; j++)
					if (sift_col_bit_test(ovf, r->sel[j]))
						r->ovf[i][(r->rows + j) / 64] |= UINT64CONST(1) << ((r->rows + j) % 64);
			r->anyovf[i] = true;
		}
	}
	{
		int64		lastday = PG_INT64_MIN,
					cell = SIDE_CELL_NONE;
		int32		ix = -1;
		bool		have = false;

		for (uint32 j = 0; j < nsel; j++)
		{
			uint32		row = r->rows + j;

			if (r->tany && sift_col_bit_test(r->tnull, row))
				cell = SIDE_CELL_NONE, have = false;
			else if (!have || r->time[row] != lastday)
			{
				lastday = r->time[row];
				cell = cell_of(b->time_date, b->schema->cols[b->time].type, lastday);
				have = true;
			}
			r->cellix[row] = -1;
			if (!r->cells_over)
			{
				if (ix < 0 || r->cells[ix] != cell)
					ix = cell_index(r, cell);
				r->cellix[row] = ix;
			}
		}
	}
	r->rows += nsel;
	return true;
}

/* The cube of rows [from, to) of the open run, or of all of them when order is NULL, appended to the cube area as a cell */
static void
put_cell(SiftSideBuild *b, int64 cell, const uint32 *order, uint32 from, uint32 to, SideCell *out)
{
	SideRows   *r = &b->rows;
	SiftCPaggCol *cols = palloc0(b->schema->ncols * sizeof(SiftCPaggCol));
	SiftCPaggSpec spec = b->spec;
	uint32		n = to - from;
	StringInfoData cube;
	int64	  **v = palloc(r->nleaf * sizeof(int64 *));
	uint64	  **nl = palloc(r->nleaf * sizeof(uint64 *));
	uint64	  **ov = palloc(r->nleaf * sizeof(uint64 *));

	initStringInfo(&cube);
	for (int i = 0; i < r->nleaf; i++)
	{
		if (order == NULL)
		{
			v[i] = r->vals[i];
			nl[i] = r->anynull[i] ? r->nulls[i] : NULL;
			ov[i] = r->anyovf[i] ? r->ovf[i] : NULL;
		}
		else
		{
			v[i] = palloc(n * sizeof(int64) + SIFT_COL_PAD);
			nl[i] = r->anynull[i] ? palloc0(words(n) * sizeof(uint64)) : NULL;
			ov[i] = r->anyovf[i] ? palloc0(words(n) * sizeof(uint64)) : NULL;
			for (uint32 j = 0; j < n; j++)
			{
				uint32		row = order[from + j];

				v[i][j] = r->vals[i][row];
				if (nl[i] != NULL && sift_col_bit_test(r->nulls[i], row))
					nl[i][j / 64] |= UINT64CONST(1) << (j % 64);
				if (ov[i] != NULL && sift_col_bit_test(r->ovf[i], row))
					ov[i][j / 64] |= UINT64CONST(1) << (j % 64);
			}
		}
		cols[r->leaf[i]] = (SiftCPaggCol) {v[i], nl[i], ov[i], n, r->ready[i]};
	}
	spec.minmax = order == NULL;	/* the cube of all the rows keeps the minimum and maximum of each group, a month's does not: they are a third of its bytes */
	sift_cpagg_cube(b->schema, &spec, cols, n, (Size) b->row_bytes * n, &cube, NULL);
	*out = (SideCell) {.cell = cell,.rows = n,.len = cube.len,.off = side_put(b->sink, cube.data, cube.len)};
	b->cnt->cell_bytes += cube.len;
	SIFT_MEM_CHECKPOINT();
	if (order != NULL)
		for (int i = 0; i < r->nleaf; i++)
		{
			pfree(v[i]);
			if (nl[i])
				pfree(nl[i]);
			if (ov[i])
				pfree(ov[i]);
		}
	pfree(cube.data);
	pfree(cols);
	pfree(v);
	pfree(nl);
	pfree(ov);
}

/* Is there room in the tables of the scope for the record of one more run and its key? */
static bool
tables_room(SiftSideBuild *b)
{
	b->tables_full = b->tables_full || b->keys.len + b->recs.len + b->runs.len + sizeof(int64) + sizeof(SideKey) + sizeof(SideRun) > b->tables_cap;
	return !b->tables_full;
}

/* The unit list of a run, the first unit and then the gaps as varints, put to the sidecar */
static uint64
put_units(SiftSideBuild *b)
{
	uint8		buf[SIDE_RUN_UNITS * 5];
	Size		n = 0;
	uint32		prev = 0;

	for (uint32 i = 0; i < b->nunits; i++)
	{
		uint64		v = i == 0 ? b->units[0] : b->units[i] - prev;

		for (; v >= 128; v >>= 7)
			buf[n++] = (uint8) (v | 128);
		buf[n++] = (uint8) v;
		prev = b->units[i];
	}
	return side_put(b->sink, buf, n);
}

/* The open run is complete: its cubes, its cells and its units are put to the sidecar and its record waits for the end of the scope */
static void
close_run(SiftSideBuild *b, uint32 *nruns)
{
	SideRows   *r = &b->rows;
	SideRun		run = {.lo = b->lo,.hi = b->hi,.rows = r->rows,.nunits = b->nunits,.first = b->units[0]};
	MemoryContext old;

	if (b->nunits == 0 || (b->nunits < 2 && b->mm == NULL) || !tables_room(b))
	{
		/* one unit is read by reference as cheaply as the sidecar would read it; the runs of a string member list every unit of the key, which is how a read skips the others */
		b->nunits = 0;
		b->nwords = 0;
		rows_reset(r);
		return;
	}
	if (b->mm != NULL)
		side_put_filter(b, &run);
	else
	{
		b->pmax = *nruns == 0 ? b->hi : Max(b->pmax, b->hi);
		run.pmax = b->pmax;
	}
	old = MemoryContextSwitchTo(b->work);
	put_cell(b, SIDE_CELL_ALL, NULL, 0, r->rows, &b->cellrec[0]);
	run.ncells = 1;
	if (!r->cells_over && r->ncells > 0)
	{
		uint32	   *order = palloc(r->rows * sizeof(uint32));
		uint32	   *start = palloc0((r->ncells + 1) * sizeof(uint32));

		for (uint32 i = 0; i < r->rows; i++)
			start[r->cellix[i] + 1]++;
		for (uint32 c = 0; c < r->ncells; c++)
			start[c + 1] += start[c];
		{
			uint32	   *at = palloc(r->ncells * sizeof(uint32));

			memcpy(at, start, r->ncells * sizeof(uint32));
			for (uint32 i = 0; i < r->rows; i++)
				order[at[r->cellix[i]]++] = i;
			pfree(at);
		}
		for (uint32 k = 0; k < r->ncells; k++)
		{
			uint32		c = r->order[k];

			if (start[c + 1] > start[c])
				put_cell(b, r->cells[c], order, start[c], start[c + 1], &b->cellrec[run.ncells++]);
		}
		pfree(order);
		pfree(start);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(b->work);
	run.cells_off = side_put(b->sink, b->cellrec, run.ncells * sizeof(SideCell));
	run.units_off = put_units(b);
	appendBinaryStringInfo(&b->runs, (char *) &run, sizeof(run));
	b->cnt->runs++;
	b->cnt->units += b->nunits;
	b->cnt->rows += r->rows;
	b->cnt->cells += run.ncells;
	++*nruns;
	b->nunits = 0;
	b->nwords = 0;
	rows_reset(r);
	SIFT_MEM_CHECKPOINT();
}

static uint32
first_unit(const int64 *zmax, uint32 nsb, uint32 from, int64 m)
{
	uint32		lo = from,
				hi = nsb;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (zmax[mid] < m)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* The units hold their members in order: each starts at or after the end of the one before it */
static bool
member_ordered(const int64 *zmin, const int64 *zmax, uint32 nsb)
{
	for (uint32 u = 1; u < nsb; u++)
		if (zmin[u] < zmax[u - 1])
			return false;
	return true;
}

/*
 * The dims of the spec that the units of the segment keep a histogram of with at most SIDE_DIM_GROUPS groups (looked at in a few units): a month of a run is summarised by the dims that
 * have few groups, the others (a key, a measure's own values) answer nothing a group of rows needs and cost the most to build. The leaves the cube reads follow the dims.
 */
#define SIDE_DIM_GROUPS	256

static int
side_dims(SiftColSegment *seg, SiftCPaggSpec *spec, int *leaf)
{
	bool		keep[SIFT_PAGG_MAX_DIMS] = {false};
	bool		seen[SIFT_COL_MAX_COLS] = {false};
	int			nleaf = 0,
				nd = 0;

	for (uint32 q = 0; q < 3; q++)
	{
		uint32		len,
					u = seg->nsb * (2 * q + 1) / 6;
		const uint8 *blob = sift_cseg_cube(seg, u, &len);
		SiftCHist	h;

		if (blob == NULL || !sift_cpagg_decode(seg->schema, blob, len, CurrentMemoryContext, &h))
			continue;
		for (int d = 0; d < spec->ndims; d++)
			for (int k = 0; k < h.ndims; k++)
				if (h.dim[k].leaf == spec->dim[d].leaf && h.dim[k].ngroups <= SIDE_DIM_GROUPS && h.dim[k].ngroups * SIFT_PAGG_SCOPE_DIM_ROWS <= seg->sbdir[u].nrows)
					keep[d] = true;
	}
	for (int d = 0; d < spec->ndims; d++)
		if (keep[d])
		{
			spec->dim[nd] = spec->dim[d];
			seen[spec->dim[nd].leaf] = true;
			for (int m = 0; m < spec->dim[nd].nmeas; m++)
				seen[spec->dim[nd].meas[m]] = true;
			nd++;
		}
	spec->ndims = nd;
	spec->nrollups = 0;
	for (int c = 0; c < seg->schema->ncols; c++)
		if (seen[c])
			leaf[nleaf++] = c;
	return nleaf;
}

/* A scope of the segment that has no run to keep (its members are not in the order of the units, or no pair has a member in it): the segment has been looked at, and the scope reads it by reference */
static void
side_empty(SiftSideImage *im, const SideScopeInput *in)
{
	uint64		at = im->sink.pos;

	im->sc[im->n] = (SideScope) {.ordinal = in->scope,.member_kind = in->mm != NULL ? SIDE_MEMBER_WORDS : SIDE_MEMBER_RANGE,.keys_off = at,.recs_off = at,.runs_off = at};
	memcpy(im->sc[im->n].stamp, in->stamp, sizeof(im->sc[im->n].stamp));
}

SiftSideImage *
sift_side_image_begin(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint32 nscope, uint64 at)
{
	SiftSideImage *im = palloc0(sizeof(SiftSideImage));

	im->nsb = nsb;
	im->nscope = nscope;
	im->at = at;
	im->cx = CurrentMemoryContext;
	im->sink = (SideSink) {.wt = wt,.cx = CurrentMemoryContext,.gen = gen,.cap = side_sink_bytes()};
	im->sink.buf = palloc(im->sink.cap);
	return im;
}

/* The sidecar ends with the padding of its last page, the records of the scopes, the header and the trailer; its stream is then as long as it is */
SiftExtent *
sift_side_image_finish(SiftSideImage *im, SideCounts *cnt, int *nextents)
{
	SideSink   *k = &im->sink;
	uint64		len = k->pos,
				head = im->n * sizeof(SideScope) + sizeof(SideHeader),
				total = SIFT_PAYLOAD_BYTES(SIFT_PAYLOAD_PAGES(len + head + SIDE_TRAILER));
	SideHeader	h = {.magic = SIDE_MAGIC,.version = SIDE_VERSION,.nsb = im->nsb,.nscope = im->n,.scope_off = total - SIDE_TRAILER - head,.len = len};
	uint64		trailer[2] = {SIDE_MAGIC, im->at};

	SIFT_INVARIANT(im->n == im->nscope, "the scope sidecar of a segment lacks a scope");
	put_zeros(k, h.scope_off - len);
	side_put(k, im->sc, im->n * sizeof(SideScope));
	side_put(k, &h, sizeof(h));
	side_put(k, trailer, sizeof(trailer));
	SIFT_INVARIANT(k->pos == total, "the scope sidecar does not end its last page");
	if (k->sw == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(k->cx);

		sift_wt_expect_exact(k->wt, SIFT_PAYLOAD_PAGES(total));
		k->sw = sift_sw_begin(k->wt, k->gen);
		MemoryContextSwitchTo(old);
		sift_sw_write(k->sw, k->buf, k->len);
		pfree(k->buf);
	}
	cnt->bytes += total;
	return sift_sw_finish(k->sw, nextents);
}

/* The unit u is taken into the key's open run, which is closed first when it has no room for it */
static void
take_unit(SiftSideBuild *b, uint32 u, const SiftScopePair *m, uint64 nm, uint32 *impure, uint32 *nruns)
{
	if (b->nunits == SIDE_RUN_UNITS || (b->nunits > 0 && b->rows.rows + b->seg->sbdir[u].nrows > b->rows.cap))
		close_run(b, nruns);
	if (b->nunits == 0)
	{
		b->lo = PG_INT64_MAX;
		b->hi = PG_INT64_MIN;
	}
	if (add_unit(b, u, m, nm, impure))
	{
		b->units[b->nunits++] = u;
		if (b->zmin != NULL)
		{
			b->lo = Min(b->lo, b->zmin[u]);
			b->hi = Max(b->hi, b->zmax[u]);
		}
	}
}

/* The distinct keys of the pairs (ascending by key) */
static uint32
count_keys(const SiftScopePair *pairs, uint64 npairs)
{
	uint32		n = 0;

	for (uint64 i = 0; i < npairs; i++)
		n += i == 0 || pairs[i].key != pairs[i - 1].key;
	return n;
}

/* The run records of the scope passed their share at the key after the first kept ones: it keeps the runs it got in, the keys after it have none, and their units are read by reference */
static void
notice_cut(const SideScopeInput *in, uint32 kept, uint32 cut, Size tables_cap)
{
	ereport(NOTICE, (errmsg("scope \"%s\": the sidecar of a segment keeps the runs of the first %u of %u keys", in->name, kept, kept + cut),
					 errdetail("The run records of a scope take an eighth of maintenance_work_mem (%zu bytes now); the units of the other keys are read by reference, or the join runs where that costs less.", tables_cap),
					 errhint("Raise maintenance_work_mem and REINDEX the index.")));
}

/* The keys, their records and the runs of a scope, which waited for its end */
static void
put_tables(SiftSideBuild *b, SideScope *sc, uint32 nkeys, uint32 nruns)
{
	*sc = (SideScope) {.nkeys = nkeys,.nruns = nruns};
	sc->keys_off = side_put(b->sink, b->keys.data, b->keys.len);
	sc->recs_off = side_put(b->sink, b->recs.data, b->recs.len);
	sc->runs_off = side_put(b->sink, b->runs.data, b->runs.len);
}

static void
side_scope_body(SiftSideImage *im, SiftColSegment *seg, const SiftColRel *r, const SideScopeInput *in, SideCounts *cnt)
{
	const SiftScopePair *pairs = in->pairs;
	uint64		npairs = in->npairs;
	SiftKeyMap *mm = in->mm;
	int			scope = in->scope;
	SiftSideBuild b = {0};
	uint32		nsb = seg->nsb;
	int64	   *zmin = palloc(nsb * sizeof(int64)),
			   *zmax = palloc(nsb * sizeof(int64));
	int			leaf[SIFT_COL_MAX_COLS];
	int			nleaf;
	uint32		nkeys = 0,
				nseen = 0,
				nruns_all = 0,
				impure = 0;
	uint64	   *mu = NULL;
	uint64		nmu = 0,
				gi = 0;

	b.seg = seg;
	b.schema = seg->schema;
	b.cnt = cnt;
	b.mm = mm;
	b.sink = &im->sink;
	b.tables_cap = side_tables_bytes();
	b.member = r->first[r->schema->cols[r->scope_leaf[scope]].attno];
	b.time = sift_scope_time_leaf(r->schema->cols, r->schema->ncols);
	b.time_date = b.schema->cols[b.time].type == SIFT_COLT_DATE;
	for (uint32 u = 0; mm == NULL && u < nsb; u++)
	{
		SiftCZone	z;

		sift_cseg_zone(seg, b.member, u, &z);
		if (z.nvals > 0 && z.nvals == z.nnull)
		{
			zmin[u] = zmax[u] = u > 0 ? zmax[u - 1] : PG_INT64_MIN;
			continue;
		}
		if (z.min > z.max)
		{
			side_empty(im, in);
			return;
		}
		zmin[u] = z.min;
		zmax[u] = z.max;
	}
	if (mm == NULL && !member_ordered(zmin, zmax, nsb))
	{
		side_empty(im, in);
		return;
	}
	b.zmin = mm == NULL ? zmin : NULL;
	b.zmax = mm == NULL ? zmax : NULL;
	segw_group_spec(seg->schema, &r->pagg, &b.spec, &b.calspec, leaf);
	nleaf = side_dims(seg, &b.spec, leaf);
	if (b.spec.ndims == 0)
	{
		side_empty(im, in);
		return;
	}
	b.spec.dim_rows = SIFT_PAGG_SCOPE_DIM_ROWS;
	b.row_bytes = (double) seg->entry.bytes / (double) Max(seg->entry.ndocs, 1);
	b.work = AllocSetContextCreate(CurrentMemoryContext, "sift col side work", ALLOCSET_DEFAULT_SIZES);
	rows_init(&b.rows, seg->schema, b.time, leaf, nleaf, side_max_unit_rows(seg));
	if (mm != NULL)
	{
		b.ucx = AllocSetContextCreate(CurrentMemoryContext, "sift col side unit", ALLOCSET_DEFAULT_SIZES);
		b.key_us = palloc(nsb * sizeof(uint32));
		b.key_seen = palloc0(SIFT_COL_NWORDS(nsb) * sizeof(uint64));
		b.words = palloc(SIDE_RUN_UNITS * side_max_unit_rows(seg) * sizeof(uint64));
		b.segmap = side_member_codes(mm, sift_cseg_dict(seg, b.member), CurrentMemoryContext);
		mu = side_member_units(&b, &nmu);
		if (mu == NULL)
		{
			side_empty(im, in);
			return;
		}
		b.filter_at = hash_create("sift side filters", 1024, &(HASHCTL) {.keysize = sizeof(((FilterAt *) 0)->units),.entrysize = sizeof(FilterAt)}, HASH_ELEM | HASH_BLOBS);
	}
	initStringInfo(&b.keys);
	initStringInfo(&b.recs);
	initStringInfo(&b.runs);
	while (gi < npairs && !b.tables_full)
	{
		uint64		gj = gi;
		int64		key = pairs[gi].key;
		uint32		first_run = nruns_all,
					nruns = 0;

		while (gj < npairs && pairs[gj].key == key)
			gj++;
		nseen++;
		b.nunits = 0;
		b.nwords = 0;
		if (mm != NULL)
		{
			uint32		nu;
			uint32	   *us = side_key_units(&b, mu, nmu, pairs + gi, gj - gi, &nu);

			for (uint32 i = 0; i < nu; i++)
				take_unit(&b, us[i], pairs + gi, gj - gi, &impure, &nruns);
		}
		else
		{
			int64		last_added = -1;

			for (uint64 k = gi; k < gj; k++)
			{
				int64		mv = pairs[k].member;
				uint32		u = first_unit(zmax, nsb, 0, mv);

				if ((int64) u <= last_added)
					u = (uint32) (last_added + 1);
				for (; u < nsb && zmin[u] <= mv; u++)
				{
					last_added = u;
					take_unit(&b, u, pairs + gi, gj - gi, &impure, &nruns);
				}
			}
		}
		close_run(&b, &nruns);
		if (nruns > 0)
		{
			SideKey		kr = {first_run, nruns};

			appendBinaryStringInfo(&b.keys, (char *) &key, sizeof(key));
			appendBinaryStringInfo(&b.recs, (char *) &kr, sizeof(kr));
			nruns_all += nruns;
			nkeys++;
		}
		gi = gj;
	}
	if (b.tables_full)
		notice_cut(in, nseen - 1, 1 + count_keys(pairs + gi, npairs - gi), b.tables_cap);
	cnt->keys += nkeys;
	cnt->impure_units += impure;
	if (nruns_all == 0)
		side_empty(im, in);
	else
	{
		put_tables(&b, &im->sc[im->n], nkeys, nruns_all);
		im->sc[im->n].ordinal = scope;
		im->sc[im->n].member_kind = mm != NULL ? SIDE_MEMBER_WORDS : SIDE_MEMBER_RANGE;
		memcpy(im->sc[im->n].stamp, in->stamp, sizeof(im->sc[im->n].stamp));
	}
}

/* One scope's body is made in a context of its own, so that the memory of a sidecar of several scopes is that of the largest */
void
sift_side_image_scope(SiftSideImage *im, SiftColSegment *seg, const SiftColRel *r, const SideScopeInput *in, SideCounts *cnt)
{
	MemoryContext old = CurrentMemoryContext,
				wk = AllocSetContextCreate(im->cx, "sift col side build", ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(wk);
	side_scope_body(im, seg, r, in, cnt);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(wk);
	im->n++;
}
