/*
 * col/scope_v6/topk.c - the newest rows of a scope key: the ring of 64, the month map, and the read that uses them with the time strides (tstride.c).
 *
 * The ring answers an unfiltered ORDER BY time DESC LIMIT n from the entries of the key. When the ring runs out (rows deleted, n larger) or the
 * query has a filter, the read walks the key's months newest first, the granules that hold the month, and in each granule the strides of 256 rows whose
 * time range can still hold a newer row than the n found so far.
 */
/* check-memory: allow-allocs the arrays follow the granules of one key and the n of one query, not the rows of a table */
#include "core/host.h"

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/wire.h"

/* ---------------------------------------------------------------------------
 * 1. The ring
 * ------------------------------------------------------------------------- */

int
sift_newest_cmp(const SiftScopeNewest *a, const SiftScopeNewest *b)
{
	if (a->time != b->time)
		return a->time < b->time ? -1 : 1;
	if (a->gidx != b->gidx)
		return a->gidx < b->gidx ? -1 : 1;
	return a->row_ord < b->row_ord ? -1 : a->row_ord > b->row_ord;
}

bool
sift_topk_wants(const SiftTopKHeap *h, int64 time)
{
	return h->n < SIFT_SCOPE_TOPK_CAP || time >= h->e[0].time;
}

static void
heap_sift_down(SiftTopKHeap *h, uint32 i)
{
	for (;;)
	{
		uint32		l = 2 * i + 1,
					r = l + 1,
					m = i;

		if (l < h->n && sift_newest_cmp(&h->e[l], &h->e[m]) < 0)
			m = l;
		if (r < h->n && sift_newest_cmp(&h->e[r], &h->e[m]) < 0)
			m = r;
		if (m == i)
			return;
		{
			SiftScopeNewest t = h->e[i];

			h->e[i] = h->e[m];
			h->e[m] = t;
		}
		i = m;
	}
}

void
sift_topk_offer(SiftTopKHeap *h, int64 time, uint32 gidx, uint32 row_ord)
{
	SiftScopeNewest c = {time, gidx, row_ord};

	if (h->n < SIFT_SCOPE_TOPK_CAP)
	{
		uint32		i = h->n++;

		h->e[i] = c;
		while (i > 0 && sift_newest_cmp(&h->e[i], &h->e[(i - 1) / 2]) < 0)
		{
			SiftScopeNewest t = h->e[i];

			h->e[i] = h->e[(i - 1) / 2];
			h->e[(i - 1) / 2] = t;
			i = (i - 1) / 2;
		}
	}
	else if (sift_newest_cmp(&c, &h->e[0]) > 0)
	{
		h->e[0] = c;
		heap_sift_down(h, 0);
	}
}

static int
newest_desc(const void *a, const void *b)
{
	return -sift_newest_cmp(a, b);
}

static inline uint32
loc_pack(uint32 gidx, uint32 row_ord)
{
	return (gidx << SIFT_GRANULE_ROW_BITS) | (row_ord & SIFT_GRANULE_ROW_MASK);
}

uint32
sift_topk_entry_size(bool wide)
{
	return (uint32) (wide ? sizeof(SiftScopeTopKEntryWide) : sizeof(SiftScopeTopKEntry));
}

/* The n entries of a ring as stored: the first time raw, the others the step down from the entry before (SIFT_SMETA_TOPK_DELTA) */
void
sift_topk_read(const char *slot, uint32 n, bool wide, SiftScopeNewest *out)
{
	for (uint32 i = 0; i < n; i++)
	{
		int64		time;

		if (wide)
		{
			SiftScopeTopKEntryWide e;

			memcpy(&e, slot + i * sizeof(e), sizeof(e));
			time = e.time_key;
			out[i] = (SiftScopeNewest) {time, e.global_granule_idx, e.row_ord};
		}
		else
		{
			SiftScopeTopKEntry e;

			memcpy(&e, slot + i * sizeof(e), sizeof(e));
			time = e.time_key;
			out[i] = (SiftScopeNewest) {time, e.loc >> SIFT_GRANULE_ROW_BITS, e.loc & SIFT_GRANULE_ROW_MASK};
		}
		if (i > 0)
			out[i].time = (int64) ((uint64) out[i - 1].time - (uint64) time);
	}
}

uint32
sift_topk_write(const SiftTopKHeap *h, bool wide, StringInfo out)
{
	SiftScopeNewest s[SIFT_SCOPE_TOPK_CAP];

	memcpy(s, h->e, sizeof(SiftScopeNewest) * h->n);
	qsort(s, h->n, sizeof(SiftScopeNewest), newest_desc);
	for (uint32 i = 0; i < h->n; i++)
	{
		const int64 time = i == 0 ? s[0].time : (int64) ((uint64) s[i - 1].time - (uint64) s[i].time);

		SIFT_INVARIANT(wide || s[i].gidx < SIFT_TOPK_COMPACT_GRANS, "scope ring: granule %u does not fit a compact entry", s[i].gidx);
		if (wide)
		{
			SiftScopeTopKEntryWide e = {time, s[i].gidx, (uint16) s[i].row_ord, 0};

			appendBinaryStringInfo(out, (char *) &e, sizeof(e));
		}
		else
		{
			SiftScopeTopKEntry e = {time, loc_pack(s[i].gidx, s[i].row_ord)};

			appendBinaryStringInfo(out, (char *) &e, sizeof(e));
		}
	}
	return h->n;
}

uint32
sift_hdr_rings(const SiftScopeMetaHeader *hdr)
{
	SiftScopeTopKDesc d;

	if (!(hdr->flags & SIFT_SMETA_MULTI_TOPK))
		return 1;
	memcpy(&d, &hdr->reserved[0], sizeof(d));
	return Max(d.ntopk_rings, 1);
}

uint32
sift_ring_count(const SiftScopeL2Root *r, uint32 ring)
{
	return ring == 0 ? r->topk_count : sift_root_extra(r).extra_count[ring - 1];
}

uint32
sift_ring_start(const SiftScopeL2Root *r, uint32 ring, bool wide)
{
	uint32		n = 0;

	for (uint32 j = 0; j < ring; j++)
		n += sift_ring_count(r, j);
	return n * sift_topk_entry_size(wide);
}

uint32
sift_rings_bytes(const SiftScopeL2Root *r, uint32 nrings, bool wide)
{
	return sift_ring_start(r, nrings, wide);
}

void
sift_rings_write(const SiftTopKHeap *rings, uint32 nrings, uint32 partial, bool wide, StringInfo out, SiftScopeL2Root *root)
{
	SiftScopeL2ExtraTopK x = {{0}, 0};

	SIFT_INVARIANT(nrings >= 1 && nrings <= SIFT_SCOPE_TOPK_RINGS_CAP, "scope ring: %u rings", nrings);
	root->topk_count = (uint16) sift_topk_write(&rings[0], wide, out);
	for (uint32 r = 1; r < nrings; r++)
	{
		x.extra_count[r - 1] = (uint16) sift_topk_write(&rings[r], wide, out);
		x.extra_flags |= SIFT_L2X_PRESENT(r) | ((partial >> r) & 1 ? SIFT_L2X_PARTIAL(r) : 0);
	}
	memcpy(&root->reserved1[0], &x, sizeof(x));
	root->flags |= SIFT_L2_HAS_TOPK | (partial & 1 ? SIFT_L2_TOPK_PARTIAL : 0);
}

bool
sift_scope_ring_present(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring)
{
	return ring < m->ntopk_rings && (r->flags & SIFT_L2_HAS_TOPK) && (ring == 0 || (sift_root_extra(r).extra_flags & SIFT_L2X_PRESENT(ring)));
}

bool
sift_scope_ring_partial(const SiftScopeL2Root *r, uint32 ring)
{
	return ring == 0 ? (r->flags & SIFT_L2_TOPK_PARTIAL) != 0 : (sift_root_extra(r).extra_flags & SIFT_L2X_PARTIAL(ring)) != 0;
}

uint32
sift_scope_topk_get_ring(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, SiftScopeNewest *out)
{
	bool		wide = (m->hdr.flags & SIFT_SMETA_TOPK_WIDE) != 0;
	uint32		esize = sift_topk_entry_size(wide),
				n;
	uint64		at;

	SIFT_INVARIANT(!m->topk_packed, "scope rings are read before sift_scope_meta_ensure_topk unpacked them");
	memset(out, 0, sizeof(*out) * SIFT_SCOPE_TOPK_CAP);
	if (!sift_scope_ring_present(m, r, ring))
		return 0;
	n = Min(sift_ring_count(r, ring), SIFT_SCOPE_TOPK_CAP);
	at = (uint64) r->topk_off + sift_ring_start(r, ring, wide);
	if (at + (uint64) n * esize > m->topk.len)
		return 0;
	sift_topk_read(m->topk.p + at, n, wide, out);
	return n;
}

bool
sift_scope_ring_nulls_last(const SiftScopeMeta *m, uint32 ring)
{
	SiftScopeTopKDesc d;

	if (ring >= m->ntopk_rings)
		return false;
	if (!(m->hdr.flags & SIFT_SMETA_MULTI_TOPK))
		return true;
	memcpy(&d, &m->hdr.reserved[0], sizeof(d));
	return (d.nulls_last_mask >> ring & 1) != 0;
}

bool
sift_scope_ring_serves(const SiftScopeMeta *m, uint32 ring, bool nulls_first, bool leaf_not_null)
{
	return ring < m->ntopk_rings && (leaf_not_null || (!nulls_first && sift_scope_ring_nulls_last(m, ring)));
}

uint32
sift_scope_topk_get(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeNewest *out)
{
	return sift_scope_topk_get_ring(m, r, 0, out);
}

/* ---------------------------------------------------------------------------
 * 2. The month map
 * ------------------------------------------------------------------------- */

void
sift_mmap_add(SiftMonthMapAcc *a, int32 month_id, uint32 ordinal)
{
	uint32		lo = 0,
				hi = a->n;
	SiftMonthMapEntry *e;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (a->e[mid].month_id < month_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == a->n || a->e[lo].month_id != month_id)
	{
		if (a->n == a->cap)
		{
			a->cap = a->cap == 0 ? 8 : a->cap * 2;
			a->e = a->e == NULL ? palloc(a->cap * sizeof(*a->e)) : repalloc(a->e, a->cap * sizeof(*a->e));
		}
		memmove(&a->e[lo + 1], &a->e[lo], (a->n - lo) * sizeof(*a->e));
		a->n++;
		memset(&a->e[lo], 0, sizeof(*a->e));
		a->e[lo].month_id = month_id;
	}
	e = &a->e[lo];
	if (e->n > 0 && e->ord[e->n - 1] == ordinal)
		return;
	if (e->n == e->cap)
	{
		e->cap = e->cap == 0 ? 8 : e->cap * 2;
		e->ord = e->ord == NULL ? palloc(e->cap * sizeof(uint32)) : repalloc(e->ord, e->cap * sizeof(uint32));
	}
	e->ord[e->n++] = ordinal;
}

void
sift_mmap_free(SiftMonthMapAcc *a)
{
	for (uint32 i = 0; i < a->n; i++)
		if (a->e[i].ord != NULL)
			pfree(a->e[i].ord);
	if (a->e != NULL)
		pfree(a->e);
	memset(a, 0, sizeof(*a));
}

void
sift_mmap_write(SiftMonthMapAcc *a, uint32 ngranules, const uint32 *remap, StringInfo out)
{
	SiftScopeMonthMapHeader h = {ngranules, (uint16) a->n, 0};
	uint32		bitset_bytes = (ngranules + 7) / 8;
	SiftScopeMonthBucket *b = palloc0(sizeof(*b) * Max(a->n, 1));
	int			start = out->len;
	uint32		data = sizeof(h) + a->n * sizeof(SiftScopeMonthBucket);

	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(out, (char *) b, a->n * sizeof(*b));
	for (uint32 i = 0; i < a->n; i++)
	{
		SiftMonthMapEntry *e = &a->e[a->n - 1 - i];		/* newest month first */
		SiftScopeMonthBucket *bk = &b[i];

		if (remap != NULL)
		{
			for (uint32 j = 0; j < e->n; j++)
				e->ord[j] = remap[e->ord[j]];
			qsort(e->ord, e->n, sizeof(uint32), sift_cmp_u32);
		}
		bk->month_id = e->month_id;
		bk->count = (uint16) Min(e->n, 65535);
		bk->data_off = data;
		if (ngranules <= 65535 && e->n * 2 < bitset_bytes)
		{
			bk->flags = SIFT_MMAP_LIST16;
			for (uint32 j = 0; j < e->n; j++)
			{
				uint16		v = (uint16) e->ord[j];

				appendBinaryStringInfo(out, (char *) &v, sizeof(v));
			}
			data += e->n * 2;
		}
		else if (ngranules > 65535 && e->n <= 65535 && e->n * 4 < bitset_bytes)
		{
			bk->flags = SIFT_MMAP_LIST32;
			appendBinaryStringInfo(out, (char *) e->ord, e->n * sizeof(uint32));
			data += e->n * 4;
		}
		else
		{
			uint8	   *bits = palloc0(Max(bitset_bytes, 1));

			bk->flags = SIFT_MMAP_BITSET;
			for (uint32 j = 0; j < e->n; j++)
				bits[e->ord[j] / 8] |= 1 << (e->ord[j] % 8);
			appendBinaryStringInfo(out, (char *) bits, bitset_bytes);
			data += bitset_bytes;
			pfree(bits);
		}
	}
	memcpy(out->data + start + sizeof(h), b, a->n * sizeof(*b));
	pfree(b);
	wire_pad8(out);
}

/* A key of one granule stores no map: its month cells (month_id ascending, in the level-1 section) are the months of that granule */
static const SiftScopeMonthCell *
monthmap_cells(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 *n)
{
	*n = 0;
	if ((r->flags & SIFT_L2_HAS_MONTHMAP) || !(r->flags & SIFT_L2_HAS_MONTHS) || r->l1_count != 1 || m->nodes[r->l1_first].nrefs != 1 ||
		(uint64) r->month_dir_off + (uint64) r->nmonths * sizeof(SiftScopeMonthCell) > m->l1.len)
		return NULL;
	*n = r->nmonths;
	return (const SiftScopeMonthCell *) (m->l1.p + r->month_dir_off);
}

uint32
sift_scope_monthmap_nmonths(const SiftScopeMeta *m, const SiftScopeL2Root *r)
{
	SiftScopeMonthMapHeader h;
	uint32		n;

	if (r->flags & SIFT_L2_HAS_MONTHMAP)
	{
		memcpy(&h, m->monthmap.p + r->monthmap_off, sizeof(h));
		return h.nmonths;
	}
	monthmap_cells(m, r, &n);
	return n;
}

uint32
sift_scope_monthmap_ngranules(const SiftScopeMeta *m, const SiftScopeL2Root *r)
{
	SiftScopeMonthMapHeader h;
	uint32		n;

	if (r->flags & SIFT_L2_HAS_MONTHMAP)
	{
		memcpy(&h, m->monthmap.p + r->monthmap_off, sizeof(h));
		return h.ngranules;
	}
	monthmap_cells(m, r, &n);
	return n > 0;
}

uint32
sift_scope_monthmap_buckets(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeMonthBucket *out, uint32 max)
{
	uint32		n = Min(sift_scope_monthmap_nmonths(m, r), max),
				ncells;
	const SiftScopeMonthCell *cells = monthmap_cells(m, r, &ncells);

	if (cells == NULL)
	{
		if (n > 0)
			memcpy(out, m->monthmap.p + r->monthmap_off + sizeof(SiftScopeMonthMapHeader), n * sizeof(*out));
		return n;
	}
	for (uint32 i = 0; i < n; i++)
	{
		SiftScopeMonthCell c;

		memcpy(&c, &cells[ncells - 1 - i], sizeof(c));
		out[i] = (SiftScopeMonthBucket) {c.month_id, SIFT_MMAP_LIST16, 1, 0};
	}
	return n;
}

uint32
sift_scope_monthmap_granules(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, uint32 *out, uint32 max)
{
	SiftScopeMonthMapHeader h;
	const char *base = m->monthmap.p + r->monthmap_off;
	uint32		n = 0,
				ncells;
	const SiftScopeMonthCell *cells = monthmap_cells(m, r, &ncells);

	if (cells != NULL)
	{
		for (uint32 i = 0; i < ncells && max > 0; i++)
		{
			SiftScopeMonthCell c;

			memcpy(&c, &cells[i], sizeof(c));
			if (c.month_id == month)
			{
				out[0] = 0;
				return 1;
			}
		}
		return 0;
	}
	if (!(r->flags & SIFT_L2_HAS_MONTHMAP))
		return 0;
	memcpy(&h, base, sizeof(h));
	for (uint32 i = 0; i < h.nmonths; i++)
	{
		SiftScopeMonthBucket b;

		memcpy(&b, base + sizeof(h) + i * sizeof(b), sizeof(b));
		if (b.month_id != month)
			continue;
		if (b.flags & SIFT_MMAP_BITSET)
		{
			for (uint32 g = 0; g < h.ngranules && n < max; g++)
				if (base[b.data_off + g / 8] & (1 << (g % 8)))
					out[n++] = g;
		}
		else if (b.flags & SIFT_MMAP_LIST16)
		{
			for (uint32 j = 0; j < b.count && n < max; j++)
			{
				uint16		v;

				memcpy(&v, base + b.data_off + j * 2, 2);
				out[n++] = v;
			}
		}
		else
			for (uint32 j = 0; j < b.count && n < max; j++)
				memcpy(&out[n++], base + b.data_off + j * 4, 4);
		break;
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * 3. The newest rows of a key
 * ------------------------------------------------------------------------- */

/* The newest rows found so far, descending, at most cap: a sorted insertion */
typedef struct NewestSet
{
	SiftScopeNewest *e;
	uint32		n;
	uint32		cap;
} NewestSet;

static void
set_offer(NewestSet *s, const SiftScopeNewest *c)
{
	uint32		at = s->n;

	if (s->n == s->cap && sift_newest_cmp(c, &s->e[s->n - 1]) <= 0)
		return;
	if (s->n < s->cap)
		s->n++;
	while (at > 0 && sift_newest_cmp(c, &s->e[at - 1]) > 0)
	{
		if (at < s->n)
			s->e[at] = s->e[at - 1];
		at--;
	}
	s->e[at] = *c;
}

/* Walks the month map newest month first and fills set with the newest live rows older than bound (NULL: no bound) */
static void
scan_months(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeEntity *e, const SiftScopeNewest *bound, NewestSet *set)
{
	uint32		nb;
	SiftScopeMonthBucket *bk;
	SiftScopeGranRef *refs;
	uint32		*ords;
	SiftScopeNewest *rows;

	nb = sift_scope_monthmap_nmonths(m, r);
	bk = palloc(sizeof(*bk) * Max(nb, 1));
	nb = sift_scope_monthmap_buckets(m, r, bk, nb);
	refs = palloc(sizeof(*refs) * Max(sift_scope_key_refs(m, r, NULL), 1));
	sift_scope_key_refs(m, r, refs);
	ords = palloc(sizeof(uint32) * Max(sift_scope_monthmap_ngranules(m, r), 1));
	rows = palloc(sizeof(*rows) * SIFT_TSTRIDE_ROWS);
	for (uint32 i = 0; i < nb; i++)
	{
		int64		mstart = sift_scope_month_start(bk[i].month_id),
					mend = sift_scope_month_end(bk[i].month_id);
		uint32		ng;

		if (bound != NULL && mstart > bound->time)
			continue;
		if (set->n == set->cap && mend <= set->e[set->n - 1].time)
			break;
		ng = sift_scope_monthmap_granules(m, r, bk[i].month_id, ords, sift_scope_monthmap_ngranules(m, r));
		for (uint32 g = 0; g < ng; g++)
		{
			const SiftScopeGranRef *ref = &refs[ords[g]];
			const SiftTimeStride *st;
			uint32		nst = e->strides(e->arg, ref->global_granule_idx, &st);

			for (uint32 s = nst; s-- > 0;)
			{
				uint32		nr;

				if (st[s].time_min > st[s].time_max || (bound != NULL && st[s].time_min > bound->time) || st[s].time_max < mstart ||
					st[s].time_min >= mend || (set->n == set->cap && st[s].time_max < set->e[set->n - 1].time))
					continue;
				nr = e->scan_stride(e->arg, ref->global_granule_idx, s, ref->slice_id != SIFT_SCOPE_PURE_GRANULE, r->scope_key, rows);
				for (uint32 k = 0; k < nr; k++)
				{
					if (sift_scope_month_of(rows[k].time) != bk[i].month_id || (bound != NULL && sift_newest_cmp(&rows[k], bound) >= 0))
						continue;
					set_offer(set, &rows[k]);
				}
			}
		}
	}
	pfree(bk);
	pfree(refs);
	pfree(ords);
	pfree(rows);
}

uint32
sift_scope_latest(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 n, bool filtered, const SiftScopeEntity *e, SiftScopeNewest *out, uint32 *from_ring)
{
	NewestSet	set = {out, 0, n};
	SiftScopeNewest last = {0};
	bool		bounded = false,
				spill = true;

	*from_ring = 0;
	if (n == 0)
		return 0;
	if (!filtered && (r->flags & SIFT_L2_HAS_TOPK))
	{
		SiftScopeNewest ring[SIFT_SCOPE_TOPK_CAP];
		uint32		k = sift_scope_topk_get(m, r, ring);

		for (uint32 i = 0; i < k && set.n < n; i++)
		{
			last = ring[i];
			if (e->live(e->arg, last.gidx, last.row_ord))
				out[set.n++] = last;
		}
		*from_ring = set.n;
		bounded = k > 0;
		spill = set.n < n && (k == SIFT_SCOPE_TOPK_CAP || (r->flags & SIFT_L2_TOPK_PARTIAL));
	}
	if (spill && sift_scope_monthmap_ngranules(m, r) > 0)
	{
		NewestSet	rest = {out + set.n, 0, n - set.n};

		scan_months(m, r, e, bounded ? &last : NULL, &rest);
		set.n += rest.n;
	}
	return set.n;
}

/* ---------------------------------------------------------------------------
 * 4. The extra rings
 * ------------------------------------------------------------------------- */

typedef struct RingGran
{
	int64		zmax;
	SiftScopeGranRef ref;
} RingGran;

static int
ring_gran_desc(const void *a, const void *b)
{
	const RingGran *x = a,
			   *y = b;

	if (x->zmax != y->zmax)
		return x->zmax > y->zmax ? -1 : 1;
	return x->ref.global_granule_idx < y->ref.global_granule_idx ? -1 : x->ref.global_granule_idx > y->ref.global_granule_idx;
}

/* The granules of a key whose zone of the leaf of ring can hold a value of [lo, hi] below bound (NULL: no bound), the largest maximum first */
static uint32
ring_granules(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, const SiftScopeEntity *e, int64 lo, int64 hi, const SiftScopeNewest *bound, RingGran **out)
{
	uint32		nrefs = sift_scope_key_refs(m, r, NULL),
				n = 0;
	SiftScopeGranRef *refs = palloc(sizeof(*refs) * Max(nrefs, 1));
	RingGran   *g = palloc(sizeof(*g) * Max(nrefs, 1));

	sift_scope_key_refs(m, r, refs);
	for (uint32 i = 0; i < nrefs; i++)
	{
		int64		zmin,
					zmax;

		if (!e->ring_zone(e->arg, refs[i].global_granule_idx, ring, &zmin, &zmax) || zmax < lo || zmin > hi || (bound != NULL && zmin > bound->time))
			continue;
		g[n++] = (RingGran) {zmax, refs[i]};
	}
	qsort(g, n, sizeof(*g), ring_gran_desc);
	pfree(refs);
	*out = g;
	return n;
}

/* Fills set with the largest live rows of the leaf of ring below bound, from the granules the largest zone maximum first: it stops at the first granule that cannot beat the n found */
static void
ring_walk(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, const SiftScopeEntity *e, const SiftScopeNewest *bound, NewestSet *set)
{
	RingGran   *g;
	uint32		n = ring_granules(m, r, ring, e, INT64_MIN, INT64_MAX, bound, &g);
	SiftScopeNewest *rows = palloc(sizeof(*rows) << SIFT_GRANULE_ROW_BITS);

	for (uint32 i = 0; i < n; i++)
	{
		uint32		nr;

		if (set->n == set->cap && g[i].zmax < set->e[set->n - 1].time)
			break;
		nr = e->scan_ring(e->arg, g[i].ref.global_granule_idx, ring, g[i].ref.slice_id != SIFT_SCOPE_PURE_GRANULE, r->scope_key, INT64_MIN, INT64_MAX, rows);
		for (uint32 k = 0; k < nr; k++)
			if (bound == NULL || sift_newest_cmp(&rows[k], bound) < 0)
				set_offer(set, &rows[k]);
	}
	pfree(g);
	pfree(rows);
}

static uint32
ring_latest(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, uint32 n, bool filtered, const SiftScopeEntity *e, SiftScopeNewest *out, uint32 *from_ring)
{
	NewestSet	set = {out, 0, n};
	SiftScopeNewest last = {0};
	bool		bounded = false,
				spill = true;

	*from_ring = 0;
	if (!filtered && sift_scope_ring_present(m, r, ring))
	{
		SiftScopeNewest slot[SIFT_SCOPE_TOPK_CAP];
		uint32		k = sift_scope_topk_get_ring(m, r, ring, slot);

		for (uint32 i = 0; i < k && set.n < n; i++)
		{
			last = slot[i];
			if (e->live(e->arg, last.gidx, last.row_ord))
				out[set.n++] = last;
		}
		*from_ring = set.n;
		bounded = k > 0;
		spill = set.n < n && (k == SIFT_SCOPE_TOPK_CAP || sift_scope_ring_partial(r, ring));
	}
	if (spill)
	{
		NewestSet	rest = {out + set.n, 0, n - set.n};

		ring_walk(m, r, ring, e, bounded ? &last : NULL, &rest);
		set.n += rest.n;
	}
	return set.n;
}

uint32
sift_scope_latest_ring(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, uint32 n, bool filtered, const SiftScopeEntity *e, SiftScopeNewest *out, uint32 *from_ring, bool *tie_at_cut)
{
	uint32		want = tie_at_cut != NULL ? n + 1 : n,
				got,
				kept;
	SiftScopeNewest *tmp;

	SIFT_INVARIANT(ring < m->ntopk_rings, "scope ring: ring %u of an object of %u", ring, m->ntopk_rings);
	if (tie_at_cut != NULL)
		*tie_at_cut = false;
	*from_ring = 0;
	if (n == 0)
		return 0;
	tmp = palloc(sizeof(*tmp) * want);
	got = ring == 0 ? sift_scope_latest(m, r, want, filtered, e, tmp, from_ring) : ring_latest(m, r, ring, want, filtered, e, tmp, from_ring);
	kept = Min(got, n);
	memcpy(out, tmp, sizeof(*out) * kept);
	*from_ring = Min(*from_ring, kept);
	if (tie_at_cut != NULL)
		*tie_at_cut = got > n && tmp[n].time == tmp[n - 1].time;
	pfree(tmp);
	return kept;
}

typedef struct TieList
{
	SiftScopeNewest *e;
	uint32		n;
	uint32		cap;
} TieList;

static void
ties_push(TieList *l, const SiftScopeNewest *rows, uint32 n)
{
	if (l->n + n > l->cap)
		l->e = repalloc(l->e, sizeof(*l->e) * (l->cap = Max(l->cap * 2, l->n + n)));
	memcpy(l->e + l->n, rows, sizeof(*rows) * n);
	l->n += n;
}

static void
ties_leaf(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, int64 value, const SiftScopeEntity *e, TieList *l)
{
	RingGran   *g;
	uint32		n = ring_granules(m, r, ring, e, value, value, NULL, &g);
	SiftScopeNewest *rows = palloc(sizeof(*rows) << SIFT_GRANULE_ROW_BITS);

	for (uint32 i = 0; i < n; i++)
		ties_push(l, rows, e->scan_ring(e->arg, g[i].ref.global_granule_idx, ring, g[i].ref.slice_id != SIFT_SCOPE_PURE_GRANULE, r->scope_key, value, value, rows));
	pfree(g);
	pfree(rows);
}

/* The time leaf has no zone to read: the month of the value names the granules, and the strides that can hold it the rows */
static void
ties_time(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 value, const SiftScopeEntity *e, TieList *l)
{
	SiftScopeGranRef *refs;
	uint32	   *ords;
	SiftScopeNewest *rows;
	uint32		ng,
				ngranules = sift_scope_monthmap_ngranules(m, r);

	if (ngranules == 0)
		return;
	refs = palloc(sizeof(*refs) * Max(sift_scope_key_refs(m, r, NULL), 1));
	sift_scope_key_refs(m, r, refs);
	ords = palloc(sizeof(uint32) * ngranules);
	rows = palloc(sizeof(*rows) * SIFT_TSTRIDE_ROWS);
	ng = sift_scope_monthmap_granules(m, r, sift_scope_month_of(value), ords, ngranules);
	for (uint32 g = 0; g < ng; g++)
	{
		const SiftScopeGranRef *ref = &refs[ords[g]];
		const SiftTimeStride *st;
		uint32		nst = e->strides(e->arg, ref->global_granule_idx, &st);

		for (uint32 s = 0; s < nst; s++)
		{
			uint32		nr;

			if (st[s].time_min > value || st[s].time_max < value)
				continue;
			nr = e->scan_stride(e->arg, ref->global_granule_idx, s, ref->slice_id != SIFT_SCOPE_PURE_GRANULE, r->scope_key, rows);
			for (uint32 k = 0; k < nr; k++)
				if (rows[k].time == value)
					ties_push(l, &rows[k], 1);
		}
	}
	pfree(refs);
	pfree(ords);
	pfree(rows);
}

uint32
sift_scope_ring_ties(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, int64 value, const SiftScopeEntity *e, SiftScopeNewest **out)
{
	TieList		l = {palloc(sizeof(SiftScopeNewest) * 64), 0, 64};

	SIFT_INVARIANT(ring < m->ntopk_rings, "scope ring: ties of ring %u of an object of %u", ring, m->ntopk_rings);
	if (ring == 0)
		ties_time(m, r, value, e, &l);
	else
		ties_leaf(m, r, ring, value, e, &l);
	*out = l.e;
	return l.n;
}

static int
cand_rank(const void *a, const void *b)
{
	const SiftScopeRingCand *x = a,
			   *y = b;
	SiftU128	xs = (SiftU128) (x->nrows - x->nnull) * y->nrows,
				ys = (SiftU128) (y->nrows - y->nnull) * x->nrows;

	if (x->kind != y->kind)
		return x->kind < y->kind ? -1 : 1;
	if (xs != ys)
		return xs > ys ? -1 : 1;
	return x->leaf < y->leaf ? -1 : x->leaf > y->leaf;
}

uint32
sift_scope_rings_pick(SiftScopeBuildSpec *spec, const SiftScopeRingCand *cand, uint32 ncand)
{
	SiftScopeRingCand *ok = palloc(sizeof(*ok) * Max(ncand, 1));
	uint32		n = 0,
				rings = 1;

	for (uint32 i = 0; i < ncand; i++)
		if (cand[i].leaf >= 0 && cand[i].leaf != spec->member_leaf && cand[i].leaf != spec->time_leaf &&
			cand[i].kind >= SIFT_RING_KIND_TIMESTAMP && cand[i].kind <= SIFT_RING_KIND_ORDINAL && cand[i].nrows > 0 &&
			(cand[i].nrows - cand[i].nnull) * 100 >= cand[i].nrows * SIFT_SCOPE_RING_NONNULL_PCT)
			ok[n++] = cand[i];
	qsort(ok, n, sizeof(*ok), cand_rank);
	memset(spec->extra_leaf, 0xFF, sizeof(spec->extra_leaf));
	if (spec->time_leaf >= 0)
		for (uint32 i = 0; i < n && rings < SIFT_SCOPE_TOPK_RINGS_CAP; i++)
			spec->extra_leaf[rings++ - 1] = ok[i].leaf;
	spec->ntopk_rings = (uint8) rings;
	pfree(ok);
	return rings;
}
