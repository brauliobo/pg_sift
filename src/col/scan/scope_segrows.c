/*
 * col/scan/scope_segrows.c - the rows of the members whose pairs changed, read from the entity segments (sift_scope_segment_plan, sift_scope_segment_rows): the units that may hold a member are listed
 * from the zones and the member leaf, and their rows are found by member key (col/scan/scope_delta.c copies them for each pair)
 */
/* check-memory: allow-allocs the arrays follow the rows of the members of one read, which the caller bounds by work_mem through the pairs */
#include "postgres.h"

#include "common/int.h"
#include "core/cmp.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/conn.h"
#include "col/scan_int.h"
#include "col/scan/scope_pairs.h"
#include "col/scope.h"
#include "col/wbuf.h"
#include "core/damage.h"

static int
cmp_member(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

/* does the zone z of a unit's member leaf leave room for one of the members (ascending m[0..n))? */
static bool
zone_may_hold(const SiftCZone *z, const int64 *m, uint32 n)
{
	uint32		lo = 0,
				hi = n;

	if ((z->flags & SIFT_COLZ_OVERFLOW))
		return true;
	if (z->nvals == z->nnull)
		return false;
	SIFT_BISECT(uint32, mid, lo, hi, m[mid] < z->min);
	return lo < n && m[lo] <= z->max;
}

/*
 * What the point directory of the member leaf of a segment lists of the members: rows of the segment (a directory of rows), or heap addresses (a directory of addresses, that name the unit of
 * a segment cut by superblock). Both are candidates: the value is read at them
 */
typedef struct UnitListing
{
	bool		found;
	bool		tids;
	uint32	   *rows;			/* ascending */
	SiftKey    *addr;			/* ascending */
	uint32		n;
	uint32		next;			/* the first entry not handed over yet */
} UnitListing;

static UnitListing
unit_listing(SiftColSegment *seg, int fk, const int64 *members, uint32 n)
{
	UnitListing l = {0};

	if (sift_cseg_point_is_tid(seg, fk))
	{
		uint64	   *keep = palloc(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));

		memset(keep, 0xFF, SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
		l.tids = l.found = !sift_cseg_keyed(&seg->entry) && sift_cseg_point_tids(seg, fk, members, n, keep, &l.addr, &l.n);
		pfree(keep);
	}
	else
		l.found = sift_cseg_point_rows(seg, fk, members, n, &l.rows, &l.n);
	return l;
}

/* how many units of the segment the listing names rows in */
static uint64
listing_units(const UnitListing *l, SiftColSegment *seg)
{
	uint64		units = 0;
	uint32		u = 0,
				last = PG_UINT32_MAX;

	for (uint32 j = 0; j < l->n; j++)
	{
		if (l->tids)
			u = sift_cseg_unit_of_block(seg, sift_key_block(l->addr[j]));
		else
			while (l->rows[j] >= seg->sbdir[u].rank_base + seg->sbdir[u].nrows)
				u++;
		units += u != last;
		last = u;
	}
	return units;
}

/* The rows of unit u (ascending) the listing names, in *cand: false when it names none (the listing is walked in the order of the units) */
static bool
listing_unit(UnitListing *l, SiftColSegment *seg, uint32 u, uint32 **cand, uint32 *ncand)
{
	const SiftColSBRef *ur = &seg->sbdir[u];
	uint32		first = l->next;

	if (!l->tids)
	{
		while (l->next < l->n && l->rows[l->next] < ur->rank_base + ur->nrows)
			l->next++;
		if (l->next == first)
			return false;
		*cand = palloc((l->next - first) * sizeof(uint32));
		for (uint32 j = first; j < l->next; j++)
			(*cand)[j - first] = l->rows[j] - (uint32) ur->rank_base;
		*ncand = l->next - first;
		return true;
	}
	while (l->next < l->n && sift_cseg_unit_of_block(seg, sift_key_block(l->addr[l->next])) == u)
		l->next++;
	if (l->next == first)
		return false;
	{
		SiftCUnitView v;

		if (!sift_cseg_unit_view(seg, u, &v))
			SIFT_DAMAGE_DATA(seg->index, errmsg("sift_col: unit %u of segment %llu does not parse", u, (unsigned long long) seg->entry.gen));
		*cand = palloc((l->next - first) * sizeof(uint32));
		*ncand = sift_cunit_find_rows(&v, ur->id, l->addr + first, l->next - first, *cand);
	}
	return *ncand > 0;
}

/* does the member key v belong to the members (ascending m[0..n), mem their bitmap) */
static bool
member_of(const Members *mem, const int64 *m, uint32 n, int64 v)
{
	return mem->bits != NULL ? members_has(mem, v) : bsearch(&v, m, n, sizeof(int64), cmp_member) != NULL;
}

/*
 * The members as one segment's units are asked for them. An integer member: its own keys, which the zone and the leaf of a unit hold. A string member (mm): the units of a segment share its
 * dictionary (a unit with strings of its own, SIFT_COLZ_INLINE, has none), so the members are looked up in it once, and the zone and the leaf, which hold its codes, are asked as an integer's are,
 * with the member (its code in the map of the read) of each code at hand: nothing is hashed or looked up for a row.
 */
typedef struct SegMembers
{
	const int64 *m;				/* ascending: the members, or their codes in the dictionary of the segment (str, once made) */
	uint32		n;
	Members		mem;			/* of m */
	SiftKeyMap *mm;
	const int64 *ids;			/* the members of the read, ascending, and their bitmap: what a unit with strings of its own is asked for */
	uint32		nids;
	const Members *ids_mem;
	int64	   *id;				/* the member of each of the codes m */
	uint32	   *at;				/* when the codes span few: the place in m of the code first + i, else (NULL) a bisection finds it */
	bool		coded;			/* m are the codes of the dictionary */
	bool		lut_made;
	uint64	   *lut;			/* a bit for each integer up to lut_max that is one of m: the lookup kernel tests the member leaf of a unit by it (NULL: none, the rows are tested one by one) */
	int64		lut_max;
} SegMembers;

#define SEG_CODES_SPAN_MAX	((uint64) 1 << 20)

static SegMembers
seg_members(const int64 *members, uint32 n, SiftKeyMap *mm, const Members *mem)
{
	return (SegMembers) {.m = members, .n = n, .mem = *mem, .mm = mm, .ids = members, .nids = n, .ids_mem = mem};
}

typedef struct CodeId
{
	int64		code;
	int64		id;
} CodeId;

static int
cmp_code(const void *a, const void *b)
{
	return pg_cmp_s64(((const CodeId *) a)->code, ((const CodeId *) b)->code);
}

/* the members of a string member as codes of the dictionary of the segment: those it has */
static void
seg_codes(SegMembers *sm, SiftColSegment *seg, int fk)
{
	SiftCDict  *dict;
	CodeId	   *ci;
	int64	   *code;
	uint32		k = 0;

	if (sm->coded)
		return;
	dict = sift_cseg_dict(seg, fk);
	ci = palloc(Max(sm->nids, 1) * sizeof(CodeId));
	for (uint32 i = 0; i < sm->nids; i++)
	{
		uint32		len;
		const char *str = sift_keymap_bytes(sm->mm, sm->ids[i], &len);
		int64		c = sift_cdict_find(dict, str, len);

		if (c >= 0)
			ci[k++] = (CodeId) {c, sm->ids[i]};
	}
	qsort(ci, k, sizeof(CodeId), cmp_code);
	code = palloc(Max(k, 1) * sizeof(int64));
	sm->id = palloc(Max(k, 1) * sizeof(int64));
	for (uint32 i = 0; i < k; i++)
	{
		code[i] = ci[i].code;
		sm->id[i] = ci[i].id;
	}
	pfree(ci);
	sm->m = code;
	sm->n = k;
	sm->mem = k > 0 ? members_of(code, sizeof(int64), k) : (Members) {0, 0, NULL};
	if (k > 0 && sm->mem.span <= SEG_CODES_SPAN_MAX)
	{
		sm->at = palloc(sm->mem.span * sizeof(uint32));
		for (uint32 i = 0; i < k; i++)
			sm->at[code[i] - sm->mem.lo] = i;
	}
	sm->coded = true;
}

/* does unit u of the segment hold a row of a member: its zone of the member leaf leaves room for one (a string member: for the code of one, a unit with strings of its own is read whole) */
static bool
unit_may_hold(SegMembers *sm, SiftColSegment *seg, int fk, uint32 u)
{
	SiftCZone	z;

	sift_cseg_zone(seg, fk, u, &z);
	if (sm->mm != NULL)
	{
		if (z.flags & SIFT_COLZ_INLINE)
			return true;
		seg_codes(sm, seg, fk);
	}
	return zone_may_hold(&z, sm->m, sm->n);
}

/* the member (the code in mm) of each row of the chunk c of the member leaf of a unit with strings of its own, PG_INT64_MIN for a value mm has none for */
static void
unit_member_ids(const SegMembers *sm, SiftCChunk *c, int64 *ids)
{
	SiftCDict  *dict;
	SiftCChunk	codes = *c;
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	(void) sift_cchunk_istr_codes(c, CurrentMemoryContext, NULL, 0, &dict, &codes);
	sift_cchunk_decode(&codes, key);
	sift_keymap_batch_codes(sm->mm, dict, key, c->nulls, c->n, false, ids);
	pfree(key);
}

/* the member of the code of the leaf, which is one of those of sm */
static int64
member_of_code(const SegMembers *sm, int64 code)
{
	if (sm->at != NULL)
		return sm->id[sm->at[code - sm->mem.lo]];
	return sm->id[(const int64 *) bsearch(&code, sm->m, sm->n, sizeof(int64), cmp_member) - sm->m];
}

/* The rows r < n of val whose member key is one of the members (a byte or a bit for each key of their range, else a search), ascending: a row that passes the test is stored and counted without a branch on it */
static uint32
rows_of_members(const Members *mem, const int64 *m, uint32 nm, const int64 *val, uint32 n, uint32 *out)
{
	const uint64 lo = (uint64) mem->lo,
				span = mem->span;
	const uint8 *flags = mem->flags;
	const uint64 *bits = mem->bits;
	uint32		k = 0;

	/* the members are read into locals: the store to out would make the compiler load them again for each row */
	if (flags != NULL)
		for (uint32 r = 0; r < n; r++)
		{
			uint64		off = (uint64) val[r] - lo;

			out[k] = r;
			k += off < span ? flags[off] : 0;
		}
	else if (bits != NULL)
		for (uint32 r = 0; r < n; r++)
		{
			uint64		off = (uint64) val[r] - lo;

			out[k] = r;
			k += off < span ? (bits[off >> 6] >> (off & 63)) & 1 : 0;
		}
	else
		for (uint32 r = 0; r < n; r++)
		{
			out[k] = r;
			k += member_of(mem, m, nm, val[r]);
		}
	return k;
}

/* The rows (ascending, n of them) that are not NULL in nulls and are live (NULL: all of them are) */
static uint32
rows_kept(uint32 *rows, uint32 n, const uint64 *nulls, const uint64 *live)
{
	uint32		k = 0;

	for (uint32 j = 0; j < n; j++)
		if (!(nulls != NULL && sift_col_bit_test(nulls, rows[j])) && (live == NULL || sift_col_bit_test(live, rows[j])))
			rows[k++] = rows[j];
	return k;
}

/* the largest integer a lookup table covers: its bits are the members' ones, 512 KB of them */
#define SEG_LUT_MAX		((int64) 1 << 22)

/* may the lookup kernel test the member leaf of a unit whose zone is z: its values are integers 0 .. SEG_LUT_MAX, and are not the strings of the unit (SIFT_COLZ_INLINE) */
static inline bool
lut_unit(const SiftCZone *z)
{
	return !(z->flags & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE)) && z->nvals > z->nnull && z->min >= 0 && z->max <= SEG_LUT_MAX;
}

/* The table of the members for the units of a segment that lut_unit passes: the first call makes it, up to the largest value those units hold (the members are the codes of the dictionary for a string member) */
static void
seg_lut(SegMembers *sm, SiftColSegment *seg, int fk)
{
	int64		max = -1;

	if (sm->lut_made)
		return;
	sm->lut_made = true;
	if (sm->mem.bits == NULL)
		return;
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCZone	z;

		sift_cseg_zone(seg, fk, u, &z);
		if (lut_unit(&z))
			max = Max(max, z.max);
	}
	if (max < 0)
		return;
	sm->lut_max = max;
	sm->lut = palloc0((max / 64 + 2) * sizeof(uint64));
	for (uint64 w = 0; w < (sm->mem.span + 63) / 64; w++)
		for (uint64 b = sm->mem.bits[w]; b != 0; b &= b - 1)
		{
			int64		v = sm->mem.lo + (int64) (w * 64 + pg_rightmost_one_pos64(b));

			if (v >= 0 && v <= max)
				sift_col_bit_set(sm->lut, v);
		}
}

/* The live rows of a unit whose member leaf c is tested by the lookup kernel: the codes of the leaf in 32 bits, a bit for each row, the rows of the bits that are not NULL and live, and the member of each */
static void
unit_rows_lut(const SegMembers *sm, const SiftCChunk *c, const uint64 *live, uint32 *out, uint32 *nout, int64 **ids)
{
	uint32	   *val = palloc(SIFT_CC_CAP32(c->n) * sizeof(uint32));
	uint64	   *hit = palloc(SIFT_COL_NWORDS(c->n) * sizeof(uint64));

	sift_cchunk_decode_u32(c, val);
	sift_colk->lut[2](val, c->n, sm->lut, hit);
	if (c->nulls != NULL)
		sift_colk->bits_andnot(hit, c->nulls, SIFT_COL_NWORDS(c->n));
	if (live != NULL)
		sift_colk->bits_and(hit, live, SIFT_COL_NWORDS(c->n));
	*nout = sift_colk->bits_to_idx(hit, SIFT_COL_NWORDS(c->n), 0, out);
	if (*nout > 0)
	{
		*ids = palloc(*nout * sizeof(int64));
		for (uint32 j = 0; j < *nout; j++)
			(*ids)[j] = sm->mm == NULL ? (int64) val[out[j]] : member_of_code(sm, val[out[j]]);
	}
	pfree(hit);
	pfree(val);
}

/*
 * The live rows of unit u (ascending) that may hold a member key: the cand[0..ncand) the point directory lists (rows of the unit), which the caller checks against the values it decodes, or, with no
 * directory (cand NULL), the rows of the unit whose member key is among the members, read from the leaf. NULL when none. A string member: *ids the member of each row returned
 */
static uint32 *
unit_rows_of(SegMembers *sm, SiftColSegment *seg, uint32 u, int fk, const uint32 *cand, uint32 ncand, uint32 *nout, int64 **ids)
{
	const SiftColSBRef *ur = &seg->sbdir[u];
	uint32	   *out = palloc((Max(cand != NULL ? ncand : ur->nrows, 1) + SIFT_COL_IDXPAD) * sizeof(uint32));
	uint64	   *live = NULL;

	*nout = 0;
	*ids = NULL;
	if (sift_cseg_dirty(seg, u))
	{
		live = palloc(SIFT_COL_NWORDS(ur->nrows) * sizeof(uint64));
		sift_cseg_live(seg, u, live);
	}
	if (cand != NULL)
	{
		for (uint32 j = 0; j < ncand; j++)
			if (live == NULL || sift_col_bit_test(live, cand[j]))
				out[(*nout)++] = cand[j];
	}
	else
	{
		SiftCUnitView v;
		SiftCChunk	c;
		SiftCZone	z;
		int64	   *val;
		bool		own;			/* the strings of the unit are its own: val are the members */
		const Members *mem;
		const int64 *m;
		uint32		n;

		if (!sift_cseg_unit_view(seg, u, &v))
			SIFT_DAMAGE_DATA(seg->index, errmsg("sift_col: unit %u of segment %llu does not parse", u, (unsigned long long) seg->entry.gen));
		sift_cseg_zone(seg, fk, u, &z);
		own = sm->mm != NULL && (z.flags & SIFT_COLZ_INLINE);
		mem = own ? sm->ids_mem : &sm->mem;
		m = own ? sm->ids : sm->m;
		n = own ? sm->nids : sm->n;
		if (v.chunk_len[fk] > 0)
			sift_cseg_unit_fill(seg, u, v.chunk_off[fk], v.chunk_len[fk]);
		if (v.chunk_len[fk] > 0 && sift_cunit_chunk(&v, fk, &c))
		{
			if (!own && lut_unit(&z))
				seg_lut(sm, seg, fk);
			if (!own && sm->lut != NULL && lut_unit(&z))
				unit_rows_lut(sm, &c, live, out, nout, ids);
			else
			{
				val = palloc((c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
				if (own)
					unit_member_ids(sm, &c, val);
				else
					sift_cchunk_decode(&c, val);
				*nout = rows_of_members(mem, m, n, val, c.n, out);
				if (c.nulls != NULL || live != NULL)
					*nout = rows_kept(out, *nout, c.nulls, live);
				if (*nout > 0)
				{
					*ids = palloc(*nout * sizeof(int64));
					for (uint32 j = 0; j < *nout; j++)
						(*ids)[j] = sm->mm == NULL || own ? val[out[j]] : member_of_code(sm, val[out[j]]);
				}
				pfree(val);
			}
		}
	}
	if (live != NULL)
		pfree(live);
	if (*nout == 0)
	{
		pfree(out);
		return NULL;
	}
	return out;
}

/*
 * The rows of the decoded batches that are of a member (the directory may list a row of another key), as two buffers that share the batches: those on pages the map holds all-visible (read after the
 * rows were: core/visibility.h), which the scan may take as it takes a stored unit's, and the others, which it checks against the heap
 */
static void
add_split(SiftScopeBuffer *out, int *nout, SiftVis *vis, SiftColBatch **bs, int64 **ids, int nb, uint32 flags, int fk, const int64 *m, uint32 n, SiftKeyMap *mm, const Members *mem)
{
	SiftScopeBuffer set[2];		/* [0] checked against the heap, [1] all-visible */

	for (int k = 0; k < 2; k++)
	{
		set[k] = (SiftScopeBuffer) {.stamp = InvalidTransactionId, .roots = (flags & SIFT_SEG_ROOTS) != 0, .lane = k == 0};
		set[k].b = palloc(nb * sizeof(SiftColBatch *));
		set[k].rows = palloc(nb * sizeof(uint64 *));
		set[k].ids = palloc(nb * sizeof(int64 *));
	}
	for (int i = 0; i < nb; i++)
	{
		uint32		rows = bs[i]->nrows;
		BlockNumber *blk = palloc(rows * sizeof(BlockNumber));
		uint64	   *bits = palloc(SIFT_COL_NWORDS(rows) * sizeof(uint64));
		uint64	   *take[2] = {palloc0(SIFT_COL_NWORDS(rows) * sizeof(uint64)), palloc0(SIFT_COL_NWORDS(rows) * sizeof(uint64))};
		bool		any[2] = {false, false};
		const int64 *member = ids[i] != NULL ? ids[i] : sift_scope_batch_members(mm, bs[i], fk);

		for (uint32 r = 0; r < rows; r++)
			blk[r] = sift_key_block(bs[i]->keys[r]);
		sift_vis_blocks_allvisible(vis, blk, rows, bits);
		if (ids[i] != NULL)
		{
			/* the rows are those of the members: the pages the map holds all-visible are one set, the others the other */
			for (uint32 w = 0; w < (rows + 63) / 64; w++)
			{
				uint64		valid = rows - w * 64 >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (rows - w * 64)) - 1;

				take[1][w] = bits[w];
				take[0][w] = ~bits[w] & valid;
				any[1] |= bits[w] != 0;
				any[0] |= take[0][w] != 0;
			}
		}
		else
			for (uint32 r = 0; r < rows; r++)
			{
				int			k = sift_col_bit_test(bits, r) ? 1 : 0;

				if (sift_col_bit_test(bs[i]->col[fk].nulls, r) || !member_of(mem, m, n, member[r]))
					continue;
				sift_col_bit_set(take[k], r);
				any[k] = true;
			}
		for (int k = 0; k < 2; k++)
			if (any[k])
			{
				set[k].b[set[k].n] = bs[i];
				set[k].ids[set[k].n] = ids[i];
				set[k].rows[set[k].n++] = take[k];
			}
		pfree(blk);
		pfree(bits);
		if (ids[i] == NULL && mm != NULL)
			pfree((int64 *) member);
	}
	for (int k = 0; k < 2; k++)
		if (set[k].n > 0)
			out[(*nout)++] = set[k];
}

/* What the entity segments say of the rows of the members before any is read: for each, its listing of the point directory, or the units the zone of the member leaf leaves open */
typedef struct SegPlan
{
	int			seg;			/* its place in the directory */
	UnitListing listing;
} SegPlan;

struct SiftScopeSegPlan
{
	int			fk;
	int			nseg;
	SegPlan	   *seg;
	const int64 *members;
	uint32		n;
	Members		mem;
	SiftKeyMap *mm;				/* a string member: the members are codes of this map, and no point directory of the leaf tells where they are */
};

/*
 * words: the members are the journal words of a string member, not yet its members (the pairs are not listed): no unit can be ruled out, and every one is expected to be read
 */
SiftScopeSegPlan *
sift_scope_segment_plan(Relation index, int scope, const SiftDirSnapshot *dir, const int64 *members, uint32 n, SiftKeyMap *mm, bool words, double rows_per_member, uint64 *rows, uint64 *scanned,
						uint64 *units)
{
	SiftScopeSegPlan *plan = palloc0(sizeof(SiftScopeSegPlan));

	*rows = *scanned = *units = 0;
	plan->fk = sift_scope_fk_leaf(index, scope);
	plan->members = members;
	plan->n = n;
	plan->mem = members_of(members, sizeof(int64), n);
	plan->mm = mm;
	plan->seg = palloc(Max(dir->nsegs, 1) * sizeof(SegPlan));
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;
		SegPlan    *sp = &plan->seg[plan->nseg];
		SegMembers	sm = seg_members(members, n, mm, &plan->mem);
		uint64		open = 0;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || sift_seg_is_scope(&dir->segs[i]))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		sp->seg = i;
		sp->listing = mm != NULL ? (UnitListing) {0} : unit_listing(seg, plan->fk, members, n);
		if (sp->listing.found)
		{
			*rows += sp->listing.n;
			*units += listing_units(&sp->listing, seg);
		}
		else
		{
			for (uint32 u = 0; u < seg->nsb; u++)
				if (words || unit_may_hold(&sm, seg, plan->fk, u))
				{
					open += seg->sbdir[u].nrows;
					(*units)++;
				}
			*scanned += open;
			*rows += (uint64) (open * Min(1.0, n * rows_per_member / Max(seg->entry.ndocs, 1)));
		}
		sift_cseg_close(seg);
		plan->nseg++;
	}
	return plan;
}

SiftScopeBuffer *
sift_scope_segment_rows(SiftScopeSegPlan *plan, Relation heap, Relation index, Snapshot snap, int scope, const SiftDirSnapshot *dir, SiftScopeBuffer *bufs, int *nbufs,
						const int *leaves, int nleaves, uint64 *rows, uint64 *units)
{
	int			fk = plan->fk;
	bool		kept[SIFT_COL_MAX_COLS],
				want[SIFT_COL_MAX_COLS];
	SiftVis		vis;

	*rows = *units = 0;
	sift_scope_delta_kept(index, scope, leaves, nleaves, kept);
	bufs = repalloc(bufs, (*nbufs + 2 * Max(plan->nseg, 1)) * sizeof(SiftScopeBuffer));
	sift_vis_begin(&vis, heap, snap, index, dir->version);
	for (int i = 0; i < plan->nseg; i++)
	{
		SegPlan    *sp = &plan->seg[i];
		SiftColSegment *seg = sift_cseg_open(index, &dir->segs[sp->seg], CurrentMemoryContext);
		SegMembers	sm = seg_members(plan->members, plan->n, plan->mm, &plan->mem);
		SiftColBatch **bs = NULL;
		int64	  **bids = NULL;
		int			nb = 0,
					cap = 0;

		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint32	   *cand = NULL,
					   *ords;
			uint32		ncand = 0,
						nord;
			int64	   *ids;
			SiftColBatch *b1;

			if (sp->listing.found ? !listing_unit(&sp->listing, seg, u, &cand, &ncand) : !unit_may_hold(&sm, seg, fk, u))
				continue;
			(*units)++;
			if ((ords = unit_rows_of(&sm, seg, u, fk, cand, ncand, &nord, &ids)) == NULL)
				continue;
			b1 = sift_cbatch_create(CurrentMemoryContext, seg->schema);
			/* the rows the point directory listed have no member id yet: it is read from the rows */
			memcpy(want, kept, sizeof(kept));
			want[fk] |= ids == NULL;
			sift_cseg_decode_unit_rows(seg, u, want, ords, nord, b1);
			pfree(ords);
			if (nb == cap)
			{
				cap = Max(8, cap * 2);
				bs = bs ? repalloc(bs, cap * sizeof(SiftColBatch *)) : palloc(cap * sizeof(SiftColBatch *));
				bids = bids ? repalloc(bids, cap * sizeof(int64 *)) : palloc(cap * sizeof(int64 *));
			}
			bids[nb] = ids;
			bs[nb++] = b1;
			*rows += b1->nrows;
		}
		sift_cseg_close(seg);
		if (nb > 0)
			add_split(bufs, nbufs, &vis, bs, bids, nb, dir->segs[sp->seg].flags, fk, plan->members, plan->n, plan->mm, &plan->mem);
	}
	sift_vis_end(&vis);
	return bufs;
}
