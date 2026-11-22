/*
 * col/scope_v6/object.c - the object of a scope summary as bytes: the canonical dictionary, the member Bloom filter, the writer that lays the sections
 * out and the reader that finds a key in them in place.
 *
 * Layout: the sections in the order level 0, level 1, ring, month map, level 2, key directory, dictionary, header; then the directory of the
 * sections and the trailer. Each section is written front to back and its counts sit in a footer, so a section is appended without a pass back.
 * Every offset inside a section is from the start of that section.
 */
/* check-memory: allow-allocs the arrays follow the keys and level-1 nodes of one summary, which the build holds by design (88 bytes a node, 104 a key) */
#include "core/host.h"

#include "col/codec.h"
#include "col/compress.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/wire.h"

/* ---------------------------------------------------------------------------
 * 1. Canonical dictionary
 * ------------------------------------------------------------------------- */

void
sift_sdict_init(SiftScopeDict *d, MemoryContext mcxt)
{
	memset(d, 0, sizeof(*d));
	d->mcxt = mcxt;
}

static void
sdict_rehash(SiftScopeDict *d, uint32 nslot)
{
	if (d->slot != NULL)
		pfree(d->slot);
	d->nslot = nslot;
	d->slot = MemoryContextAllocZero(d->mcxt, sizeof(uint32) * nslot);
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		h = hash_bytes((const unsigned char *) d->str[c], d->len[c]) & (nslot - 1);

		while (d->slot[h] != 0)
			h = (h + 1) & (nslot - 1);
		d->slot[h] = c + 1;
	}
}

uint32
sift_sdict_intern(SiftScopeDict *d, const char *s, uint32 len)
{
	uint32		h;

	if (d->nslot == 0)
		sdict_rehash(d, 64);
	h = hash_bytes((const unsigned char *) s, len) & (d->nslot - 1);
	for (; d->slot[h] != 0; h = (h + 1) & (d->nslot - 1))
	{
		uint32		c = d->slot[h] - 1;

		if (d->len[c] == len && memcmp(d->str[c], s, len) == 0)
			return c;
	}
	if (d->n == d->cap)
	{
		d->cap = d->cap == 0 ? 64 : d->cap * 2;
		d->str = d->str == NULL ? MemoryContextAlloc(d->mcxt, d->cap * sizeof(char *)) : repalloc(d->str, d->cap * sizeof(char *));
		d->len = d->len == NULL ? MemoryContextAlloc(d->mcxt, d->cap * sizeof(uint32)) : repalloc(d->len, d->cap * sizeof(uint32));
	}
	d->str[d->n] = MemoryContextAlloc(d->mcxt, len + 1);
	memcpy(d->str[d->n], s, len);
	d->len[d->n] = len;
	d->n++;
	if (d->n * 2 > d->nslot)
		sdict_rehash(d, d->nslot * 2);
	else
		d->slot[h] = d->n;
	return d->n - 1;
}

void
sift_sdict_write(const SiftScopeDict *d, StringInfo out)
{
	uint32		off = 0;

	appendBinaryStringInfo(out, (char *) &d->n, sizeof(uint32));
	for (uint32 i = 0; i < d->n; i++)
	{
		appendBinaryStringInfo(out, (char *) &off, sizeof(uint32));
		off += d->len[i];
	}
	appendBinaryStringInfo(out, (char *) &off, sizeof(uint32));
	for (uint32 i = 0; i < d->n; i++)
		appendBinaryStringInfo(out, d->str[i], d->len[i]);
}

bool
sift_sdict_seed(SiftScopeDict *d, const char *p, uint64 len)
{
	uint32		n;

	if (len < 4)
		return false;
	memcpy(&n, p, 4);
	if (len < 4 + ((uint64) n + 1) * 4)
		return false;
	for (uint32 i = 0; i < n; i++)
	{
		uint32		a,
					b;

		memcpy(&a, p + 4 + (uint64) i * 4, 4);
		memcpy(&b, p + 4 + (uint64) (i + 1) * 4, 4);
		if (a > b || 4 + ((uint64) n + 1) * 4 + b > len)
			return false;
		if (sift_sdict_intern(d, p + 4 + ((uint64) n + 1) * 4 + a, b - a) != i)
			return false;
	}
	return true;
}

const char *
sift_scope_dict_get(const SiftScopeMeta *m, uint32 code, uint32 *len)
{
	uint32		n,
				a,
				b;
	const char *p = m->dict.p;

	memcpy(&n, p, 4);
	if (code >= n)
		return NULL;
	memcpy(&a, p + 4 + (uint64) code * 4, 4);
	memcpy(&b, p + 4 + (uint64) (code + 1) * 4, 4);
	*len = b - a;
	return p + 4 + ((uint64) n + 1) * 4 + a;
}

/* ---------------------------------------------------------------------------
 * 2. Writer
 * ------------------------------------------------------------------------- */

uint32
sift_block_end(const SiftScopeL1Node *n)
{
	return n->nmonths > 0 ? n->month_dir_off + n->nmonths * (uint32) sizeof(SiftScopeMonthCell) : n->all_cube_off + n->all_cube_len;
}

/* Moves the offsets of a cube block that was copied from delta bytes away: the month cells inside it */
static void
rebase_cells(char *cells, uint32 nmonths, int64 delta)
{
	for (uint32 i = 0; i < nmonths; i++)
	{
		SiftScopeMonthCell c;

		memcpy(&c, cells + i * sizeof(c), sizeof(c));
		c.cube_off = (uint32) ((int64) c.cube_off + delta);
		memcpy(cells + i * sizeof(c), &c, sizeof(c));
	}
}

void
sift_put_refs(StringInfo out, const SiftScopeGranRef *refs, uint32 n)
{
	int64		prev = 0;

	for (uint32 i = 0; i < n; i++)
	{
		wire_put_var(out, wire_zig(i == 0 ? (int64) refs[i].global_granule_idx : (int64) refs[i].global_granule_idx - prev));
		wire_put_var(out, (uint32) (refs[i].slice_id + 1));
		prev = refs[i].global_granule_idx;
	}
}

/* The level-2 block of a key: its all-time cube, the cubes of its months, their directory */
void
sift_l2_block(StringInfo blk, SiftScopeL2Root *root, const SiftScopeCubeAcc *all, const SiftScopeMonthTable *months)
{
	SiftScopeMonthCell *cells = NULL;

	root->all_cube_off = blk->len;
	sift_scube_write(blk, all);
	root->all_cube_len = blk->len - root->all_cube_off;
	sift_smonth_sort(months);
	root->nmonths = months->n;
	if (months->n > 0)
		cells = palloc(sizeof(*cells) * months->n);
	for (uint32 i = 0; i < months->n; i++)
	{
		uint32		at = blk->len;

		sift_scube_write(blk, &months->e[i].cube);
		cells[i] = (SiftScopeMonthCell) {months->e[i].month_id, at, blk->len - at};
	}
	if (cells != NULL)
	{
		root->month_dir_off = blk->len;
		appendBinaryStringInfo(blk, (char *) cells, months->n * sizeof(*cells));
		pfree(cells);
	}
}

/* The rings of a record that came from an object of the other width, written in the width of this one */
static void
put_ring_as(StringInfo out, const SiftKeyRec *rec, bool wide, uint32 nrings)
{
	for (uint32 r = 0; r < nrings; r++)
	{
		SiftScopeNewest e[SIFT_SCOPE_TOPK_CAP];
		SiftTopKHeap h = {.n = sift_ring_count(&rec->root, r),.e = e};
		const char *slot = rec->topk + sift_ring_start(&rec->root, r, rec->topk_wide);

		sift_topk_read(slot, h.n, rec->topk_wide, h.e);
		(void) sift_topk_write(&h, wide, out);
	}
}

static uint64
begin_section(StringInfo out, SiftScopeSecEntry *dir, uint32 *nsec, uint16 tag)
{
	wire_pad8(out);
	dir[*nsec] = (SiftScopeSecEntry) {tag, 0, 0, out->len, 0};
	return out->len;
}

static void
end_section(StringInfo out, SiftScopeSecEntry *dir, uint32 *nsec, uint64 start)
{
	dir[(*nsec)++].len = out->len - start;
}

/* Ends the ring section, stored as a raw length and one LZ4 block of its bytes when that is at least SIFT_SCOPE_TOPK_LZ4_MIN_PCT smaller */
static void
end_topk_section(StringInfo out, SiftScopeSecEntry *dir, uint32 *nsec, uint64 start)
{
	uint64		raw_len = out->len - start;
	uint32		raw32 = (uint32) raw_len;
	char	   *packed;
	Size		cap,
				clen = 0;

	SIFT_INVARIANT(raw_len <= UINT32_MAX, "scope object: the rings take %llu bytes", (unsigned long long) raw_len);
	if (raw_len > 0 && sift_scope_inner_lz4())
	{
		cap = sift_colz_bound(SIFT_CC_ALG_LZ4, raw_len);
		packed = palloc(cap);
		clen = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, out->data + start, raw_len, packed, cap);
		if (clen > 0 && (clen + sizeof(raw32)) * 100 <= raw_len * (100 - SIFT_SCOPE_TOPK_LZ4_MIN_PCT))
		{
			out->len = start;
			appendBinaryStringInfo(out, (char *) &raw32, sizeof(raw32));
			appendBinaryStringInfo(out, packed, clen);
			dir[*nsec].flags |= SIFT_SEC_LZ4;
		}
		pfree(packed);
	}
	end_section(out, dir, nsec, start);
}

void
sift_emit_object(StringInfo out, SiftEmit *em, SiftKeyRec *recs, uint32 nrecs)
{
	SiftScopeSecEntry dir[SIFT_SCOPE_SECTIONS_CAP];
	uint32		nsec = 0;
	uint64		s;
	SiftScopeL1Node *descs;
	uint32		nnodes = 0,
				nl2 = 0;
	SiftScopeTrailer tr;
	SiftScopeMetaHeader hdr = em->hdr;
	uint64		pool;
	bool		wide = hdr.total_granules > SIFT_TOPK_COMPACT_GRANS;
	uint32		nrings = sift_hdr_rings(&hdr);

	SIFT_INVARIANT(out->len == 0, "scope object: the output is not empty");
	hdr.flags = (wide ? hdr.flags | SIFT_SMETA_TOPK_WIDE : hdr.flags & ~SIFT_SMETA_TOPK_WIDE) | SIFT_SMETA_VAR_TOPK | SIFT_SMETA_TOPK_DELTA;
	for (uint32 i = 0; i < nrecs; i++)
		nnodes += recs[i].nnodes;
	descs = palloc(sizeof(*descs) * Max(nnodes, 1));

	s = begin_section(out, dir, &nsec, SEC_SCOPE_L0_SLICE);
	appendBinaryStringInfo(out, em->l0_pool, em->l0_pool_len);
	wire_pad8(out);
	if (em->l0_nentries > 0)
		appendBinaryStringInfo(out, (char *) em->l0_entries, em->l0_nentries * sizeof(SiftScopeL0SliceEntry));
	{
		uint32		f[2] = {em->l0_nentries, 0};

		appendBinaryStringInfo(out, (char *) f, sizeof(f));
		appendBinaryStringInfo(out, (char *) &em->l0_pool_len, sizeof(uint64));
	}
	end_section(out, dir, &nsec, s);

	s = begin_section(out, dir, &nsec, SEC_SCOPE_L1);
	nnodes = 0;
	for (uint32 i = 0; i < nrecs; i++)
	{
		SiftKeyRec *r = &recs[i];

		r->root.l1_first = nnodes;
		r->root.l1_count = r->nnodes;
		for (uint32 j = 0; j < r->nnodes; j++)
		{
			SiftKeyNodeRec *nr = &r->nodes[j];
			SiftScopeL1Node d = nr->node;
			int64		delta;
			uint64		at;

			wire_pad8(out);
			at = out->len - s;
			delta = (int64) at - (int64) nr->block_off;
			appendBinaryStringInfo(out, nr->block, nr->block_len);
			d.node_id = nnodes;
			d.key_ordinal = i;
			d.ref_off = (uint32) ((int64) d.ref_off + delta);
			if (d.bloom_len > 0)
				d.bloom_off = (uint32) ((int64) d.bloom_off + delta);
			d.all_cube_off = (uint32) ((int64) d.all_cube_off + delta);
			if (d.nmonths > 0)
			{
				d.month_dir_off = (uint32) ((int64) d.month_dir_off + delta);
				rebase_cells(out->data + s + d.month_dir_off, d.nmonths, delta);
			}
			descs[nnodes++] = d;
		}
		if (r->root.flags & SIFT_L2_SINGLE_L1)
		{
			r->root.all_cube_off = descs[r->root.l1_first].all_cube_off;
			r->root.all_cube_len = descs[r->root.l1_first].all_cube_len;
			r->root.month_dir_off = descs[r->root.l1_first].month_dir_off;
		}
	}
	pool = out->len - s;
	wire_pad8(out);
	appendBinaryStringInfo(out, (char *) descs, nnodes * sizeof(*descs));
	{
		uint32		f[2] = {nnodes, 0};

		appendBinaryStringInfo(out, (char *) f, sizeof(f));
		appendBinaryStringInfo(out, (char *) &pool, sizeof(uint64));
	}
	end_section(out, dir, &nsec, s);

	s = begin_section(out, dir, &nsec, SEC_SCOPE_TOPK);
	for (uint32 i = 0; i < nrecs; i++)
		if (recs[i].root.flags & SIFT_L2_HAS_TOPK)
		{
			uint32		bytes = sift_rings_bytes(&recs[i].root, nrings, wide);

			SIFT_INVARIANT(out->len - s + bytes <= UINT32_MAX, "scope object: the rings of %u keys do not fit a 32-bit offset", i + 1);
			recs[i].root.topk_off = (uint32) (out->len - s);
			if (recs[i].topk_wide == wide)
				appendBinaryStringInfo(out, recs[i].topk, bytes);
			else
				put_ring_as(out, &recs[i], wide, nrings);
		}
	end_topk_section(out, dir, &nsec, s);

	s = begin_section(out, dir, &nsec, SEC_SCOPE_MONTHMAP);
	for (uint32 i = 0; i < nrecs; i++)
		if (recs[i].root.flags & SIFT_L2_HAS_MONTHMAP)
		{
			recs[i].root.monthmap_off = out->len - s;
			appendBinaryStringInfo(out, recs[i].monthmap, recs[i].root.monthmap_len);
			wire_pad8(out);
		}
	end_section(out, dir, &nsec, s);

	s = begin_section(out, dir, &nsec, SEC_SCOPE_L2);
	for (uint32 i = 0; i < nrecs; i++)
	{
		SiftKeyRec *r = &recs[i];

		if (r->l2block_len == 0)
			continue;
		{
			int64		delta;
			uint64		at;

			wire_pad8(out);
			at = out->len - s;
			delta = (int64) at - (int64) r->l2block_off;
			appendBinaryStringInfo(out, r->l2block, r->l2block_len);
			r->root.all_cube_off = (uint32) ((int64) r->root.all_cube_off + delta);
			if (r->root.nmonths > 0)
			{
				r->root.month_dir_off = (uint32) ((int64) r->root.month_dir_off + delta);
				rebase_cells(out->data + s + r->root.month_dir_off, r->root.nmonths, delta);
			}
		}
	}
	pool = out->len - s;
	wire_pad8(out);
	for (uint32 i = 0; i < nrecs; i++)
		appendBinaryStringInfo(out, (char *) &recs[i].root, sizeof(SiftScopeL2Root));
	nl2 = nrecs;
	{
		uint32		f[2] = {nl2, 0};

		appendBinaryStringInfo(out, (char *) f, sizeof(f));
		appendBinaryStringInfo(out, (char *) &pool, sizeof(uint64));
	}
	end_section(out, dir, &nsec, s);

	s = begin_section(out, dir, &nsec, SEC_SCOPE_KEYDIR);
	for (uint32 i = 0; i < nrecs; i += 256)
		appendBinaryStringInfo(out, (char *) &recs[i].root.scope_key, sizeof(int64));
	end_section(out, dir, &nsec, s);

	if (em->route_len > 0)
	{
		s = begin_section(out, dir, &nsec, SEC_SCOPE_ENT_ROUTE);
		appendBinaryStringInfo(out, em->route, em->route_len);
		end_section(out, dir, &nsec, s);
	}

	s = begin_section(out, dir, &nsec, SEC_SCOPE_DICT);
	appendBinaryStringInfo(out, em->dict, em->dict_len);
	end_section(out, dir, &nsec, s);

	hdr.magic = SIFT_SCOPE_META_MAGIC;
	hdr.version = SIFT_SCOPE_META_VER;
	hdr.nkeys = nrecs;
	hdr.nl1_nodes = nnodes;
	hdr.nl0_slices = em->l0_nentries;
	s = begin_section(out, dir, &nsec, SEC_SCOPE_HDR);
	appendBinaryStringInfo(out, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(out, (char *) em->parts, hdr.nparts * sizeof(SiftScopePartMapEntry));
	end_section(out, dir, &nsec, s);

	wire_pad8(out);
	tr = (SiftScopeTrailer) {SIFT_SCOPE_META_MAGIC, nsec, out->len};
	appendBinaryStringInfo(out, (char *) dir, nsec * sizeof(dir[0]));
	appendBinaryStringInfo(out, (char *) &tr, sizeof(tr));
	pfree(descs);
}

/* ---------------------------------------------------------------------------
 * 3. Reader
 * ------------------------------------------------------------------------- */

static bool
section_find(const char *base, uint64 len, const SiftScopeSecEntry *dir, uint32 nsec, uint16 tag, SiftScopeSection *s)
{
	for (uint32 i = 0; i < nsec; i++)
		if (dir[i].tag == tag)
		{
			if (dir[i].off > len || dir[i].len > len - dir[i].off)
				return false;
			s->p = base + dir[i].off;
			s->len = dir[i].len;
			return true;
		}
	return false;
}

/* The footer of a counted section: {count, pad, pool bytes} in its last 16 bytes; false when the section cannot hold what it says */
static bool
footer_read(const SiftScopeSection *s, uint32 *count, uint64 *pool, uint64 rec_size, uint64 *recs_off)
{
	uint32		f[2];

	if (s->len < 16)
		return false;
	memcpy(f, s->p + s->len - 16, 8);
	memcpy(pool, s->p + s->len - 8, 8);
	*count = f[0];
	if ((uint64) f[0] * rec_size + 16 > s->len || *pool > s->len)
		return false;
	*recs_off = s->len - 16 - (uint64) f[0] * rec_size;
	return *recs_off >= *pool;
}

/* The null_filter leaf of a header: none without SIFT_SMETA_NULL_FILTERED, else a leaf number */
static bool
filter_valid(const SiftScopeMetaHeader *h)
{
	return (h->flags & SIFT_SMETA_NULL_FILTERED) ? h->reserved[1] <= INT16_MAX : h->reserved[1] == 0;
}

int
sift_scope_null_filter_leaf(const SiftScopeMeta *m)
{
	return (m->hdr.flags & SIFT_SMETA_NULL_FILTERED) ? (int) m->hdr.reserved[1] : -1;
}

bool
sift_scope_serves_null_filter(const SiftScopeMeta *m, int is_null_leaf)
{
	return !(m->hdr.flags & SIFT_SMETA_NULL_FILTERED) || is_null_leaf == sift_scope_null_filter_leaf(m);
}

/* The description of the rings of a header: none without SIFT_SMETA_MULTI_TOPK, else 2..SIFT_SCOPE_TOPK_RINGS_CAP rings of leaves that are there */
static bool
rings_valid(const SiftScopeMetaHeader *h, uint32 *nrings)
{
	SiftScopeTopKDesc d;

	memcpy(&d, &h->reserved[0], sizeof(d));
	*nrings = 1;
	if (!(h->flags & SIFT_SMETA_MULTI_TOPK))
		return h->reserved[0] == 0;
	if (d.ntopk_rings < 2 || d.ntopk_rings > SIFT_SCOPE_TOPK_RINGS_CAP || h->time_leaf < 0 || d.nulls_last_mask != (1U << d.ntopk_rings) - 1)
		return false;
	for (uint32 i = 0; i < SIFT_SCOPE_TOPK_RINGS_CAP - 1; i++)
		if ((i < (uint32) d.ntopk_rings - 1) != (d.extra_leaf[i] >= 0))
			return false;
	*nrings = d.ntopk_rings;
	return true;
}

/* Unpacks the ring section of an object that stored it LZ4-compressed: topk then reads the raw bytes, as the offsets of the roots count them */
static bool
topk_unpack(SiftScopeMeta *m, bool lz4)
{
	uint32		raw_len;
	uint64		clen;
	char	   *raw;

	m->topk_stored = m->topk.len;
	if (!lz4)
		return true;
	if (m->topk.len <= sizeof(raw_len) || !sift_colz_available(SIFT_CC_ALG_LZ4))
		return false;
	memcpy(&raw_len, m->topk.p, sizeof(raw_len));
	clen = m->topk.len - sizeof(raw_len);
	/* LZ4 cannot expand more than 255 to 1, and the writer keeps a block only when it is smaller: anything else is a damaged length, and the buffer is never larger than the bytes can fill */
	if (raw_len <= clen || raw_len / 255 > clen ||
		raw_len > (uint64) m->hdr.nkeys * m->ntopk_rings * SIFT_SCOPE_TOPK_CAP * sizeof(SiftScopeTopKEntryWide))
		return false;
	/* MEM[MEM-SCOPE-08]: LZ4 ring section unpacked whole in lazy rings cx (nkeys*rings*depth*12 B) */
	raw = palloc(raw_len);
	if (!sift_colz_decompress(SIFT_CC_ALG_LZ4, m->topk.p + sizeof(raw_len), clen, raw, raw_len))
	{
		pfree(raw);
		return false;
	}
	m->topk_raw = raw;
	m->topk.p = raw;
	m->topk.len = raw_len;
	return true;
}

void
sift_scope_meta_close(SiftScopeMeta *m)
{
	if (m->topk_raw != NULL)
		pfree(m->topk_raw);
	m->topk_raw = NULL;
}

bool
sift_scope_meta_open(SiftScopeMeta *m, const char *base, uint64 len)
{
	return sift_scope_meta_open_ex(m, base, len, 0);
}

bool
sift_scope_meta_ensure_topk(SiftScopeMeta *m)
{
	bool		ok = true;

	if (m->topk_packed)
	{
		MemoryContext old = MemoryContextSwitchTo(m->topk_cx);

		ok = topk_unpack(m, true);
		MemoryContextSwitchTo(old);
		m->topk_packed = !ok;
	}
	return ok;
}

bool
sift_scope_meta_open_ex(SiftScopeMeta *m, const char *base, uint64 len, uint32 flags)
{
	SiftScopeTrailer tr;
	const SiftScopeSecEntry *dir;
	SiftScopeSection hdr;
	uint32		cnt;
	uint64		pool,
				off;
	bool		lz4 = false;

	memset(m, 0, sizeof(*m));
	if (len < sizeof(tr))
		return false;
	memcpy(&tr, base + len - sizeof(tr), sizeof(tr));
	if (tr.magic != SIFT_SCOPE_META_MAGIC || tr.nsec > SIFT_SCOPE_SECTIONS_CAP || tr.dir_off > len || (uint64) tr.nsec * sizeof(*dir) > len - sizeof(tr) - tr.dir_off ||
		len - sizeof(tr) - tr.dir_off != (uint64) tr.nsec * sizeof(*dir))
		return false;
	dir = (const SiftScopeSecEntry *) (base + tr.dir_off);
	if (!section_find(base, len, dir, tr.nsec, SEC_SCOPE_HDR, &hdr) || hdr.len < sizeof(m->hdr))
		return false;
	memcpy(&m->hdr, hdr.p, sizeof(m->hdr));
	if (m->hdr.magic != SIFT_SCOPE_META_MAGIC || m->hdr.version != SIFT_SCOPE_META_VER ||
		hdr.len != sizeof(m->hdr) + (uint64) m->hdr.nparts * sizeof(SiftScopePartMapEntry) ||
		((m->hdr.flags & SIFT_SMETA_TOPK_WIDE) != 0) != (m->hdr.total_granules > SIFT_TOPK_COMPACT_GRANS) || (m->hdr.flags & (SIFT_SMETA_VAR_TOPK | SIFT_SMETA_TOPK_DELTA)) != (SIFT_SMETA_VAR_TOPK | SIFT_SMETA_TOPK_DELTA))
		return false;
	if (!rings_valid(&m->hdr, &m->ntopk_rings) || !filter_valid(&m->hdr))
		return false;
	m->base = base;
	m->len = len;
	m->parts = (const SiftScopePartMapEntry *) (hdr.p + sizeof(m->hdr));
	if (!section_find(base, len, dir, tr.nsec, SEC_SCOPE_L0_SLICE, &m->l0) || !section_find(base, len, dir, tr.nsec, SEC_SCOPE_L1, &m->l1) ||
		!section_find(base, len, dir, tr.nsec, SEC_SCOPE_TOPK, &m->topk) || !section_find(base, len, dir, tr.nsec, SEC_SCOPE_MONTHMAP, &m->monthmap) ||
		!section_find(base, len, dir, tr.nsec, SEC_SCOPE_L2, &m->l2) || !section_find(base, len, dir, tr.nsec, SEC_SCOPE_KEYDIR, &m->keydir) ||
		!section_find(base, len, dir, tr.nsec, SEC_SCOPE_DICT, &m->dict))
		return false;
	if (section_find(base, len, dir, tr.nsec, SEC_SCOPE_ENT_ROUTE, &m->ent_route) &&
		(!(m->hdr.flags & SIFT_SMETA_CLUSTERED_PP) || !sift_ent_route_check(&m->ent_route)))
		return false;
	if (!footer_read(&m->l0, &cnt, &pool, sizeof(SiftScopeL0SliceEntry), &off) || cnt != m->hdr.nl0_slices)
		return false;
	m->slices = (const SiftScopeL0SliceEntry *) (m->l0.p + off);
	if (!footer_read(&m->l1, &cnt, &pool, sizeof(SiftScopeL1Node), &off) || cnt != m->hdr.nl1_nodes)
		return false;
	m->nodes = (const SiftScopeL1Node *) (m->l1.p + off);
	if (!footer_read(&m->l2, &cnt, &pool, sizeof(SiftScopeL2Root), &off) || cnt != m->hdr.nkeys)
		return false;
	m->roots = (const SiftScopeL2Root *) (m->l2.p + off);
	if (m->keydir.len != (uint64) ((m->hdr.nkeys + 255) / 256) * sizeof(int64))
		return false;
	for (uint32 i = 0; i < tr.nsec; i++)
	{
		if (dir[i].flags & ~(dir[i].tag == SEC_SCOPE_TOPK ? SIFT_SEC_LZ4 : 0U))
			return false;
		lz4 |= dir[i].tag == SEC_SCOPE_TOPK && (dir[i].flags & SIFT_SEC_LZ4);
	}
	if (lz4 && (flags & SIFT_SMETA_OPEN_LAZY_TOPK))
	{
		m->topk_stored = m->topk.len;
		m->topk_cx = CurrentMemoryContext;
		m->topk_packed = true;
		return true;
	}
	return topk_unpack(m, lz4);
}

const SiftScopeL2Root *
sift_scope_l2_find(const SiftScopeMeta *m, int64 scope_key)
{
	uint32		nf = m->keydir.len / sizeof(int64),
				lo = 0,
				hi = nf,
				f;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		int64		fence;

		memcpy(&fence, m->keydir.p + (uint64) mid * sizeof(int64), sizeof(int64));
		if (fence <= scope_key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return NULL;
	f = lo - 1;
	lo = f * 256;
	hi = Min(m->hdr.nkeys, lo + 256);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (m->roots[mid].scope_key == scope_key)
			return &m->roots[mid];
		if (m->roots[mid].scope_key < scope_key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

bool
sift_scope_gidx_part(const SiftScopeMeta *m, uint32 gidx, uint64 *part_gen, uint32 *granule_id)
{
	uint32		lo = 0,
				hi = m->hdr.nparts;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (m->parts[mid].base_granule_idx + m->parts[mid].ngranules <= gidx)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == m->hdr.nparts || m->parts[lo].base_granule_idx > gidx || m->parts[lo].part_gen == 0)
		return false;
	*part_gen = m->parts[lo].part_gen;
	*granule_id = gidx - m->parts[lo].base_granule_idx;
	return true;
}

uint32
sift_scope_node_refs(const SiftScopeMeta *m, const SiftScopeL1Node *n, SiftScopeGranRef *out)
{
	WireCur		c;
	int64		prev = 0;

	wire_cur(&c, m->l1.p + n->ref_off, n->ref_len);
	for (uint32 i = 0; i < n->nrefs; i++)
	{
		uint64		g = wire_get_var(&c);
		uint32		slice = (uint32) wire_get_var(&c) - 1;

		prev = i == 0 ? wire_unzig(g) : prev + wire_unzig(g);
		out[i] = (SiftScopeGranRef) {(uint32) prev, slice};
	}
	SIFT_INVARIANT(!c.bad, "scope object: a damaged ref list");
	return n->nrefs;
}

uint32
sift_scope_key_refs(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeGranRef *out)
{
	uint32		n = 0;

	for (uint32 i = 0; i < r->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &m->nodes[r->l1_first + i];

		if (out != NULL)
			sift_scope_node_refs(m, nd, out + n);
		n += nd->nrefs;
	}
	return n;
}

static const uint32 bloom_salt[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU, 0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

/* STOR[STOR-SCOPE-09]: member Bloom sized by node member count (32 B blocks; no skew sizing) */
uint32
sift_bloom_blocks(uint32 members)
{
	return Max(1, (members * 10 + 255) / 256);
}

static inline uint64
member_hash(int64 member)
{
	return hash_bytes_uint32_extended((uint32) member, (uint64) (member >> 32));
}

void
sift_bloom_add(char *bits, uint32 nblocks, int64 member)
{
	uint64		h = member_hash(member);
	uint32	   *blk = (uint32 *) (bits + (uint64) ((h >> 32) % nblocks) * 32);

	for (int i = 0; i < 8; i++)
		blk[i] |= 1U << (((uint32) h * bloom_salt[i]) >> 27);
}

bool
sift_bloom_has(const char *bits, uint32 len, int64 member)
{
	uint64		h = member_hash(member);
	uint32		nblocks = len / 32;
	uint32		blk[8];

	memcpy(blk, bits + (uint64) ((h >> 32) % nblocks) * 32, 32);
	for (int i = 0; i < 8; i++)
		if (!(blk[i] & (1U << (((uint32) h * bloom_salt[i]) >> 27))))
			return false;
	return true;
}

/* The exact members of a node: ascending int64 */
static inline int64
list_at(const char *list, uint32 i)
{
	int64		v;

	memcpy(&v, list + (uint64) i * 8, 8);
	return v;
}

static bool
sorted_list_has(const char *list, uint32 n, int64 member)
{
	uint32		lo = 0;

	while (lo < n)
	{
		uint32		mid = lo + (n - lo) / 2;
		int64		v = list_at(list, mid);

		if (v == member)
			return true;
		if (v < member)
			lo = mid + 1;
		else
			n = mid;
	}
	return false;
}

bool
sift_scope_node_has_member(const SiftScopeMeta *m, const SiftScopeL1Node *n, int64 member)
{
	if (member < n->member_lo || member > n->member_hi)
		return false;
	if (n->bloom_len == 0)
		return true;
	if (n->flags & SIFT_L1_HAS_BLOOM)
		return sift_bloom_has(m->l1.p + n->bloom_off, n->bloom_len, member);
	return sorted_list_has(m->l1.p + n->bloom_off, n->bloom_len / 8, member);
}

bool
sift_scope_node_hits_any(const SiftScopeMeta *m, const SiftScopeL1Node *n, const int64 *sorted, uint32 nm)
{
	uint32		lo = 0,
				hi = nm,
				i = 0,
				cnt = n->bloom_len / 8;
	const char *list = m->l1.p + n->bloom_off;

	if (nm == 0 || sorted[nm - 1] < n->member_lo || sorted[0] > n->member_hi)
		return false;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (sorted[mid] < n->member_lo)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (n->bloom_len == 0)
		return lo < nm && sorted[lo] <= n->member_hi;
	for (; lo < nm && sorted[lo] <= n->member_hi; lo++)
	{
		if (n->flags & SIFT_L1_HAS_BLOOM)
		{
			if (sift_bloom_has(list, n->bloom_len, sorted[lo]))
				return true;
			continue;
		}
		while (i < cnt && list_at(list, i) < sorted[lo])
			i++;
		if (i == cnt)
			return false;
		if (list_at(list, i) == sorted[lo])
			return true;
	}
	return false;
}

static bool
cube_at(const SiftScopeSection *s, uint32 off, uint32 len, SiftScopeCubeAcc *out)
{
	return (uint64) off + len <= s->len && sift_scube_read(s->p + off, len, out);
}

/* The cell of month in a directory of nmonths cells at off in s */
static bool
month_cell(const SiftScopeSection *s, uint32 off, uint32 nmonths, int32 month, SiftScopeMonthCell *cell)
{
	uint32		lo = 0,
				hi = nmonths;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		memcpy(cell, s->p + off + (uint64) mid * sizeof(*cell), sizeof(*cell));
		if (cell->month_id == month)
			return true;
		if (cell->month_id < month)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

bool
sift_scope_key_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, SiftScopeCubeAcc *out)
{
	const SiftScopeSection *s = (r->flags & SIFT_L2_SINGLE_L1) ? &m->l1 : &m->l2;
	SiftScopeMonthCell c;

	if (month == SIFT_SCOPE_NO_MONTH)
		return cube_at(s, r->all_cube_off, r->all_cube_len, out);
	return month_cell(s, r->month_dir_off, r->nmonths, month, &c) && cube_at(s, c.cube_off, c.cube_len, out);
}

bool
sift_scope_node_cube(const SiftScopeMeta *m, const SiftScopeL1Node *n, int32 month, SiftScopeCubeAcc *out)
{
	SiftScopeMonthCell c;

	if (month == SIFT_SCOPE_NO_MONTH)
		return cube_at(&m->l1, n->all_cube_off, n->all_cube_len, out);
	return month_cell(&m->l1, n->month_dir_off, n->nmonths, month, &c) && cube_at(&m->l1, c.cube_off, c.cube_len, out);
}

bool
sift_scope_slice_cube(const SiftScopeMeta *m, uint32 slice_id, SiftScopeCubeAcc *out)
{
	return slice_id < m->hdr.nl0_slices && cube_at(&m->l0, m->slices[slice_id].cube_off, m->slices[slice_id].cube_len, out);
}

uint32
sift_scope_group_by(const SiftScopeMeta *m, int32 month, void (*fn) (void *arg, const SiftScopeL2Root *r, const SiftScopeCubeAcc *c), void *arg)
{
	uint32		n = 0;

	for (uint32 i = 0; i < m->hdr.nkeys; i++)
	{
		SiftScopeCubeAcc c;

		if (!sift_scope_key_cube(m, &m->roots[i], month, &c))
			continue;
		fn(arg, &m->roots[i], &c);
		sift_scube_free(&c);
		n++;
	}
	return n;
}
