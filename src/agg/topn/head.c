/*
 * agg/topn/head.c - the heads of a superblock and the order of the dictionary
 * The heads a superblock keeps, the order codes of a dictionary, and the unit of blob keys
 * read through them.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/topn/int.h"

/* The bar as a code (on: false when there is none, or it is NULL, which is compared as a NULL is): a row is strictly after it when its code is above ob.v (below it, for DESC) */
typedef struct OrdBar
{
	bool		on;
	uint64		v;
} OrdBar;

/* The entries of the head from the end the sort wants go to the result, up to the limit and the bar; false when the head is too short to answer */
static bool
heads_take(SiftTopN *t, const SiftSrcHead *lo, uint32 nlo, const SiftSrcHead *hi, uint32 nhi, uint16 part)
{
	const TopKey *k0 = &t->key[0];
	const SiftSrcHead *h = k0->desc ? hi : lo;
	uint32		n = k0->desc ? nhi : nlo;
	Rank		bar;
	bool		barred = bar_rank(t, &bar);

	if (n < (uint32) t->limit && !(barred && n > 0 && rank_after(rank_of(k0, false, h[n - 1].key), bar)))
		return false;
	for (uint32 i = 0; i < Min(n, (uint32) t->limit); i++)
	{
		TopEnt		e = {0};

		if (barred && rank_after(rank_of(k0, false, h[i].key), bar))
			break;
		e.k[0] = h[i].key;
		ItemPointerSet(&e.tid, (BlockNumber) (h[i].addr >> 16), (OffsetNumber) (h[i].addr & 0xFFFF));
		e.part = part;
		e.tie = tie_key(&e.tid, part);
		res_push(t, &e, NULL, NULL);
	}
	share_bar(t);
	t->nrhead++;
	return true;
}

/*
 * A unit of which every row passes, asked for at most as many rows as the head of the run of the first key holds: the n best rows of the unit
 * are the first n entries of the head from the end the sort wants, with their keys and heap addresses, so the unit is not read. Asked for more, the head still
 * answers once the heap is full and the last entry of the head is after its bar: the rows after the head are not before that entry, so none can enter the result.
 * A row that is dead or changed since the unit was written fails the mask (the visibility check clears its bit), and the unit is read as before.
 */
bool
head_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow)
{
	const TopKey *k0 = &t->key[0];
	SiftSrcHead lo[SIFT_SRC_HEAD_MAX],
				hi[SIFT_SRC_HEAD_MAX];
	uint32		nlo,
				nhi,
				nvals,
				nnull;
	uint64		id;

	if (!heads_ok(t))
		return false;
	for (int i = 0; i < nrow / 64; i++)
		if (rmask[i] != ~UINT64CONST(0))
			return false;
	if (nrow % 64 && rmask[nrow / 64] != (UINT64CONST(1) << (nrow % 64)) - 1)
		return false;
	if (!sift_colsrc_run_info(src, unit, k0->leaf, false, &nvals, &nnull, &id) || nvals != (uint32) nrow || nnull != 0)
		return false;
	if (sift_colsrc_run_heads(src, unit, k0->leaf, lo, &nlo, hi, &nhi) == 0)
		return false;
	return heads_take(t, lo, nlo, hi, nhi, unit_part(t, src));
}

bool
sift_topn_sb_head(SiftTopN *t, SiftSrcSource *src, uint32 sb)
{
	const TopKey *k0 = &t->key[0];
	SiftSrcHead lo[SIFT_SRC_HEAD_MAX],
				hi[SIFT_SRC_HEAD_MAX];
	uint32		nlo,
				nhi;

	if (!heads_ok(t))
		return false;
	if (!t->wkeep || walk_in_head(t, sb) || !sift_colsrc_sb_heads(src, sb, k0->leaf, lo, &nlo, hi, &nhi))
		return false;
	return heads_take(t, lo, nlo, hi, nhi, 0);
}

/* the first 8 bytes of an entry (after the varlena header of a bytea), most significant first, zero padded: it orders the entries as they are up to a tie */
static uint64
key_prefix(uint8 bkind, const char *p, uint32 len)
{
	uint64		k = 0;
	uint32		m;

	if (bkind == BK_BYTEA)
	{
		varattrib_4b h;

		/* an entry of the dictionary is padded to its alignment: the bytes after the varlena image are not part of the value (key_entry) */
		if (len >= sizeof(uint32))
		{
			memcpy(&h, p, sizeof(uint32));
			len = Min(len, VARSIZE_4B(&h));
		}
		p += Min(len, VARHDRSZ);
		len -= Min(len, VARHDRSZ);
	}
	m = Min(len, 8);
	for (uint32 i = 0; i < 8; i++)
		k = k << 8 | (i < m ? (uint8) p[i] : 0);
	return k;
}

/* The order code of the dictionary d for a unit with npass rows to compare (NULL: none, they are compared as bytes) */
static OrdCode *
unit_ord(SiftTopN *t, const SiftSrcDict *d, int64 npass)
{
	const TopKey *k = &t->key[0];
	OrdCode    *oc = NULL;
	const uint32 *sorted;

	if (k->leaf < 0 || k->x || k->xfn || d->n == 0 || d->n > sift_limit(topn_code_entries, ORD_MAX_ENTRIES) ||
		!((k->bkind == BK_TEXT && sift_collate_is_c(k->collid)) || k->bkind == BK_UUID || k->bkind == BK_BYTEA))
		return NULL;
	for (int i = 0; i < ORD_CACHE && oc == NULL; i++)
		if (t->oc[i].id == d->id && t->oc[i].bytes == d->bytes && t->oc[i].n == d->n)
			oc = &t->oc[i];
	if (oc == NULL)
	{
		oc = &t->oc[t->oc_next++ % ORD_CACHE];
		if (oc->ord)
			pfree(oc->ord);
		*oc = (OrdCode) {.id = d->id,.bytes = d->bytes,.n = d->n};
	}
	if (oc->exact)
		return oc;
	oc->rows += npass;
	/* the order of a dictionary is that of its entries as bytes: a bytea entry is a varlena image, which starts with its length, not the order of byteacmp */
	sorted = d->order && k->bkind != BK_BYTEA ? d->order(d->octx, npass) : NULL;
	if (oc->rows < d->n * ORD_PREFIX_ROWS || (sorted == NULL && oc->useless))
		return NULL;
	if (sorted == NULL && oc->ord)
		return oc;
	if (t->ocxt == NULL)
		t->ocxt = AllocSetContextCreate(MemoryContextGetParent(t->ucxt), "sift top-N order codes", ALLOCSET_DEFAULT_SIZES);
	if (oc->ord == NULL)
		oc->ord = MemoryContextAlloc(t->ocxt, (Size) d->n * sizeof(uint64));
	if (sorted)
	{
		oc->exact = true;
		for (uint32 i = 0; i < d->n; i++)
			oc->ord[sorted[i]] = i + 1;
	}
	else
		for (uint32 c = 0; c < d->n; c++)
			oc->ord[c] = key_prefix(k->bkind, d->bytes + d->off[c], d->off[c + 1] - d->off[c]);
	return oc;
}

/* the codes of the dictionary in byte order, for an exact code: the dictionary has them, or sorts now (the cache may have parsed it again since the code was made) */
static const uint32 *
dict_sorted(const SiftSrcDict *d)
{
	const uint32 *sorted = d->order(d->octx, 0);

	return sorted ? sorted : d->order(d->octx, (uint64) d->n * ORD_FORCE_ROWS);
}

/* the entry with this code compared with the bytes of a key, as the sort does */
static int
code_vs_blob(SiftTopN *t, const SiftSrcDict *d, uint32 code, const KeyBlob *b)
{
	const char *p;
	uint32		len;

	key_entry(t, 0, d, code, &p, &len);
	return bytes_cmp(&t->key[0], p, len, b->data, b->len);
}

static void
ord_bar(SiftTopN *t, OrdBar *ob, const OrdCode *oc, const SiftSrcDict *d, const uint32 *code, const TopEnt *bar)
{
	const TopKey *k = &t->key[0];

	ob->on = oc != NULL && bar != NULL && !(bar->nul & 1);
	if (!ob->on)
		return;
	if (bar == &t->lh[0])
		ob->v = oc->ord[code[bar->row]];		/* the unit's n-th: an entry of this dictionary */
	else if (oc->exact)
	{
		/* an entry of another unit: its place among the entries, after the last one that is not above it (ASC) or the last one below it (DESC) */
		const KeyBlob *b = KEY_BLOB(bar->k[0]);
		const uint32 *sorted = dict_sorted(d);
		uint32		lo = 0,
					hi = d->n;

		SIFT_BISECT(uint32, m, lo, hi, k->desc ? code_vs_blob(t, d, sorted[m], b) < 0 : code_vs_blob(t, d, sorted[m], b) <= 0);
		ob->v = lo + (k->desc ? 1 : 0);
	}
	else
		ob->v = key_prefix(k->bkind, KEY_BLOB(bar->k[0])->data, KEY_BLOB(bar->k[0])->len);
}

/* Is the row strictly after the bar? By the codes unless they tie (not exact), then as bytes; without a code, as after_bar does */
static inline bool
row_after(SiftTopN *t, const OrdCode *oc, const OrdBar *ob, uint8 *worse, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	if (oc && ob->on && !isnull)
	{
		uint64		a = oc->ord[code];

		if (a != ob->v)
			return t->key[0].desc ? a < ob->v : a > ob->v;
		if (oc->exact)
			return false;
		t->nties++;
		return vs_bar(t, d, false, code, bar) > 0;
	}
	return worse ? after_bar(t, worse, d, isnull, code, bar) : vs_bar(t, d, isnull, code, bar) > 0;
}

/* Does the row come after the unit's n-th, or tie with it (the rows are visited in order, so a tie comes after)? */
static inline bool
row_not_before(SiftTopN *t, const OrdCode *oc, const uint32 *codes, const SiftSrcDict *d, bool isnull, uint32 code)
{
	const TopEnt *l0 = &t->lh[0];

	if (oc && !isnull && !(l0->nul & 1))
	{
		uint64		a = oc->ord[code],
					c = oc->ord[codes[l0->row]];

		if (a != c)
			return t->key[0].desc ? a < c : a > c;
		if (oc->exact)
			return true;
		t->nties++;
	}
	return vs_bar(t, d, isnull, code, l0) >= 0;
}

/*
 * sift_topn_unit for a first key of byte strings: the bar is the heap's worst entry or the unit's n-th so far (whichever sorts first), and the rows are
 * compared with it by the entry of the dictionary, as the type's comparison says, or by the order code of the dictionary when it has one.
 */
void
unit_blob(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	const uint32 *code = key0->data;
	const TopEnt *bar = t->n == t->limit ? &t->heap[0] : NULL;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;
	int64		npass = 0;
	OrdCode    *oc;
	uint8	   *worse = NULL;
	OrdBar		ob;

	for (int w = 0; w < nw; w++)
		npass += pg_popcount64(rmask[w]);
	oc = unit_ord(t, key0->dict, npass);
	/* a string that was after the bar is remembered by its code: worth the byte a code when the rows of the unit are not few against the entries */
	if (oc == NULL && npass * 8 >= key0->dict->n)
		worse = MemoryContextAllocZero(t->ucxt, Max(key0->dict->n, 1));
	t->nord[oc ? (oc->exact ? 0 : 1) : 2]++;
	t->nties = 0;
	grow_unit_buffers(t, nrow);
	t->ln = 0;
	ord_bar(t, &ob, oc, key0->dict, code, bar);
	for (int w = 0; w < nw; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			int			i = w * 64 + pg_rightmost_one_pos64(b);
			bool		isnull = key0->isnull && key0->isnull[i];
			TopEnt		e;

			if (bar && row_after(t, oc, &ob, worse, key0->dict, isnull, code[i], bar))
				continue;
			t->crow[m] = i;
			t->ck[m] = code[i];
			t->cnul[m++] = isnull;
			/* a row that ties with the unit's n-th stays a candidate, but it comes after it: the rows are visited in order */
			if (t->ln == t->limit && row_not_before(t, oc, code, key0->dict, isnull, code[i]))
				continue;
			memset(&e, 0, sizeof(e));
			e.nul = isnull;
			e.row = e.tie = i;
			if (!isnull)
			{
				const char *p;
				uint32		len;

				key_entry(t, 0, key0->dict, code[i], &p, &len);
				e.k[0] = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
			}
			heap_push(t, t->lh, &t->ln, &e);
			if (t->ln == t->limit && (bar == NULL || key_cmp_n(t, &t->lh[0], bar, 1) < 0))
				bar = &t->lh[0];
			if (bar == &t->lh[0])
				ord_bar(t, &ob, oc, key0->dict, code, bar);
		}
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (bar && row_after(t, oc, &ob, worse, key0->dict, t->cnul[q], t->ck[q], bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->nul = t->cnul[q];
		e->row = t->cand[nc] = t->crow[q];
		e->tie = e->row;
		if (!t->cnul[q])
		{
			const char *p;
			uint32		len;

			key_entry(t, 0, key0->dict, t->ck[q], &p, &len);
			e->k[0] = (int64) (uintptr_t) blob_new(t->ucxt, p, len);
		}
		nc++;
	}
	if (oc && !oc->exact && t->nties * 4 > npass)
		oc->useless = true;
	if (nc > 0)
		finish_unit(t, src, unit, nc);
}

/*
 * Sorted runs in a walk over the units: the first pass takes about 2 n / units rows of each (their best, so that together they hold the n best of all but
 * for the odd unit with more), which fills the heap with a bar close to the last, and leaves the units that have more rows below it to a second pass,
 * which reads only them (sift_topn_revisit_next) and takes the rows after the ones taken. One pass over the units, each at most as far as the n-th row
 * of the heap in the end needs, where one unit after the other would take n / k rows of the k-th.
 */
void
sift_topn_run_budget(SiftTopN *t, uint32 units)
{
	int			b;

	if (!sift_topn_runs || t->key[0].xk != TXK_NONE || t->key[0].leaf < 0 || t->all || t->sort || t->limit <= 0 || units < 8)
		return;
	b = Max(2, (int) (((uint64) t->limit * 2 + units - 1) / units));
	if (b * 2 < t->limit)
		t->budget = b;
}
