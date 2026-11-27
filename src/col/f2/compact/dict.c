/*
 * col/compact_dict.c - the string columns of a compaction whose input parts all code them by a dictionary of their part (SEC_DICTS). The dictionaries are merged once (each is in byte order,
 * so the merge compares their entries, not the values of the rows) and the rows are read as the codes of the merged dictionary: no string of a row is copied, hashed or compared.
 * What is written is what a build from the strings would write: the dictionary of the strings that are left (in byte order) when it pays, else the strings themselves.
 * check-memory: allow-allocs each array follows the rows of one compaction, which its row buffer already holds in memory
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"
#include "col/granule/int.h"

bool		sift_compact_dict_remap = true;

static int
entry_cmp(const SiftPartDict *a, uint32 ia, const SiftPartDict *b, uint32 ib)
{
	uint32		la = a->off[ia + 1] - a->off[ia],
				lb = b->off[ib + 1] - b->off[ib];
	int			c = memcmp(a->bytes + a->off[ia], b->bytes + b->off[ib], Min(la, lb));

	return c != 0 ? c : (la > lb) - (la < lb);
}

/* The parts ordered by the entry each is at, the least first: a binary heap of part numbers, so that the next entry of the merge costs O(log parts) */
typedef struct DictHeap
{
	const SiftPartDict *const *in;
	const uint32 *cur;
	int		   *part;
	int			n;
} DictHeap;

static bool
heap_before(const DictHeap *h, int a, int b)
{
	int			c = entry_cmp(h->in[a], h->cur[a], h->in[b], h->cur[b]);

	return c < 0 || (c == 0 && a < b);
}

static void
heap_down(DictHeap *h, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					least = i;

		if (l < h->n && heap_before(h, h->part[l], h->part[least]))
			least = l;
		if (l + 1 < h->n && heap_before(h, h->part[l + 1], h->part[least]))
			least = l + 1;
		if (least == i)
			return;
		int			t = h->part[i];

		h->part[i] = h->part[least];
		h->part[least] = t;
		i = least;
	}
}

/* MEM[MEM-COL-09]: f2 dict merge table (max-part initial cap, grown by doubling, shrunk to the merged size) */
/* The merge of the dictionaries in[0 .. nparts) of column c into m->dict[c], and the map of the codes of each */
static void
merge_column(SiftDictMerge *m, int c, const SiftPartDict *const *in)
{
	uint32		ocap = 1;
	Size		bcap = 1;
	uint32	   *cur = palloc0(m->nparts * sizeof(uint32));
	SiftPartDict *out = &m->dict[c];
	DictHeap	h = {in, cur, palloc(m->nparts * sizeof(int)), 0};

	for (int k = 0; k < m->nparts; k++)
	{
		ocap = Max(ocap, in[k]->n);
		bcap = Max(bcap, in[k]->off[in[k]->n]);
		m->map[k * m->ncols + c] = (SiftCodeMap) {palloc(in[k]->n * sizeof(uint32)), in[k]->n};
		if (in[k]->n > 0)
			h.part[h.n++] = k;
	}
	for (int i = h.n / 2 - 1; i >= 0; i--)
		heap_down(&h, i);
	out->col = c;
	out->off = palloc((ocap + 1) * sizeof(uint32));
	out->bytes = palloc(bcap);
	out->off[0] = 0;
	while (h.n > 0)
	{
		const SiftPartDict *b = in[h.part[0]];
		uint32		bi = cur[h.part[0]];
		uint32		len = b->off[bi + 1] - b->off[bi];

		CHECK_FOR_INTERRUPTS();
		if (out->n == ocap)
			out->off = repalloc(out->off, ((ocap *= 2) + 1) * sizeof(uint32));
		if (out->off[out->n] + (Size) len > bcap)
			out->bytes = repalloc(out->bytes, bcap = Max(out->off[out->n] + (Size) len, Min(2 * bcap, MaxAllocSize)));
		memcpy(out->bytes + out->off[out->n], b->bytes + b->off[bi], len);
		out->off[out->n + 1] = out->off[out->n] + len;
		while (h.n > 0 && entry_cmp(in[h.part[0]], cur[h.part[0]], b, bi) == 0)
		{
			int			k = h.part[0];

			((uint32 *) m->map[k * m->ncols + c].to)[cur[k]++] = out->n;
			if (cur[k] == in[k]->n)
				h.part[0] = h.part[--h.n];
			heap_down(&h, 0);
		}
		out->n++;
	}
	out->off = repalloc(out->off, ((Size) out->n + 1) * sizeof(uint32));
	out->bytes = repalloc(out->bytes, Max(out->off[out->n], 1));
	pfree(h.part);
	pfree(cur);
}

/* A column the rows are sorted by is kept as its strings: the key of a row, which bounds the parts in the manifest, is made of them */
static bool
is_sort_col(const SiftPartSpec *spec, int c)
{
	for (int k = 0; k < spec->nsort; k++)
		if (spec->sort_cols[k] == c)
			return true;
	return false;
}

/* The dictionary of column c of part k, which the part codes by one (physical column pc), or NULL */
static const SiftPartDict *
part_dict(Relation rel, const SiftPart *part, const SiftColRemap *remap, int c)
{
	int			pc = remap != NULL ? remap->phys[c] : c;

	return pc >= 0 ? sift_part_dict_find(sift_part_dicts(rel, part), pc, SIFT_DICT_KIND_STR) : NULL;
}

/* The columns every input codes by a dictionary, with what their dictionaries take together, found one input at a time: only the input that is read is open */
static Size *
dict_sizes(Relation rel, uint32 part_id, const SiftPartDesc *const *at, int nparts, const SiftPartSpec *spec, const SiftColRemap *const *remaps, bool *candidate)
{
	Size	   *size = palloc0(Max(spec->ncols, 1) * sizeof(Size));

	for (int k = 0; k < nparts; k++)
	{
		MemoryContext icx;
		SiftPart   *part = sift_compact_open_input(rel, at[k], part_id, &icx);

		for (int c = 0; c < spec->ncols; c++)
		{
			const SiftPartDict *d = candidate[c] ? part_dict(rel, part, remaps != NULL ? remaps[k] : NULL, c) : NULL;

			candidate[c] = d != NULL;
			size[c] += d != NULL ? (Size) d->off[d->n] + 2 * sizeof(uint32) * d->n : 0;
		}
		MemoryContextDelete(icx);
	}
	return size;
}

/* The dictionaries of the columns that are merged, copied out of the inputs one at a time: in[c * nparts + k] is that of column c of input k */
static SiftPartDict *
copy_dicts(Relation rel, uint32 part_id, const SiftPartDesc *const *at, int nparts, int ncols, const SiftColRemap *const *remaps, const bool *merged)
{
	SiftPartDict *copy = palloc0(Max(ncols * nparts, 1) * sizeof(SiftPartDict));

	for (int k = 0; k < nparts; k++)
	{
		MemoryContext icx;
		SiftPart   *part = sift_compact_open_input(rel, at[k], part_id, &icx);

		for (int c = 0; c < ncols; c++)
		{
			const SiftPartDict *d = merged[c] ? part_dict(rel, part, remaps != NULL ? remaps[k] : NULL, c) : NULL;
			SiftPartDict *to = &copy[c * nparts + k];

			if (d != NULL)
			{
				*to = *d;
				to->off = memcpy(palloc((d->n + 1) * sizeof(uint32)), d->off, (d->n + 1) * sizeof(uint32));
				to->bytes = memcpy(palloc(Max(d->off[d->n], 1)), d->bytes, d->off[d->n]);
			}
		}
		MemoryContextDelete(icx);
	}
	return copy;
}

SiftDictMerge *
sift_dict_merge_plan(Relation rel, uint32 part_id, const SiftPartDesc *const *at, int nparts, const SiftPartSpec *spec, const bool *keep_strings,
					 const SiftColRemap *const *remaps, Size max_bytes)
{
	Size		held = 0;
	SiftDictMerge *m = palloc0(sizeof(SiftDictMerge));
	bool	   *merged = palloc0(Max(spec->ncols, 1));
	const SiftPartDict **in = palloc(Max(nparts, 1) * sizeof(SiftPartDict *));
	Size	   *size;
	SiftPartDict *copy;
	bool		any = false;

	m->ncols = spec->ncols;
	m->nparts = nparts;
	m->kinds = memcpy(palloc(Max(spec->ncols, 1) * sizeof(SiftCellKind)), spec->kinds, spec->ncols * sizeof(SiftCellKind));
	m->dict = palloc0(Max(spec->ncols, 1) * sizeof(SiftPartDict));
	m->map = palloc0(Max(nparts * spec->ncols, 1) * sizeof(SiftCodeMap));
	for (int c = 0; c < spec->ncols; c++)
		merged[c] = sift_compact_dict_remap && (keep_strings == NULL || !keep_strings[c]) && !is_sort_col(spec, c) && spec->kinds[c] == SIFT_CELL_STR && nparts > 0;
	size = dict_sizes(rel, part_id, at, nparts, spec, remaps, merged);
	for (int c = 0; c < spec->ncols; c++)
	{
		merged[c] = merged[c] && size[c] <= MaxAllocSize && held + size[c] <= max_bytes;
		held += merged[c] ? size[c] : 0;
		any |= merged[c];
	}
	copy = any ? copy_dicts(rel, part_id, at, nparts, spec->ncols, remaps, merged) : NULL;
	for (int c = 0; c < spec->ncols; c++)
	{
		if (!merged[c])
			continue;
		for (int k = 0; k < nparts; k++)
			in[k] = &copy[c * nparts + k];
		merge_column(m, c, in);
		m->kinds[c] = SIFT_CELL_INT;
		for (int k = 0; k < nparts; k++)
		{
			pfree(copy[c * nparts + k].off);
			pfree(copy[c * nparts + k].bytes);
		}
	}
	pfree(in);
	return m;
}

SiftPartSpec
sift_dict_merge_spec(const SiftDictMerge *m, const SiftPartSpec *spec)
{
	SiftPartSpec s = *spec;

	s.kinds = m->kinds;
	return s;
}

/* The values of a merged column as codes of the dictionary of the strings that are left, or as those strings when the dictionary does not pay (what a build from them decides) */
static void
finish_column(const SiftDictMerge *m, const SiftRowBuf *rb, int c, SiftColIn *col, SiftCompactResult *res)
{
	const SiftPartDict *d = &m->dict[c];
	uint32		nvals = sift_rowbuf_values(rb, c),
				nn = 0,
				nd = 0;
	const int64 *code = rb->i[c];
	bool	   *used = palloc0(Max(d->n, 1));
	uint32	   *rank = palloc(Max(d->n, 1) * sizeof(uint32));

	for (uint32 i = 0; i < nvals; i++)
		if (!rb->isnull[c][i])
		{
			used[code[i]] = true;
			nn++;
		}
	for (uint32 u = 0; u < d->n; u++)
		rank[u] = used[u] ? nd++ : 0;
	col->kind = SIFT_CELL_STR;
	col->i = NULL;
	if (nd > 0 && nd <= sift_part_dict_max(nn))
	{
		SiftDictEnc *pd = palloc(sizeof(SiftDictEnc));
		uint32	   *dcode = palloc0(Max(nvals, 1) * sizeof(uint32));

		pd->n = nd;
		pd->p = palloc(nd * sizeof(char *));
		pd->len = palloc(nd * sizeof(uint32));
		for (uint32 u = 0, k = 0; u < d->n; u++)
			if (used[u])
			{
				pd->p[k] = d->bytes + d->off[u];
				pd->len[k++] = d->off[u + 1] - d->off[u];
			}
		for (uint32 i = 0; i < nvals; i++)
			if (!rb->isnull[c][i])
				dcode[i] = rank[code[i]];
		col->dcode = dcode;
		col->pdict = pd;
		res->dict_cols_remapped++;
		res->dict_codes_remapped += nn;
	}
	else
	{
		const char **s = palloc0(Max(nvals, 1) * sizeof(char *));
		uint32	   *len = palloc0(Max(nvals, 1) * sizeof(uint32));

		for (uint32 i = 0; i < nvals; i++)
			if (!rb->isnull[c][i])
			{
				s[i] = d->bytes + d->off[code[i]];
				len[i] = d->off[code[i] + 1] - d->off[code[i]];
			}
		col->s = s;
		col->slen = len;
	}
	pfree(used);
	pfree(rank);
}

void
sift_dict_merge_finish(const SiftDictMerge *m, const SiftRowBuf *rb, SiftColIn *cols, SiftCompactResult *res)
{
	for (int c = 0; c < m->ncols; c++)
		if (m->dict[c].n > 0)
			finish_column(m, rb, c, &cols[c], res);
}
