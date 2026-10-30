/*
 * col/scan/prune_point.c - the pruning of a segment by heap address and by key
 * a window of units, the points and ranges a predicate names
 * and the lists of addresses it intersects
 */
#include "postgres.h"
#include "col/point.h"
#include "col/scan_int.h"

/* No unit of the range is kept: the tests that follow cannot change what a begin reads, and its other columns' zones are not touched */
bool
scan_window_empty(const uint64 *keep, const PruneRange *pr)
{
	uint32		a = pr->ulo / 64,
				b = (pr->uhi + 63) / 64;

	for (uint32 w = a; w < b; w++)
	{
		uint64		m = ~UINT64CONST(0);

		if (w == a)
			m &= ~UINT64CONST(0) << (pr->ulo % 64);
		if (w == b - 1 && pr->uhi % 64)
			m &= (UINT64CONST(1) << (pr->uhi % 64)) - 1;
		if (keep[w] & m)
			return false;
	}
	return true;
}

/* The keys a test can match, when they are few (palloc'd, *nkeys of them): the keys of the point directory to probe. NULL: the units answer */
int64 *
scan_point_keys(const SiftCTest *t, int *nkeys)
{
	int64	   *keys;
	int			n = 0;

	switch (t->kind)
	{
		case SIFT_CT_SET:
			if (t->nset == 0 || t->nset > SIFT_CPOINT_MAX_KEYS)
				return NULL;
			keys = palloc(t->nset * sizeof(int64));
			memcpy(keys, t->set, t->nset * sizeof(int64));
			n = t->nset;
			break;
		case SIFT_CT_RANGE:
			if (t->lo > t->hi || (uint64) t->hi - (uint64) t->lo >= SIFT_CPOINT_MAX_RANGE)
				return NULL;
			n = (int) ((uint64) t->hi - (uint64) t->lo) + 1;
			keys = palloc(n * sizeof(int64));
			for (int i = 0; i < n; i++)
				keys[i] = (int64) ((uint64) t->lo + i);
			break;
		case SIFT_CT_LUT:
			{
				/* by words: a table of the codes of a large dictionary costs its words, not a test of every code */
				uint32		w0,
							w1;

				if (t->hi < 0 || t->lo >= t->lutbits)
					return NULL;
				w0 = (uint32) Max(t->lo, 0) / 64;
				w1 = (uint32) Min(t->hi, t->lutbits - 1) / 64 + 1;
				for (uint32 w = w0; w < w1; w++)
					n += pg_popcount64(t->lut[w]);
				if (n == 0 || n > SIFT_CPOINT_MAX_KEYS)
					return NULL;
				keys = palloc(n * sizeof(int64));
				n = 0;
				for (uint32 w = w0; w < w1; w++)
					for (uint64 bits = t->lut[w]; bits; bits &= bits - 1)
					{
						int64		k = (int64) w * 64 + pg_rightmost_one_pos64(bits);

						if (k >= Max(t->lo, 0) && k <= t->hi)
							keys[n++] = k;
					}
				if (n == 0)
				{
					pfree(keys);
					return NULL;
				}
			}
			break;
		default:
			return NULL;
	}
	*nkeys = n;
	return keys;
}

/*
 * The units that hold the rows the point directory lists for the predicate i, on leaf col, of the segment (the test t), when the directory has a short
 * answer: the other units are cut, and the cursor keeps the rows for the units that remain (sift_col_point_sel). One directory per segment answers.
 */
/*
 * A directory of heap addresses (column format minor 16) names the rows with no unit to read: the addresses in the units that still pass the zones go to the
 * source, and those units are done. A unit with a dead row is read instead, as if no directory were there (the address of a dead row may be used again), and
 * so is every unit when the caller wants the values (an index-only scan): the directory only cuts the units, whose rows the predicate then selects.
 */
#define SIFT_CTID_FILTER 64

/* are the units of `keep` all among those of `held` */
static bool
units_within(const uint64 *keep, const uint64 *held, uint32 nw)
{
	for (uint32 w = 0; w < nw; w++)
		if (keep[w] & ~held[w])
			return false;
	return true;
}

static bool
keep_some(const ColCursor *c, const uint64 *keep)
{
	uint32		nw = SIFT_COL_NWORDS(c->seg->nsb);
	bool		some = false;

	for (uint32 w = 0; w < nw && !some; w++)
		some = (w == c->seg->nsb / 64 ? keep[w] & ((UINT64CONST(1) << (c->seg->nsb % 64)) - 1) : keep[w]) != 0;
	return some;
}

/* The stream c->ps hands the addresses over: no unit stays kept until it does */
static void
point_lazy_adopt(ColCursor *c, int i, uint64 *keep)
{
	uint32		nw = SIFT_COL_NWORDS(c->seg->nsb);

	c->pkept = palloc(nw * sizeof(uint64));
	memcpy(c->pkept, keep, nw * sizeof(uint64));
	c->ppend = palloc0(nw * sizeof(uint64));
	memset(keep, 0, nw * sizeof(uint64));
	c->ptids = true;
	c->ppred = i;
}

/* The unit of an address a list of the segment names: a segment cut by key has no unit by address, and an address in no unit is a corrupt list */
static uint32
listed_unit(SiftColSegment *seg, SiftKey key)
{
	uint32		u = sift_cseg_unit_of_block(seg, sift_key_block(key));

	SIFT_INVARIANT(u != SIFT_CUNIT_UNKNOWN, "sift_col: the addresses of a segment cut by key name no unit");
	if (u >= seg->nsb)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("sift_col index \"%s\" lists the address %llx, which is in no unit of its segment",
																   RelationGetRelationName(seg->index), (unsigned long long) key)));
	return u;
}

/*
 * The ascending addresses of the rows a directory or a range lists: those in a unit that is kept, has no dead row and is not read for the other predicates go to the
 * source; the units that hold any other address stay to be read, and the units that hold none are cleared.
 */
static bool
emit_tids(SiftColSource *src, ColCursor *c, int i, uint64 *keep, const SiftKey *tids, uint32 n)
{
	SiftColSegment *seg = c->seg;
	uint32		nw = SIFT_COL_NWORDS(seg->nsb),
				last = PG_UINT32_MAX;
	bool		clean = false,
				emit = (src->flags & SIFT_COLO_POINT_TIDS) != 0;
	uint64	   *has,
			   *done;

	c->prun = src->nptid;
	has = palloc0(2 * nw * sizeof(uint64));
	done = has + nw;
	/* the addresses ascend, so a unit repeats, and the run of a segment follows the runs of the segments before it unless their blocks interleave */
	if (n > 0 && src->nptid > 0 && tids[0] < src->ptid[src->nptid - 1])
		src->ptid_unsorted = true;
	for (uint32 k = 0, e; k < n; k = e)
	{
		uint32		sb = sift_block_sb(sift_key_block(tids[k])),
					u = last != PG_UINT32_MAX && sb == sift_cseg_unit_sb(seg, last) ? last : listed_unit(seg, tids[k]);

		for (e = k + 1; e < n && sift_block_sb(sift_key_block(tids[e])) == sb; e++)
			;
		sift_col_bit_set(has, u);
		last = u;
		/* past SIFT_CTID_FILTER addresses, the heap fetches cost more than reading the unit and testing the other predicates on its rows */
		clean = emit && sift_col_bit_test(keep, u) && !sift_cseg_dirty(seg, u) && !(c->others && e - k > SIFT_CTID_FILTER);
		if (!clean)
			continue;
		if (src->nptid + (e - k) > src->capptid)
		{
			src->capptid = Max(64, Max(src->capptid * 2, src->nptid + (e - k)));
			src->ptid = src->ptid ? repalloc_huge(src->ptid, src->capptid * sizeof(SiftKey)) : palloc(src->capptid * sizeof(SiftKey));
		}
		memcpy(src->ptid + src->nptid, tids + k, (e - k) * sizeof(SiftKey));
		src->nptid += e - k;
		sift_col_bit_set(done, u);
	}
	for (uint32 w = 0; w < nw; w++)
		keep[w] &= has[w] & ~done[w];
	pfree(has);
	if (src->flags & SIFT_COLO_POINT_ROWS)
	{
		c->ptidv = (SiftKey *) tids;
		c->nptidv = n;
	}
	c->ptids = true;
	c->ppred = i;
	return true;
}

/*
 * SIFT_COLO_POINT_LAZY: the lists of the lookup stay undecoded (a cursor over each, sift_cseg_point_stream_open) and the units they name are told apart as
 * sift_col_point_next hands the addresses over: no unit stays kept until then (the units with a dead row come back at the end of the segment's addresses). Not
 * for a scan with other predicates, whose units with many addresses are read instead of fetched (that needs every address of the unit before the first).
 */
static bool
prune_point_lazy(SiftColSource *src, ColCursor *c, int i, int col, const int64 *keys, int nkeys, uint64 *keep)
{
	if (!(src->flags & SIFT_COLO_POINT_LAZY) || c->others || !keep_some(c, keep) || (c->ps = sift_cseg_point_stream_open(c->seg, col, keys, nkeys)) == NULL)
		return false;
	point_lazy_adopt(c, i, keep);
	return true;
}

static bool
prune_point_tids(SiftColSource *src, ColCursor *c, int i, int col, const int64 *keys, int nkeys, uint64 *keep)
{
	SiftColSegment *seg = c->seg;
	SiftKey    *tids;
	uint32		n,
				nw = SIFT_COL_NWORDS(seg->nsb);
	bool		emit = (src->flags & SIFT_COLO_POINT_TIDS) != 0;

	/* a range of the scan that lists a fourth of the rows the keys do or less narrows the heap's work as well, and the list of the keys is not decoded */
	if (emit && c->rhint != UINT32_MAX && sift_cseg_point_nkeys(seg, col) > 0 &&
		(double) c->rhint * 4 < (double) nkeys * sift_cseg_point_count(seg, col) / sift_cseg_point_nkeys(seg, col))
		return false;
	if (emit && prune_point_lazy(src, c, i, col, keys, nkeys, keep))
		return true;
	if (c->pc_tids != NULL && c->pc_gen == seg->entry.gen && c->pc_col == col && c->pc_nkeys == nkeys && memcmp(c->pc_keys, keys, nkeys * sizeof(int64)) == 0 &&
		units_within(keep, c->pc_keep, nw))
	{
		tids = c->pc_tids;
		n = c->pc_n;
	}
	else
	{
		/* the begins of a scan ask the same keys again (the zones of the top-N bar change between them) and the list is decoded entry by entry: only the addresses in the units still read */
		MemoryContext old = MemoryContextSwitchTo(src->mcxt);
		bool		found = sift_cseg_point_tids(seg, col, keys, nkeys, keep, &tids, &n);

		if (found)
		{
			if (c->pc_tids)
				pfree(c->pc_tids);
			if (c->pc_keys)
				pfree(c->pc_keys);
			if (c->pc_keep)
				pfree(c->pc_keep);
			c->pc_keys = palloc(Max(nkeys, 1) * sizeof(int64));
			memcpy(c->pc_keys, keys, nkeys * sizeof(int64));
			c->pc_keep = palloc(nw * sizeof(uint64));
			memcpy(c->pc_keep, keep, nw * sizeof(uint64));
			c->pc_gen = seg->entry.gen;
			c->pc_col = col;
			c->pc_nkeys = nkeys;
			c->pc_tids = tids ? tids : palloc(sizeof(SiftKey));
			c->pc_n = n;
		}
		MemoryContextSwitchTo(old);
		if (!found)
			return false;
	}
	return emit_tids(src, c, i, keep, tids, n);
}

/* The run of the segment (its addresses in the units handed over) narrowed to the addresses of another list, and the units still to read to those that hold one of them */
static bool
intersect_tids(SiftColSource *src, ColCursor *c, uint64 *keep, const SiftKey *tids, uint32 n)
{
	SiftColSegment *seg = c->seg;
	uint32		nw = SIFT_COL_NWORDS(seg->nsb),
				last = PG_UINT32_MAX,
				m = 0,
				j = 0,
				nrun = src->nptid - c->prun;
	SiftKey    *run = src->ptid + c->prun;
	uint64	   *has = palloc0(nw * sizeof(uint64));

	for (uint32 k = 0, e; k < n; k = e)
	{
		uint32		sb = sift_block_sb(sift_key_block(tids[k])),
					u = last != PG_UINT32_MAX && sb == sift_cseg_unit_sb(seg, last) ? last : listed_unit(seg, tids[k]);

		for (e = k + 1; e < n && sift_block_sb(sift_key_block(tids[e])) == sb; e++)
			;
		sift_col_bit_set(has, u);
		last = u;
	}
	for (uint32 i = 0; i < nrun; i++)
	{
		while (j < n && tids[j] < run[i])
			j++;
		if (j < n && tids[j] == run[i])
			run[m++] = run[i];
	}
	src->nptid = c->prun + m;
	for (uint32 w = 0; w < nw; w++)
		keep[w] &= has[w];
	pfree(has);
	return true;
}

/*
 * A range on a leaf with a global order is a seek in the sorted keys of the segment: the addresses of the rows between the bounds go to the heap as a
 * point directory's do, and the units that hold them but are read stay kept. Not past SIFT_CORDER_MAX_ROWS rows (the addresses are sorted by address), not for a scan that
 * wants values, not in a key-cut segment (an address names no unit there, emit_tids would keep the wrong ones). After another list of addresses answered the scan (a key and a range, two ranges) the range narrows that list when it is not much longer, as a bitmap AND does.
 */
bool
scan_prune_range_tids(SiftColSource *src, ColCursor *c, int i, int col, int64 lo, int64 hi, uint64 *keep)
{
	SiftKey    *tids;
	uint32		n,
				limit = SIFT_CORDER_MAX_ROWS;

	if (c->prow != NULL || c->ps != NULL || !(src->flags & SIFT_COLO_POINT_TIDS) || (src->flags & SIFT_COLO_POINT_ROWS) || sift_cseg_keyed(&c->seg->entry))
		return false;
	if (c->ptids)
		limit = Min(limit, Max(256, 8 * (src->nptid - c->prun)));
	if (!sift_cseg_gorder_tids(c->seg, col, lo, hi, limit, &tids, &n))
		return false;
	return c->ptids ? intersect_tids(src, c, keep, tids, n) : emit_tids(src, c, i, keep, tids, n);
}

static bool
prune_keys(SiftColSource *src, SiftColSegment *seg, ColCursor *c, int i, int col, const int64 *keys, int nkeys, uint64 *keep)
{
	uint32	   *rows,
				nrows,
				u = 0;
	uint64	   *has;

	if (sift_cseg_point_is_tid(seg, col))
		return prune_point_tids(src, c, i, col, keys, nkeys, keep);
	if (!sift_cseg_point_rows(seg, col, keys, nkeys, &rows, &nrows))
		return false;
	if (c->prow != NULL)
	{
		uint32		m = 0;

		for (uint32 a = 0, b = 0; a < c->nprow && b < nrows;)
			if (c->prow[a] < rows[b])
				a++;
			else if (c->prow[a] > rows[b])
				b++;
			else
			{
				rows[m++] = rows[b];
				a++;
				b++;
			}
		nrows = m;
	}
	has = palloc0(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	for (uint32 k = 0; k < nrows; k++)
	{
		while (u < seg->nsb && seg->sbdir[u].rank_base + seg->sbdir[u].nrows <= rows[k])
			u++;
		if (u < seg->nsb)
			sift_col_bit_set(has, u);
	}
	for (uint32 w = 0; w < SIFT_COL_NWORDS(seg->nsb); w++)
		keep[w] &= has[w];
	pfree(has);
	c->prow = rows;
	c->nprow = nrows;
	c->ppred = i;
	/* the directory of a list leaf holds each value's rows exactly (no two values share a fingerprint, or it was not written) */
	if (seg->schema->cols[col].parent >= 0 && i < 64)
		c->pexact |= UINT64CONST(1) << i;
	return true;
}

/*
 * A prefix or a range of a text leaf whose dictionary is read in place and whose point directory lists addresses: the keys of the lookup are the codes at places [from, to)
 * of the dictionary's byte order, which two bisections find. A scan that stops early (SIFT_COLO_POINT_LAZY) reads them from the order a chunk at a time and makes neither the
 * table of codes of the test nor the list of keys; one that does not, or a lookup of few keys, makes the keys and asks the directory as prune_point does.
 */
bool
scan_prune_places(SiftColSource *src, ColCursor *c, int i, int col, const SiftColPred *p, uint64 *keep)
{
	SiftColSegment *seg = c->seg;
	uint32		from,
				to,
				n;
	uint64		rows = (uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows;
	int64	   *keys;
	bool		ok;

	if (!(src->flags & SIFT_COLO_POINT_TIDS) || c->prow != NULL || c->ptids || p->nvals != 1 ||
		(p->op != SIFT_COLOP_PREFIX && !(p->op == SIFT_COLOP_LT || p->op == SIFT_COLOP_LE || p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE)) ||
		!sift_cseg_dict_places(seg, col, p->op, &p->vals[0], &from, &to))
		return false;
	n = to - from;
	if (n == 0 || n > SIFT_CPOINT_MAX_KEYS || (n > SIFT_CPOINT_MIN_KEYS && n > rows / 16))
		return false;
	if ((src->flags & SIFT_COLO_POINT_LAZY) && !c->others && n >= SIFT_CPOINT_CHUNK_MIN && keep_some(c, keep) &&
		(c->ps = sift_cseg_point_stream_open_places(seg, col, from, to)) != NULL)
	{
		point_lazy_adopt(c, i, keep);
		return true;
	}
	keys = palloc(n * sizeof(int64));
	sift_cseg_dict_codes(seg, col, from, to, keys);
	ok = prune_keys(src, seg, c, i, col, keys, n, keep);
	pfree(keys);
	return ok;
}

bool
scan_prune_point(SiftColSource *src, SiftColSegment *seg, ColCursor *c, int i, int col, const SiftCTest *t, uint64 *keep)
{
	int			nkeys;
	int64	   *keys;
	bool		ok;

	/* the lists of the values of a list leaf intersect: a row is kept for each value it must own */
	if ((c->prow != NULL && seg->schema->cols[col].parent < 0) || c->ptids || (keys = scan_point_keys(t, &nkeys)) == NULL)
		return false;
	/*
	 * A list that names a good share of the rows is no lookup: the zones and the units answer it as well. So does one whose keys have many rows
	 * on average: reading them costs about what scanning the units that hold them does (a row of a list is read at some ten times a row of a unit).
	 * A scan that hands the addresses to the heap has no units to scan, and the directory is its answer.
	 */
	if (nkeys > SIFT_CPOINT_MIN_KEYS && (uint64) nkeys > ((uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows) / 16)
		ok = false;
	else if ((src->flags & SIFT_COLO_POINT_TIDS) == 0 && sift_cseg_point_nkeys(seg, col) > 0 &&
			 (double) nkeys * sift_cseg_point_count(seg, col) / sift_cseg_point_nkeys(seg, col) >
			 Max((double) SIFT_CPOINT_DENSE_ROWS, ((double) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows) / SIFT_CPOINT_DENSE_SHARE))
		ok = false;
	else
		ok = prune_keys(src, seg, c, i, col, keys, nkeys, keep);
	pfree(keys);
	return ok;
}
