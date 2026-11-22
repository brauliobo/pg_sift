/*
 * col/v1/scan/scope_delta.c - the rows no scope entry describes, as rows of the scope units of a scope index (col/scope.h)
 *
 * No scope entry describes a row of a write buffer, nor a row of a member whose pairs changed since the stamp of the scope units (its stored entries are left out of the read): a scan of
 * the scope units reads them from the buffers and from the entity segments, each live row whose member has a pair among those the reader listed (the pairs under the statement
 * snapshot) copied once for each of them, with the scope key in the derived leaf, as the promotion would write it. The reader selects the rows (scope_segrows.c reads those of the
 * changed members by member key; sift_scope_delta_select expands them: it knows then how many there are, which prices the read), the source makes the copies of one key one in-memory unit
 * (numbered after the stored units, rows in address order): what the scan reads of it is what it reads of a stored unit, and the rows keep their heap addresses, so the visibility check
 * and the tombstones are the ones of the rows they copy.
 */
/* check-memory: allow-allocs the arrays follow the rows of the write buffers the pairs of one read select, which the caller bounds by work_mem through the pairs */
#include "postgres.h"

#include "common/int.h"
#include "core/cmp.h"
#include "col/v1/seg.h"
#include "core/visibility.h"
#include "col/v1/conn.h"
#include "col/v1/scan_int.h"
#include "col/v1/scan/scope_pairs.h"
#include "col/scope.h"
#include "col/v1/wbuf.h"

/* the copies of one key make one unit up to this many rows */
#define DELTA_UNIT_ROWS		32768
/* the least rows of a run of one key that make a unit of their own: a unit costs about what the sort of this many rows does */
#define DELTA_RUN_ROWS		512

typedef struct DeltaRow
{
	SiftColRowRef ref;			/* first: sift_cbatch_add_scoped_rows takes the rows of a run as refs */
	int64		key;
	SiftKey		addr;
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
int
sift_scope_fk_leaf(Relation index, int scope)
{
	/* check-memory: allow nothing reads the catalog between the fetch and the return */
	const SiftColRel *r = sift_col_rel(index);

	return r->first[r->schema->cols[r->scope_leaf[scope]].attno];
}

/* The leaves of a read and the member leaf of the scope: the drop lists of an integer member are conjuncts of the scan, which test it in the rows of the patch and of the delta as in the units */
const int *
sift_scope_leaves_with_member(Relation index, int scope, const int *leaves, int *nleaves)
{
	int		   *l = memcpy(palloc((*nleaves + 1) * sizeof(int)), leaves, *nleaves * sizeof(int));

	l[(*nleaves)++] = sift_scope_fk_leaf(index, scope);
	return l;
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
	int			fk = sift_scope_fk_leaf(index, scope);
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
const int64 *
sift_scope_batch_members(SiftKeyMap *mm, const SiftColBatch *b, int fk)
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
#define DELTA_ADDR(x) ((int64) (x)->addr)
#define DELTA_NOTIE(x) 0
/* by key, then address, as DELTA_LESS: the rows of a patch come unit by unit, not in the order of their addresses, which the radix sort does not need */
SIFT_DEFINE_RADIX_SORT(delta_sort, DeltaRow, DELTA_KEY, DELTA_ADDR, DELTA_NOTIE, delta_copy, pfree, delta_sort_cmp)

/* The rows of a set, as they are made, and the places in them where one is before the one ahead of it by (key, address): none when they are in order */
typedef struct DeltaOut
{
	DeltaRow   *row;
	uint32		n;
	uint32		cap;
	uint32		descents;
} DeltaOut;

/*
 * The rows of a set in the order of (key, address). The units of a segment that is cut by key overlap in their addresses, so the rows of a key come as a run of each unit; where each run is long
 * enough to be a unit of its own (sift_col_scope_delta starts one where the addresses fall) the runs are left as they are, as a unit costs less than the sort of the rows it holds.
 */
static void
delta_order(DeltaOut *o, bool onekey)
{
	if (o->descents == 0 || (onekey && o->n / (o->descents + 1) >= DELTA_RUN_ROWS))
		return;
	delta_sort(o->row, o->n);
}

/*
 * The leaves a copy holds: those the scan reads (and their groups) and the scope key, less those the scope segments keep no values of; encoding the others would cost each row a chunk of nothing.
 * The member of a row of the patch is the one of the pairs (an id of the read, SiftScopeBuffer.ids): its leaf is kept only when the scan reads it (the leaves say: a conjunct that tests it is one), as
 * decoding it and copying it would cost a chunk for each unit of the patch.
 */
void
sift_scope_delta_kept(Relation index, int scope, const int *leaves, int nleaves, bool *kept)
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
	for (int i = 0; i < r->schema->ncols; i++)
		kept[i] &= r->scope_cols[scope][i];
}

/* The copies of the row r of b, of member member: one for each pair of the member (a NULL member, or a dead row, has none) */
static inline void
delta_row(DeltaOut *o, const PairFirsts *f, const SiftScopePair *pairs, uint32 npairs, const SiftColBatch *b, uint32 r, int64 member, const uint64 *nulls)
{
	if (sift_cbatch_dead(b, r) || (nulls != NULL && sift_col_bit_test(nulls, r)))
		return;
	for (uint32 j = pairs_of_member(f, pairs, npairs, member); j < npairs && pairs[j].member == member; j++)
	{
		DeltaRow	row = {{b, r}, pairs[j].key, b->keys[r]};

		if (o->n == o->cap)
		{
			o->cap = Max(256, o->cap * 2);
			o->row = repalloc(o->row, o->cap * sizeof(DeltaRow));
		}
		o->descents += o->n > 0 && DELTA_LESS(&row, &o->row[o->n - 1]);
		o->row[o->n++] = row;
	}
}

SiftScopeDelta *
sift_scope_delta_select(Relation index, int scope, SiftKeyMap *mm, const SiftScopeBuffer *bufs, int nbufs, const SiftScopePair *pairs, uint32 npairs, const int *leaves, int nleaves, uint64 *rows)
{
	SiftScopeDelta *d = palloc0(sizeof(SiftScopeDelta));
	int			fk = sift_scope_fk_leaf(index, scope);
	PairFirsts	firsts;
	bool		onekey = true;
	DeltaOut	o;

	*rows = 0;
	d->scope = scope;
	d->nsets = nbufs;
	sift_scope_delta_kept(index, scope, leaves, nleaves, d->kept);
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
	firsts = pairs_firsts(pairs, npairs);
	for (uint32 i = 1; i < npairs; i++)
		onekey &= pairs[i].key == pairs[0].key;
	for (int s = 0; s < nbufs; s++)
	{
		uint32		cap = 1;

		/* a copy for each row of the patch, as most members have one pair: the array is not moved by a growth; the rows of a buffer that match are not known, a few are expected */
		for (int i = 0; i < bufs[s].n; i++)
		{
			const uint64 *take = bufs[s].rows ? bufs[s].rows[i] : NULL;

			cap += take == NULL ? Min(bufs[s].b[i]->nrows, 1024) : (uint32) sift_colk->bits_count(take, (bufs[s].b[i]->nrows + 63) / 64);
		}
		o = (DeltaOut) {palloc(cap * sizeof(DeltaRow)), 0, cap, 0};
		for (int i = 0; i < bufs[s].n; i++)
		{
			const SiftColBatch *b = bufs[s].b[i];
			const int64 *ids = bufs[s].ids ? bufs[s].ids[i] : NULL;
			const int64 *member = ids != NULL ? ids : sift_scope_batch_members(mm, b, fk);
			const uint64 *take = bufs[s].rows ? bufs[s].rows[i] : NULL;
			const uint64 *nulls = ids == NULL ? b->col[fk].nulls : NULL;

			if (take == NULL)
				for (uint32 r = 0; r < b->nrows; r++)
					delta_row(&o, &firsts, pairs, npairs, b, r, member[r], nulls);
			else
				for (uint32 w = 0; w < (b->nrows + 63) / 64; w++)
					for (uint64 bits = take[w]; bits != 0; bits &= bits - 1)
					{
						uint32		r = w * 64 + pg_rightmost_one_pos64(bits);

						delta_row(&o, &firsts, pairs, npairs, b, r, member[r], nulls);
					}
			if (ids == NULL && mm != NULL)
				pfree((int64 *) member);
		}
		if (o.n > 0)
			delta_order(&o, onekey);
		d->row[s] = o.row;
		d->n[s] = o.n;
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
		scope_except(src, sift_scope_fk_leaf(src->index, rd->scope), leaf, rd);
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
			for (e = a; e < n && row[e].key == row[a].key && e - a < DELTA_UNIT_ROWS && (e == a || row[e].addr > row[e - 1].addr); e++)
				;
			sift_cbatch_add_scoped_rows(b, &row[a].ref, sizeof(DeltaRow), e - a, leaf, row[a].key, rd->delta->kept);
			scan_add_mem(src, b, rd->delta->lane[s], rd->delta->stamp[s], rd->delta->roots[s]);
		}
	}
}
