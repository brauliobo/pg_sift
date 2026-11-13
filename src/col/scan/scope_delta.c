/*
 * col/scan/scope_delta.c - the rows no scope entry describes, as rows of the scope units of a scope index (col/scope.h)
 *
 * No scope entry describes a row of a write buffer, nor a row of a member whose pairs changed since the stamp of the scope units (its stored entries are left out of the read): a scan of
 * the scope units reads them from the buffers and from the entity segments, each live row whose member has a pair among those the reader listed (the pairs under the statement
 * snapshot) copied once for each of them, with the scope key in the derived leaf, as the promotion would write it. The reader selects the rows (sift_scope_segment_rows reads those of the
 * changed members by member key; sift_scope_delta_select expands them: it knows then how many there are, which prices the read), the source makes the copies of one key one in-memory unit
 * (numbered after the stored units, rows in address order): what the scan reads of it is what it reads of a stored unit, and the rows keep their heap addresses, so the visibility check
 * and the tombstones are the ones of the rows they copy.
 */
/* check-memory: allow-allocs the arrays follow the rows of the write buffers the pairs of one read select, which the caller bounds by work_mem through the pairs */
#include "postgres.h"

#include "common/int.h"
#include "core/cmp.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/conn.h"
#include "col/scan_int.h"
#include "col/scope.h"
#include "col/wbuf.h"

/* the copies of one key make one unit up to this many rows */
#define DELTA_UNIT_ROWS		32768

typedef struct DeltaRow
{
	int64		key;
	SiftKey		addr;
	const SiftColBatch *b;
	uint32		row;
} DeltaRow;

typedef struct SiftScopeDelta
{
	int			scope;
	int			nsets;
	DeltaRow  **row;			/* [set]: the copies of the rows of the set, ascending by (key, address) */
	uint32	   *n;
	TransactionId *stamp;		/* what the scan stamps the rows of the set with */
	bool	   *roots;
	bool	   *lane;			/* the rows of the set are checked against the heap whatever the map says (a copy of the records as they were when the read began) */
	bool		kept[SIFT_COL_MAX_COLS];	/* the leaves the copies hold values of: the others are NULL in them, as in a scope segment of cols(..) */
} SiftScopeDelta;

/* the leaf of the fact index that holds the member key of the scope */
static int
fk_leaf(Relation index, int scope)
{
	/* check-memory: allow nothing reads the catalog between the fetch and the return */
	const SiftColRel *r = sift_col_rel(index);

	return r->first[r->schema->cols[r->scope_leaf[scope]].attno];
}

SiftScopeBuffer *
sift_scope_buffers(Relation index, const SiftDirSnapshot *dir, int *n)
{
	SiftScopeBuffer *all = palloc(Max(dir->nsegs, 1) * sizeof(SiftScopeBuffer));

	*n = 0;
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftScopeBuffer *b = &all[*n];

		if (!sift_seg_is_wbuf(&dir->segs[i]))
			continue;
		b->stamp = InvalidTransactionId;
		/* the schema of the relcache entry, which the scan's source reads the batches with: a set the backend keeps is for one */
		b->b = sift_cwbuf_batches_shared(index, &dir->segs[i], sift_col_rel(index)->schema, dir->version, CurrentMemoryContext, &b->n, NULL, &b->stamp);
		b->roots = (dir->segs[i].flags & SIFT_SEG_ROOTS) != 0;
		b->lane = true;
		b->rows = NULL;
		b->ids = NULL;
		(*n)++;
	}
	return all;
}

uint64
sift_scope_buffers_rows(const SiftScopeBuffer *bufs, int nbufs)
{
	uint64		rows = 0;

	for (int i = 0; i < nbufs; i++)
		for (int j = 0; j < bufs[i].n; j++)
			rows += bufs[i].b[j]->nrows;
	return rows;
}

/* the journal word of the string of each code of the dictionary of a batch of a string member */
static uint64 *
dict_words(const SiftCDict *dict)
{
	uint64	   *w = palloc(Max(dict->n, 1) * sizeof(uint64));

	for (uint32 i = 0; i < dict->n; i++)
	{
		uint32		len;
		const char *s = sift_cdict_str(dict, i, &len);

		w[i] = sift_member_hash_bytes(s, len);
	}
	return w;
}

int64 *
sift_scope_buffers_members(Relation index, int scope, const SiftScopeBuffer *bufs, int nbufs, SiftKeyMap *mm, uint32 *n)
{
	int			fk = fk_leaf(index, scope);
	int64	   *m = palloc(Max(sift_scope_buffers_rows(bufs, nbufs), 1) * sizeof(int64));

	*n = 0;
	for (int i = 0; i < nbufs; i++)
		for (int j = 0; j < bufs[i].n; j++)
		{
			const SiftColBatch *b = bufs[i].b[j];
			uint64	   *words = mm != NULL ? dict_words(b->dict[fk]) : NULL;

			for (uint32 r = 0; r < b->nrows; r++)
				if (!sift_cbatch_dead(b, r) && !sift_col_bit_test(b->col[fk].nulls, r))
					m[(*n)++] = words != NULL ? (int64) words[b->col[fk].key[r]] : b->col[fk].key[r];
			if (words != NULL)
				pfree(words);
		}
	return m;
}

/*
 * The member of each row of a batch, as the pairs name it: its key, or for a string member its code in mm, PG_INT64_MIN for a value no pair has (mm knows the members of the pairs the read listed, and a
 * value it does not know has none). A NULL row has none either.
 */
static const int64 *
batch_members(SiftKeyMap *mm, const SiftColBatch *b, int fk)
{
	int64	   *ids;

	if (mm == NULL)
		return b->col[fk].key;
	ids = palloc(Max(b->nrows, 1) * sizeof(int64));
	sift_keymap_batch_codes(mm, b->dict[fk], b->col[fk].key, b->col[fk].nulls, b->nrows, false, ids);
	return ids;
}

#define DELTA_LESS(x, y) ((x)->key != (y)->key ? (x)->key < (y)->key : (x)->addr < (y)->addr)
SIFT_DEFINE_SORT(delta_sort_cmp, DeltaRow, DELTA_LESS)

static void *
delta_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define DELTA_KEY(x) ((x)->key)
#define DELTA_NOKEY(x) 0
#define DELTA_ADDR(x) ((x)->addr)
/* the copies come in the order of their rows, which the buffers keep in the order of their addresses: the radix sort by key is stable, and the comparison sort takes any other order */
SIFT_DEFINE_RADIX_SORT(delta_sort, DeltaRow, DELTA_KEY, DELTA_NOKEY, DELTA_ADDR, delta_copy, pfree, delta_sort_cmp)

/* The first pair of the member in the pairs (ascending by member), n if it has none */
static uint32
first_pair(const SiftScopePair *p, uint32 n, int64 member)
{
	uint32		lo = 0,
				hi = n;

	SIFT_BISECT(uint32, m, lo, hi, p[m].member < member);
	return lo;
}

/* the member keys as a bitmap over their range, which most rows of a buffer or a unit miss: a row is tested in a few instructions before its pairs are looked for */
typedef struct Members
{
	int64		lo;
	uint64		span;
	uint64	   *bits;			/* NULL: the range is too wide for a bitmap, every row looks for its pairs */
} Members;

#define MEMBERS_BITS_MAX	((uint64) 1 << 24)

/* the ascending member keys that the n entries one stride apart from base start with */
static Members
members_of(const void *base, Size stride, uint32 n)
{
	int64		first = *(const int64 *) base,
				last = *(const int64 *) ((const char *) base + (n - 1) * stride);
	Members		m = {first, (uint64) last - (uint64) first + 1, NULL};

	if (m.span > MEMBERS_BITS_MAX)
		return m;
	m.bits = palloc0((m.span + 63) / 64 * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		sift_col_bit_set(m.bits, (uint64) *(const int64 *) ((const char *) base + i * stride) - (uint64) m.lo);
	return m;
}

static bool
members_has(const Members *m, int64 member)
{
	uint64		off = (uint64) member - (uint64) m->lo;

	return off < m->span && (m->bits == NULL || sift_col_bit_test(m->bits, off));
}

/*
 * The leaves a copy holds: those the scan reads (and their groups), the member key and the scope key, less those the scope segments keep no values of; encoding the others would cost each row a chunk of nothing.
 * The member of a string member is the one of the pairs (an id of the read, SiftScopeBuffer.ids): its string is kept only when the scan reads it, as copying it would intern it again for each row.
 */
static void
delta_kept(Relation index, int scope, int fk, bool str, const int *leaves, int nleaves, bool *kept)
{
	/* check-memory: allow the loops below read memory only */
	const SiftColRel *r = sift_col_rel(index);

	memset(kept, 0, SIFT_COL_MAX_COLS * sizeof(bool));
	for (int i = 0; i < nleaves; i++)
		if (leaves[i] < r->schema->ncols)
		{
			kept[leaves[i]] = true;
			if (r->schema->cols[leaves[i]].parent >= 0)
				kept[r->schema->cols[leaves[i]].parent] = true;
		}
	kept[fk] |= !str;
	for (int i = 0; i < r->schema->ncols; i++)
		kept[i] &= r->scope_cols[scope][i];
}

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
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_col: unit %u of segment %llu does not parse", u, (unsigned long long) seg->entry.gen)));
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

/*
 * The live rows of unit u (ascending) that may hold a member key: the cand[0..ncand) the point directory lists (rows of the unit), which the caller checks against the values it decodes, or, with no
 * directory (cand NULL), the rows of the unit whose member key is among the members, read from the leaf. NULL when none. A string member: *ids the member of each row returned
 */
static uint32 *
unit_rows_of(SegMembers *sm, SiftColSegment *seg, uint32 u, int fk, const uint32 *cand, uint32 ncand, uint32 *nout, int64 **ids)
{
	const SiftColSBRef *ur = &seg->sbdir[u];
	uint32	   *out = palloc(Max(cand != NULL ? ncand : ur->nrows, 1) * sizeof(uint32));
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
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_col: unit %u of segment %llu does not parse", u, (unsigned long long) seg->entry.gen)));
		sift_cseg_zone(seg, fk, u, &z);
		own = sm->mm != NULL && (z.flags & SIFT_COLZ_INLINE);
		mem = own ? sm->ids_mem : &sm->mem;
		m = own ? sm->ids : sm->m;
		n = own ? sm->nids : sm->n;
		if (v.chunk_len[fk] > 0)
			sift_cseg_unit_fill(seg, u, v.chunk_off[fk], v.chunk_len[fk]);
		if (v.chunk_len[fk] > 0 && sift_cunit_chunk(&v, fk, &c))
		{
			val = palloc((c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
			if (own)
				unit_member_ids(sm, &c, val);
			else
				sift_cchunk_decode(&c, val);
			for (uint32 r = 0; r < c.n; r++)
				if (member_of(mem, m, n, val[r]) && !(c.nulls != NULL && sift_col_bit_test(c.nulls, r)) && (live == NULL || sift_col_bit_test(live, r)))
					out[(*nout)++] = r;
			if (sm->mm != NULL && *nout > 0)
			{
				*ids = palloc(*nout * sizeof(int64));
				for (uint32 j = 0; j < *nout; j++)
					(*ids)[j] = own ? val[out[j]] : member_of_code(sm, val[out[j]]);
			}
			pfree(val);
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
		const int64 *member = ids[i] != NULL ? ids[i] : batch_members(mm, bs[i], fk);

		for (uint32 r = 0; r < rows; r++)
			blk[r] = sift_key_block(bs[i]->keys[r]);
		sift_vis_blocks_allvisible(vis, blk, rows, bits);
		for (uint32 r = 0; r < rows; r++)
		{
			int			k = sift_col_bit_test(bits, r) ? 1 : 0;

			if ((ids[i] == NULL && sift_col_bit_test(bs[i]->col[fk].nulls, r)) || !member_of(mem, m, n, member[r]))
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
	plan->fk = fk_leaf(index, scope);
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
	bool		kept[SIFT_COL_MAX_COLS];
	SiftVis		vis;

	*rows = *units = 0;
	delta_kept(index, scope, fk, plan->mm != NULL, leaves, nleaves, kept);
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
			sift_cseg_decode_unit_rows(seg, u, kept, ords, nord, b1);
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

SiftScopeDelta *
sift_scope_delta_select(Relation index, int scope, SiftKeyMap *mm, const SiftScopeBuffer *bufs, int nbufs, const SiftScopePair *pairs, uint32 npairs, const int *leaves, int nleaves, uint64 *rows)
{
	SiftScopeDelta *d = palloc0(sizeof(SiftScopeDelta));
	int			fk = fk_leaf(index, scope);
	Members		mem;

	*rows = 0;
	d->scope = scope;
	d->nsets = nbufs;
	delta_kept(index, scope, fk, mm != NULL, leaves, nleaves, d->kept);
	d->row = palloc0(Max(nbufs, 1) * sizeof(DeltaRow *));
	d->n = palloc0(Max(nbufs, 1) * sizeof(uint32));
	d->stamp = palloc(Max(nbufs, 1) * sizeof(TransactionId));
	d->roots = palloc(Max(nbufs, 1) * sizeof(bool));
	d->lane = palloc(Max(nbufs, 1) * sizeof(bool));
	for (int s = 0; s < nbufs; s++)
	{
		d->stamp[s] = bufs[s].stamp;
		d->roots[s] = bufs[s].roots;
		d->lane[s] = bufs[s].lane;
	}
	if (npairs == 0)
		return d;
	mem = members_of(pairs, sizeof(SiftScopePair), npairs);
	for (int s = 0; s < nbufs; s++)
	{
		uint32		cap = 0;

		for (int i = 0; i < bufs[s].n; i++)
		{
			const SiftColBatch *b = bufs[s].b[i];
			const int64 *ids = bufs[s].ids ? bufs[s].ids[i] : NULL;
			const int64 *member = ids != NULL ? ids : batch_members(mm, b, fk);
			const uint64 *take = bufs[s].rows ? bufs[s].rows[i] : NULL;

			for (uint32 r = 0; r < b->nrows; r++)
			{
				if ((take != NULL && !sift_col_bit_test(take, r)) || sift_cbatch_dead(b, r) || !members_has(&mem, member[r]) || (ids == NULL && sift_col_bit_test(b->col[fk].nulls, r)))
					continue;
				for (uint32 j = first_pair(pairs, npairs, member[r]); j < npairs && pairs[j].member == member[r]; j++)
				{
					if (d->n[s] == cap)
					{
						cap = Max(256, cap * 2);
						d->row[s] = d->row[s] ? repalloc(d->row[s], cap * sizeof(DeltaRow)) : palloc(cap * sizeof(DeltaRow));
					}
					d->row[s][d->n[s]++] = (DeltaRow) {pairs[j].key, b->keys[r], b, r};
				}
			}
			if (ids == NULL && mm != NULL)
				pfree((int64 *) member);
		}
		if (d->n[s] > 0)
			delta_sort(d->row[s], d->n[s]);
		*rows += d->n[s];
	}
	return d;
}

/*
 * The members the source leaves out of its stored units (ascending), as a bitmap over their range when it is not too wide: the integers of the member leaf, or the journal words of a string leaf, which
 * have no range worth a bitmap. And for a string member, the members each key's units do not count (the drop lists of the read: an integer member has them as a conjunct of the scan)
 */
static void
scope_except(SiftColSource *src, int leaf, int key_leaf, const SiftScopeRead *rd)
{
	MemoryContext old = MemoryContextSwitchTo(src->mcxt);
	bool		str = rd->mm != NULL;
	Members		m = {0, 0, NULL};

	if (!str && rd->nexcept > 0)
		m = members_of(rd->except, sizeof(int64), rd->nexcept);
	src->sx = memcpy(palloc(Max(rd->nexcept, 1) * sizeof(int64)), rd->except, rd->nexcept * sizeof(int64));
	src->nsx = rd->nexcept;
	src->sx_leaf = leaf;
	src->sx_str = str;
	src->sxb = m.bits;
	src->sx_span = m.span;
	if (str && rd->ndrop > 0)
	{
		src->sd = palloc(rd->ndrop * sizeof(ColDrop));
		for (int i = 0; i < rd->ndrop; i++)
			src->sd[i] = (ColDrop) {rd->drop[i].key, rd->drop[i].n, rd->drop[i].members};
		src->nsd = rd->ndrop;
		src->sd_leaf = key_leaf;
		src->sd_map = rd->mm;
	}
	src->sx_on = src->nsx > 0 || src->nsd > 0;
	MemoryContextSwitchTo(old);
}

/* The sets of b after those of a (the rows of the write buffers, then those of the segments), a result of its own */
SiftScopeDelta *
sift_scope_delta_merge(const SiftScopeDelta *a, const SiftScopeDelta *b)
{
	SiftScopeDelta *d = palloc0(sizeof(SiftScopeDelta));
	int			n = a->nsets + b->nsets;

	*d = *a;
	d->nsets = n;
	d->row = palloc(Max(n, 1) * sizeof(DeltaRow *));
	d->n = palloc(Max(n, 1) * sizeof(uint32));
	d->stamp = palloc(Max(n, 1) * sizeof(TransactionId));
	d->roots = palloc(Max(n, 1) * sizeof(bool));
	d->lane = palloc(Max(n, 1) * sizeof(bool));
	for (int i = 0; i < n; i++)
	{
		const SiftScopeDelta *from = i < a->nsets ? a : b;
		int			k = i < a->nsets ? i : i - a->nsets;

		d->row[i] = from->row[k];
		d->n[i] = from->n[k];
		d->stamp[i] = from->stamp[k];
		d->roots[i] = from->roots[k];
		d->lane[i] = from->lane[k];
	}
	return d;
}

void
sift_col_scope_delta(SiftColSource *src, const SiftScopeRead *rd)
{
	const SiftColRel *r = sift_col_rel(src->index);	/* check-memory: allow nothing below reads the catalog */
	int			leaf = r->scope_leaf[rd->scope];

	if (rd->nexcept > 0 || (rd->mm != NULL && rd->ndrop > 0))
		scope_except(src, fk_leaf(src->index, rd->scope), leaf, rd);
	if (rd->delta_rows == 0)
		return;
	/* the batches are copies: the state of the write buffer they came from tells nothing about them (as for sift_col_add_batches) */
	src->wtok = NULL;
	src->nwtok = -1;
	for (int s = 0; s < rd->delta->nsets; s++)
	{
		const DeltaRow *row = rd->delta->row[s];
		uint32		n = rd->delta->n[s];

		for (uint32 a = 0, e; a < n; a = e)
		{
			SiftColBatch *b = sift_cbatch_create(src->mcxt, src->schema);

			b->keyed = true;
			b->sb = src->heap_sbs++;
			for (e = a; e < n && row[e].key == row[a].key && e - a < DELTA_UNIT_ROWS; e++)
				sift_cbatch_add_scoped(b, row[e].b, row[e].row, leaf, row[e].key, rd->delta->kept);
			scan_add_mem(src, b, rd->delta->lane[s], rd->delta->stamp[s], rd->delta->roots[s]);
		}
	}
}
