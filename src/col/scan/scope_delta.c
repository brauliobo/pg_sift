/*
 * col/scan/scope_delta.c - the rows of the write buffers of a scope index as rows of its scope units (col/scope.h)
 *
 * No scope entry describes a row of a write buffer, so a scan of the scope units reads them from the buffers: each live row whose member has a pair among those the reader
 * listed (the pairs under the statement snapshot, of the members that did not change) is copied once for each of them, with the scope key in the derived leaf, as the promotion would
 * write it. The reader selects the rows (sift_scope_delta_select: it knows then how many there are, which prices the read), the source makes the copies of one key one in-memory unit
 * (numbered after the stored units, rows in address order): what the scan reads of it is what it reads of a stored unit, and the rows keep their heap addresses, so the visibility check
 * and the tombstones are the ones of the write buffer's rows.
 */
/* check-memory: allow-allocs the arrays follow the rows of the write buffers the pairs of one read select, which the caller bounds by work_mem through the pairs */
#include "postgres.h"

#include "core/cmp.h"
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
	bool		kept[SIFT_COL_MAX_COLS];	/* the leaves the copies hold values of: the others are NULL in them, as in a scope segment of cols(..) */
} SiftScopeDelta;

/* the leaf of the fact index that holds the member key of the scope */
static int
fk_leaf(Relation index, int scope)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

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

int64 *
sift_scope_buffers_members(Relation index, int scope, const SiftScopeBuffer *bufs, int nbufs, uint32 *n)
{
	int			fk = fk_leaf(index, scope);
	int64	   *m = palloc(Max(sift_scope_buffers_rows(bufs, nbufs), 1) * sizeof(int64));

	*n = 0;
	for (int i = 0; i < nbufs; i++)
		for (int j = 0; j < bufs[i].n; j++)
			for (uint32 r = 0; r < bufs[i].b[j]->nrows; r++)
				if (!sift_col_bit_test(bufs[i].b[j]->col[fk].nulls, r))
					m[(*n)++] = bufs[i].b[j]->col[fk].key[r];
	return m;
}

static int
delta_cmp(const void *a, const void *b)
{
	const DeltaRow *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return SIFT_CMP3(x->addr, y->addr);
}

/* The first pair of the member in the pairs (ascending by member), n if it has none */
static uint32
first_pair(const SiftScopePair *p, uint32 n, int64 member)
{
	uint32		lo = 0,
				hi = n;

	SIFT_BISECT(uint32, m, lo, hi, p[m].member < member);
	return lo;
}

/* the members of the pairs as a bitmap over their range, which most rows of a buffer miss: a row is tested in a few instructions before its pairs are looked for */
typedef struct Members
{
	int64		lo;
	uint64		span;
	uint64	   *bits;			/* NULL: the range is too wide for a bitmap, every row looks for its pairs */
} Members;

#define MEMBERS_BITS_MAX	((uint64) 1 << 24)

static Members
members_of(const SiftScopePair *p, uint32 n)
{
	Members		m = {p[0].member, (uint64) p[n - 1].member - (uint64) p[0].member + 1, NULL};

	if (m.span > MEMBERS_BITS_MAX)
		return m;
	m.bits = palloc0((m.span + 63) / 64 * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		sift_col_bit_set(m.bits, (uint64) p[i].member - (uint64) m.lo);
	return m;
}

static bool
members_has(const Members *m, int64 member)
{
	uint64		off = (uint64) member - (uint64) m->lo;

	return off < m->span && (m->bits == NULL || sift_col_bit_test(m->bits, off));
}

/* The leaves a copy holds: those the scan reads (and their groups), the member key and the scope key, less those the scope segments keep no values of; encoding the others would cost each row a chunk of nothing */
static void
delta_kept(Relation index, int scope, int fk, const int *leaves, int nleaves, bool *kept)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	memset(kept, 0, SIFT_COL_MAX_COLS * sizeof(bool));
	for (int i = 0; i < nleaves; i++)
		if (leaves[i] < r->schema->ncols)
		{
			kept[leaves[i]] = true;
			if (r->schema->cols[leaves[i]].parent >= 0)
				kept[r->schema->cols[leaves[i]].parent] = true;
		}
	kept[fk] = true;
	for (int i = 0; i < r->schema->ncols; i++)
		kept[i] &= r->scope_cols[scope][i];
}

SiftScopeDelta *
sift_scope_delta_select(Relation index, int scope, const SiftScopeBuffer *bufs, int nbufs, const SiftScopePair *pairs, uint32 npairs, const int *leaves, int nleaves, uint64 *rows)
{
	SiftScopeDelta *d = palloc0(sizeof(SiftScopeDelta));
	int			fk = fk_leaf(index, scope);
	Members		mem;

	*rows = 0;
	d->scope = scope;
	d->nsets = nbufs;
	delta_kept(index, scope, fk, leaves, nleaves, d->kept);
	d->row = palloc0(Max(nbufs, 1) * sizeof(DeltaRow *));
	d->n = palloc0(Max(nbufs, 1) * sizeof(uint32));
	d->stamp = palloc(Max(nbufs, 1) * sizeof(TransactionId));
	d->roots = palloc(Max(nbufs, 1) * sizeof(bool));
	for (int s = 0; s < nbufs; s++)
	{
		d->stamp[s] = bufs[s].stamp;
		d->roots[s] = bufs[s].roots;
	}
	if (npairs == 0)
		return d;
	mem = members_of(pairs, npairs);
	for (int s = 0; s < nbufs; s++)
	{
		uint32		cap = 0;

		for (int i = 0; i < bufs[s].n; i++)
		{
			const SiftColBatch *b = bufs[s].b[i];
			const SiftCVals *v = &b->col[fk];

			for (uint32 r = 0; r < b->nrows; r++)
			{
				if (!members_has(&mem, v->key[r]) || sift_col_bit_test(v->nulls, r))
					continue;
				for (uint32 j = first_pair(pairs, npairs, v->key[r]); j < npairs && pairs[j].member == v->key[r]; j++)
				{
					if (d->n[s] == cap)
					{
						cap = Max(256, cap * 2);
						d->row[s] = d->row[s] ? repalloc(d->row[s], cap * sizeof(DeltaRow)) : palloc(cap * sizeof(DeltaRow));
					}
					d->row[s][d->n[s]++] = (DeltaRow) {pairs[j].key, b->keys[r], b, r};
				}
			}
		}
		if (d->n[s] > 0)
			qsort(d->row[s], d->n[s], sizeof(DeltaRow), delta_cmp);
		*rows += d->n[s];
	}
	return d;
}

void
sift_col_scope_delta(SiftColSource *src, const SiftScopeRead *rd)
{
	const SiftColRel *r = sift_col_rel(src->index);	/* check-memory: allow nothing below reads the catalog */
	int			leaf = r->scope_leaf[rd->scope];

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
			scan_add_mem(src, b, true, rd->delta->stamp[s], rd->delta->roots[s]);
		}
	}
}
