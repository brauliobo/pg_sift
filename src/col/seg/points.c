/*
 * col/seg/points.c - the point directory of a segment: the rows listed by key or by heap
 * address, the probes and lookups of a key, the addresses a unit holds
 * and the stream that hands them to a scan in order
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"

/* a[0, n) ascending: insertion for a short chunk, else a radix sort of bytes (a callback sort costs ten times the instructions on the codes of a chunk) */
static void
sort_u32(uint32 *a, uint32 n)
{
	uint32		max = 0,
			   *tmp,
			   *from = a,
			   *to;

	if (n <= 48)
	{
		for (uint32 i = 1; i < n; i++)
		{
			uint32		v = a[i],
						j = i;

			for (; j > 0 && a[j - 1] > v; j--)
				a[j] = a[j - 1];
			a[j] = v;
		}
		return;
	}
	for (uint32 i = 0; i < n; i++)
		max |= a[i];
	to = tmp = palloc(n * sizeof(uint32));
	for (int shift = 0; shift < 32 && (max >> shift) != 0; shift += 8)
	{
		uint32		cnt[256] = {0},
					sum = 0,
				   *t;

		for (uint32 i = 0; i < n; i++)
			cnt[(from[i] >> shift) & 255]++;
		for (int d = 0; d < 256; d++)
		{
			uint32		c = cnt[d];

			cnt[d] = sum;
			sum += c;
		}
		for (uint32 i = 0; i < n; i++)
			to[cnt[(from[i] >> shift) & 255]++] = from[i];
		t = from;
		from = to;
		to = t;
	}
	if (from != a)
		memcpy(a, from, n * sizeof(uint32));
	pfree(tmp);
}

/*
 * The codes of the list at word off of the leaf's lists in the coding of sift_cpoint_put_list (heap addresses, or rows of a table of the values of a list): ascending,
 * none past max. NULL, with the count in *cnt, when the list holds more than room codes. *nwords is the words the list takes.
 */
uint64 *
seg_point_list_read(SiftColSegment *seg, int col, const PointLeaf *pl, uint32 off, uint64 max, uint32 room, uint32 *cnt, uint32 *nwords)
{
	uint32		hdr[2],
				pad;
	uint64		base,
				prev = 0;
	uint8	   *buf;
	const uint8 *p,
			   *end;
	uint64	   *code;

	if (off >= pl->words || pl->words - off < 2)
		corrupt(seg, "The point directory of leaf %d has a list at word %u of %u.", col, off, pl->words);
	base = pl->at + (uint64) pl->nslots * sizeof(uint64) + (uint64) off * sizeof(uint32);
	seg_read(seg, base, sizeof(hdr), hdr);
	if (hdr[0] < 2 || hdr[0] > pl->count || hdr[1] < hdr[0] || hdr[1] > 6 * hdr[0] || (uint64) off + 2 + (hdr[1] + 3) / 4 > pl->words)
		corrupt(seg, "The point directory of leaf %d has a list of %u codes in %u bytes at word %u of %u.", col, hdr[0], hdr[1], off, pl->words);
	*cnt = hdr[0];
	if (nwords)
		*nwords = 2 + (hdr[1] + 3) / 4;
	if (hdr[0] > room)
		return NULL;
	pad = (hdr[1] + 3) / 4 * 4;
	buf = palloc(pad);
	seg_read(seg, base + sizeof(hdr), pad, buf);
	for (uint32 i = hdr[1]; i < pad; i++)
		if (buf[i] != 0)
			corrupt(seg, "The point directory of leaf %d has a list at word %u whose padding is not zero.", col, off);
	code = palloc(hdr[0] * sizeof(uint64));
	p = buf;
	end = buf + hdr[1];
	for (uint32 i = 0; i < hdr[0]; i++)
	{
		uint64		d;

		if ((p = sift_get_varint64_n(p, end, &d)) == NULL || (i > 0 && d == 0) || d > max || prev + d > max)
			corrupt(seg, "The point directory of leaf %d has a list at word %u that does not hold %u ascending codes.", col, off, hdr[0]);
		code[i] = prev += d;
	}
	if (p != end)
		corrupt(seg, "The point directory of leaf %d has a list at word %u that is longer than its %u codes.", col, off, hdr[0]);
	pfree(buf);
	return code;
}

/* The count of the list at word off of the leaf's lists, checked against the lists' words */
uint32
seg_point_list_count(SiftColSegment *seg, int col, const PointLeaf *pl, uint32 off)
{
	uint32		cnt;

	if (off >= pl->words)
		corrupt(seg, "The point directory of leaf %d has a list at word %u of %u.", col, off, pl->words);
	seg_read(seg, pl->at + (uint64) pl->nslots * sizeof(uint64) + (uint64) off * sizeof(uint32), sizeof(cnt), &cnt);
	if (cnt < 2 || cnt > pl->count || (uint64) off + 1 + cnt > pl->words)
		corrupt(seg, "The point directory of leaf %d has a list of %u rows at word %u of %u.", col, cnt, off, pl->words);
	return cnt;
}

/*
 * The entries of the packed table of leaf pl that may hold key: those of its bucket with its remainder. fn gets them as the slot of a table of the same mode
 * would have (the fingerprint over the row, or over the address) and the index of the entry as the position.
 */
static void
point_probe_packed(SiftColSegment *seg, int col, const PointLeaf *pl, int64 key, uint64 h, void (*fn) (void *, uint64, uint32), void *arg)
{
	const SiftCPackPar *par = &pl->pk;
	uint32		bucket,
				se[2],
				width = par->rbits + par->pbits,
				nent;
	uint64		rem,
				first;
	uint8	   *buf;
	uint64		dir = ((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7);

	if (!sift_cpoint_pk_locate(par, key, h, &bucket, &rem))
		return;
	seg_read(seg, pl->at + (uint64) bucket * sizeof(uint32), sizeof(se), se);
	if (se[0] > se[1] || se[1] > pl->nslots)
		corrupt(seg, "The packed point directory of leaf %d has the bucket %u from entry %u to %u of %u.", col, bucket, se[0], se[1], pl->nslots);
	if ((nent = se[1] - se[0]) == 0)
		return;
	first = (uint64) se[0] * width / 8;
	buf = palloc((uint64) (((uint64) se[1] * width + 7) / 8 - first) + 8);
	memset(buf + (((uint64) se[1] * width + 7) / 8 - first), 0, 8);
	seg_read(seg, pl->at + dir + first, ((uint64) se[1] * width + 7) / 8 - first, buf);
	for (uint32 j = 0; j < nent; j++)
	{
		uint64		at = (uint64) (se[0] + j) * width - first * 8;

		if (sift_cpoint_pk_get(buf, at, par->rbits) != rem)
			continue;
		{
			uint64		code = sift_cpoint_pk_get(buf, at + par->rbits, par->pbits);

			fn(arg, pl->tids ? (h & 0xFFFF) << 48 | sift_cpoint_pk_tid(par, code) : (h & 0xFFFFFFFF) << 32 | code, se[0] + j);
		}
	}
	pfree(buf);
}

/* Calls fn(slot payload) for each slot of leaf pl whose fingerprint is that of key's hash: the slots from the position of the hash to the first empty one */
static void
point_probe(SiftColSegment *seg, int col, const PointLeaf *pl, int64 key, void (*fn) (void *, uint64, uint32), void *arg)
{
	uint64		h = sift_cpoint_hash(key);
	uint32		p;

	if (pl->packed)
	{
		point_probe_packed(seg, col, pl, key, h, fn, arg);
		return;
	}
	p = sift_cpoint_pos(h, pl->nslots, pl->exact);
	for (uint32 steps = 0;; steps++)
	{
		uint64		win[8];
		uint32		m = Min(8, pl->nslots - p);
		uint32		j;

		if (steps > pl->nslots / 8 + 1)
			corrupt(seg, "The point directory of leaf %d has no empty slot.", col);
		seg_read(seg, pl->at + (uint64) p * sizeof(uint64), m * sizeof(uint64), win);
		for (j = 0; j < m && win[j] != SIFT_CPOINT_EMPTY; j++)
			if (pl->tids ? sift_cpoint_tid_slot_is(win[j], h) : sift_cpoint_slot_is(win[j], h))
				fn(arg, win[j], p + j);
		if (j < m)
			break;
		p = p + m == pl->nslots ? 0 : p + m;
	}
}

typedef struct PointRows
{
	SiftColSegment *seg;
	int			col;
	const PointLeaf *pl;
	uint32		total;
	uint32	   *r;
	uint32		n;
	uint32		cap;
	bool		over;			/* more rows than a point lookup takes */
} PointRows;

static void
point_add_rows(void *arg, uint64 slot, uint32 pos)
{
	PointRows  *pr = arg;
	uint32		payload = (uint32) slot,
				cnt = 1;
	uint64	   *code = NULL;

	if (pr->over)
		return;
	if (payload == SIFT_CPOINT_DENSE)
	{
		pr->over = true;
		return;
	}
	if ((payload & SIFT_CPOINT_LIST) && pr->pl->zrows)
	{
		code = seg_point_list_read(pr->seg, pr->col, pr->pl, payload & ~SIFT_CPOINT_LIST, pr->total - 1, SIFT_CPOINT_MAX_ROWS - pr->n, &cnt, NULL);
		if (code == NULL)
		{
			pr->over = true;
			return;
		}
	}
	else if (payload & SIFT_CPOINT_LIST)
		cnt = seg_point_list_count(pr->seg, pr->col, pr->pl, payload & ~SIFT_CPOINT_LIST);
	if (pr->n + cnt > SIFT_CPOINT_MAX_ROWS)
	{
		pr->over = true;
		return;
	}
	if (pr->n + cnt > pr->cap)
		pr->r = repalloc(pr->r, (pr->cap = Max(pr->cap * 2, pr->n + cnt)) * sizeof(uint32));
	if (code)
	{
		for (uint32 i = 0; i < cnt; i++)
			pr->r[pr->n + i] = (uint32) code[i];
		pfree(code);
	}
	else if (payload & SIFT_CPOINT_LIST)
		seg_read(pr->seg, pr->pl->at + (uint64) pr->pl->nslots * sizeof(uint64) + (uint64) ((payload & ~SIFT_CPOINT_LIST) + 1) * sizeof(uint32), cnt * sizeof(uint32), pr->r + pr->n);
	else
		pr->r[0 + pr->n] = payload;
	for (uint32 i = pr->n; i < pr->n + cnt; i++)
		if (pr->r[i] >= pr->total)
			corrupt(pr->seg, "The point directory of leaf %d lists row %u of %u.", pr->col, pr->r[i], pr->total);
	pr->n += cnt;
}

bool
sift_cseg_point_rows(SiftColSegment *seg, int col, const int64 *keys, int nkeys, uint32 **rows, uint32 *nrows)
{
	const PointLeaf *pl = seg_points_read(seg);
	PointRows	pr = {.seg = seg,.col = col,.cap = 16};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	pr.pl = pl + col;
	pr.total = seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows;
	pr.r = palloc(pr.cap * sizeof(uint32));
	for (int k = 0; k < nkeys && !pr.over; k++)
		point_probe(seg, col, pr.pl, keys[k], point_add_rows, &pr);
	if (pr.over)
	{
		pfree(pr.r);
		return false;
	}
	sort_u32(pr.r, pr.n);
	for (uint32 i = 0; i < pr.n; i++)
		if (u == 0 || pr.r[u - 1] != pr.r[i])
			pr.r[u++] = pr.r[i];
	*rows = pr.r;
	*nrows = u;
	return true;
}

typedef struct PointHas
{
	SiftColSegment *seg;
	int			col;
	const PointLeaf *pl;
	uint32		row;
	bool		found;
	bool		dense;
} PointHas;

static void
point_find_row(void *arg, uint64 slot, uint32 pos)
{
	PointHas   *ph = arg;
	uint32		payload = (uint32) slot,
				lo = 0,
				hi = 1,
				off = payload & ~SIFT_CPOINT_LIST;
	uint64		base = ph->pl->at + (uint64) ph->pl->nslots * sizeof(uint64);

	if (payload == SIFT_CPOINT_DENSE)
	{
		ph->found = ph->dense = true;
		return;
	}
	if (!(payload & SIFT_CPOINT_LIST))
	{
		ph->found |= payload == ph->row;
		return;
	}
	if (ph->pl->zrows)
	{
		uint64	   *code = seg_point_list_read(ph->seg, ph->col, ph->pl, off, SIFT_CPOINT_SEG_ROWS - 1, UINT32_MAX, &hi, NULL);

		for (uint32 i = 0; i < hi; i++)
			ph->found |= code[i] == ph->row;
		pfree(code);
		return;
	}
	hi = seg_point_list_count(ph->seg, ph->col, ph->pl, off);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2,
					v;

		seg_read(ph->seg, base + (uint64) (off + 1 + mid) * sizeof(uint32), sizeof(v), &v);
		if (v < ph->row)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < seg_point_list_count(ph->seg, ph->col, ph->pl, off))
	{
		uint32		v;

		seg_read(ph->seg, base + (uint64) (off + 1 + lo) * sizeof(uint32), sizeof(v), &v);
		ph->found |= v == ph->row;
	}
}

uint32
sift_cseg_point_ndense(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].ndense : 0;
}

bool
sift_cseg_point_has(SiftColSegment *seg, int col, int64 key, uint32 row, bool *dense)
{
	const PointLeaf *pl = seg_points_read(seg);
	PointHas	ph = {.seg = seg,.col = col,.row = row};

	if (pl == NULL || pl[col].nslots == 0 || pl[col].tids)
		return false;
	ph.pl = pl + col;
	point_probe(seg, col, ph.pl, key, point_find_row, &ph);
	*dense = ph.dense;
	return ph.found;
}

#define TID_PAYLOAD(slot)	((slot) & ((UINT64CONST(1) << 48) - 1))

static void
tid_push(TidAcc *a, SiftKey key)
{
	SiftColSegment *seg = a->seg;
	uint32		sb = (uint32) (key >> 16) >> SIFT_SB_SHIFT,
				u;

	if (a->seen && sb == a->last_sb)
		u = a->last_u;
	else
	{
		u = sift_cseg_unit_of_sb(seg, sb);
		if (u >= seg->nsb)
			corrupt(seg, "The point directory of leaf %d lists the address %llx, which is in no unit of the segment.", a->col, (unsigned long long) key);
		a->last_sb = sb;
		a->last_u = u;
		a->seen = true;
	}
	if ((key & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF || (key & 0xFFFF) == 0)
		corrupt(seg, "The point directory of leaf %d lists the address %llx, which is in no unit of the segment.", a->col, (unsigned long long) key);
	if (a->keep && !sift_col_bit_test(a->keep, u))
		return;
	if (a->n == a->cap)
		a->t = a->t ? repalloc_huge(a->t, (a->cap *= 2) * sizeof(SiftKey)) : palloc((a->cap = 16) * sizeof(SiftKey));
	a->t[a->n++] = key;
}

/* Appends the addresses of one slot of a table of addresses to a (ascending in a list), and counts the words of its list */
void
seg_tid_slot_read(TidAcc *a, uint64 slot)
{
	const PointLeaf *pl = a->pl;
	uint64		pay = TID_PAYLOAD(slot);
	uint32		cnt,
				nwords;
	uint64	   *code;

	a->slots++;
	if ((pay & 0xFFFF) != SIFT_CPOINT_TID_LIST)
	{
		if (a->limit && a->n + 1 > a->limit)
			a->over = true;
		else
			tid_push(a, pay);
		return;
	}
	if (pay >> 48)
		corrupt(a->seg, "The point directory of leaf %d has a list at word %llu of %u.", a->col, (unsigned long long) (pay >> 16), pl->words);
	code = seg_point_list_read(a->seg, a->col, pl, (uint32) (pay >> 16), (UINT64CONST(1) << 41) - 1, a->limit ? a->limit - a->n : UINT32_MAX, &cnt, &nwords);
	a->words += nwords;
	if (code == NULL)
	{
		a->over = true;
		return;
	}
	for (uint32 i = 0; i < cnt; i++)
	{
		tid_push(a, sift_cpoint_tid_key(code[i]));
		/* the units ascend with the addresses: the rest of the list is in units past the last one read */
		if (a->keep && a->last_u > a->last_keep)
			break;
	}
	pfree(code);
}

static void
point_add_tids(void *arg, uint64 slot, uint32 pos)
{
	TidAcc	   *a = arg;

	if (a->slot_seen != NULL)
	{
		if (a->slot_seen[pos / 64] >> (pos % 64) & 1)
			return;
		a->slot_seen[pos / 64] |= UINT64CONST(1) << (pos % 64);
	}
	if (!a->over)
		seg_tid_slot_read(a, slot);
}

bool
sift_cseg_point_is_tid(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl != NULL && pl[col].nslots != 0 && pl[col].tids;
}

static bool
point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, uint32 limit, uint64 *seen, SiftKey **tids, uint32 *ntids)
{
	const PointLeaf *pl = seg_points_read(seg);
	TidAcc		a = {.seg = seg,.col = col,.limit = limit,.keep = keep,.slot_seen = seen};
	uint32		u = 0;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids)
		return false;
	a.pl = pl + col;
	/* the bits past the last unit of the bitmap are not units */
	for (uint32 w = 0; keep && w < (seg->nsb + 63) / 64; w++)
	{
		uint64		bits = w == seg->nsb / 64 ? keep[w] & ((UINT64CONST(1) << (seg->nsb % 64)) - 1) : keep[w];

		if (bits)
		{
			a.last_keep = w * 64 + 63 - __builtin_clzll(bits);
			a.some = true;
		}
	}
	/* no unit to read: no address is needed */
	if (keep && !a.some)
	{
		*tids = NULL;
		*ntids = 0;
		return true;
	}
	for (int k = 0; k < nkeys && !a.over; k++)
		point_probe(seg, col, a.pl, keys[k], point_add_tids, &a);
	if (a.over)
	{
		if (a.t)
			pfree(a.t);
		return false;
	}
	/* the addresses of one slot ascend and are distinct; those of several slots (several keys, or keys that share a fingerprint) are sorted and made so */
	if (a.slots > 1)
	{
		sift_sort_u64(a.t, a.n);
		for (uint32 i = 0; i < a.n; i++)
			if (u == 0 || a.t[u - 1] != a.t[i])
				a.t[u++] = a.t[i];
	}
	else
		u = a.n;
	*tids = a.t;
	*ntids = u;
	return true;
}

bool
sift_cseg_point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, SiftKey **tids, uint32 *ntids)
{
	return point_tids(seg, col, keys, nkeys, keep, SIFT_CPOINT_MAX_ROWS, NULL, tids, ntids);
}

/* ---- the addresses of a lookup read as far as the caller goes: a cursor over each list the keys name, merged ---- */

#define TCUR_BUF		2048		/* the bytes of a list read at a time: a code is at most 10 */

#define TCUR_MAX		8			/* lists a stream merges: more slots are decoded whole (sift_cseg_point_tids) */

typedef struct TidCur
{
	uint64		at;				/* the file position of the next byte of the list not read yet */
	uint32		unread;			/* ... and how many bytes of it there are */
	uint32		remaining;		/* addresses of the list not taken yet */
	uint32		taken;
	uint64		prev;
	SiftKey		head;
	bool		have;
	uint32		off;			/* word of the list, for the messages */
	uint32		bpos;
	uint32		blen;
	uint32		chunk;			/* the bytes of the next read: it doubles, so a scan that stops early reads little and one that goes on reads whole pages */
	uint8		buf[TCUR_BUF + 16];
} TidCur;

struct SiftPointStream
{
	SiftColSegment *seg;
	int			col;
	int			n;				/* the lists */
	TidCur	   *cur[TCUR_MAX];
	SiftKey	   *one;			/* the slots that hold one address, ascending and distinct, from pone */
	uint32		none;
	uint32		pone;
	SiftKey		last;
	bool		any;
	uint64	   *slots;
	int			nslots;
	int			nlists;
	/* a lookup of many keys (SIFT_CPOINT_CHUNK_MIN or more) is read a chunk of keys at a time: the keys not looked up yet, and the addresses of the chunk in hand */
	int64	   *keys;			/* NULL for the places of a dictionary: the keys are the codes at places pfrom + kpos on of its byte order (sift_cseg_point_stream_open_places) */
	uint32		pfrom;
	int			nkeys;
	int			kpos;
	int			kchunk;
	SiftKey    *buf;
	uint32		nbuf;
	uint32		pbuf;
	uint64	   *seen;			/* a bit for each slot of the table that a chunk visited: two keys with one fingerprint list the same slot, and the executor must not get a row twice */
};

static void
stream_slot(void *arg, uint64 slot, uint32 pos)
{
	SiftPointStream *s = arg;

	if ((TID_PAYLOAD(slot) & 0xFFFF) == SIFT_CPOINT_TID_LIST && s->nlists++ == TCUR_MAX)
		return;
	if (s->nslots % 64 == 0)
		s->slots = s->slots ? repalloc(s->slots, (s->nslots + 64) * sizeof(uint64)) : palloc(64 * sizeof(uint64));
	s->slots[s->nslots++] = slot;
}

/* The next address of a list into its head (have: false past its last) */
static void
tcur_advance(SiftPointStream *s, TidCur *t)
{
	uint64		d = 0;
	int			shift = 0;
	uint8		b;

	if (t->remaining == 0)
	{
		if (t->bpos != t->blen || t->unread != 0)
			corrupt(s->seg, "The point directory of leaf %d has a list at word %u that is longer than its addresses.", s->col, t->off);
		t->have = false;
		return;
	}
	if (t->blen - t->bpos < 10 && t->unread > 0)
	{
		uint32		rem = t->blen - t->bpos,
					take = Min(t->unread, Min(t->chunk, TCUR_BUF - rem));

		memmove(t->buf, t->buf + t->bpos, rem);
		seg_read(s->seg, t->at, take, t->buf + rem);
		t->at += take;
		t->unread -= take;
		t->bpos = 0;
		t->blen = rem + take;
		t->chunk = Min(t->chunk * 2, TCUR_BUF);
	}
	do
	{
		if (t->bpos >= t->blen || shift > 63)
			corrupt(s->seg, "The point directory of leaf %d has a list at word %u that does not hold its addresses.", s->col, t->off);
		b = t->buf[t->bpos++];
		d |= (uint64) (b & 0x7F) << shift;
		shift += 7;
	} while (b & 0x80);
	if ((t->taken > 0 && d == 0) || (d >> 41) || ((t->prev + d) >> 41))
		corrupt(s->seg, "The point directory of leaf %d has a list at word %u that does not hold ascending addresses.", s->col, t->off);
	t->prev += d;
	t->head = sift_cpoint_tid_key(t->prev);
	t->have = true;
	t->taken++;
	t->remaining--;
}

/* The addresses of the next chunk of keys of a chunked stream into buf, those not given before; false when every key was looked up */
static bool
stream_chunk(SiftPointStream *s)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(s));

	while (s->kpos < s->nkeys)
	{
		int			k = Min(s->nkeys - s->kpos, s->kchunk);
		SiftKey    *tids;
		uint32		n;
		bool		found;
		int64	   *places = NULL;
		const int64 *kk = s->keys != NULL ? s->keys + s->kpos : NULL;

		if (kk == NULL)
		{
			DictLook   *l = seg_dict_look(s->seg, s->col);
			uint32	   *codes = palloc(k * sizeof(uint32));

			sift_cseg_dict_perm_read(l, s->pfrom + s->kpos, k, codes);
			/* in the order of the codes, which follows that of the rows: the addresses of the keys come nearly in order of the heap, and the sort of them finds it so */
			sort_u32(codes, k);
			places = palloc(k * sizeof(int64));
			for (int j = 0; j < k; j++)
				places[j] = codes[j];
			pfree(codes);
			kk = places;
		}
		found = point_tids(s->seg, s->col, kk, k, NULL, 0, s->seen, &tids, &n);
		if (places)
			pfree(places);
		SIFT_INVARIANT(found, "sift_col: the directory of heap addresses of leaf %d gave no lookup", s->col);
		s->kpos += k;
		s->kchunk = Min(s->kchunk * 4, SIFT_CPOINT_CHUNK_LAST);
		if (s->buf)
			pfree(s->buf);
		s->buf = tids;
		s->nbuf = n;
		s->pbuf = 0;
		if (n > 0)
		{
			MemoryContextSwitchTo(old);
			return true;
		}
	}
	MemoryContextSwitchTo(old);
	return false;
}

SiftPointStream *
sift_cseg_point_stream_open(SiftColSegment *seg, int col, const int64 *keys, int nkeys)
{
	const PointLeaf *pl = seg_points_read(seg);
	SiftPointStream *s;
	uint64		rows = 0;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids)
		return NULL;
	s = palloc0(sizeof(SiftPointStream));
	/*
	 * A list of many keys is looked up a chunk at a time (a LIMIT stops after the first): the first chunk's keys cost what they cost, the others wait. The cap on
	 * the rows of the segment bounds what a key can list, as a lookup that finds more rows than SIFT_CPOINT_MAX_ROWS cannot be refused halfway.
	 */
	if (nkeys >= SIFT_CPOINT_CHUNK_MIN && (uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows <= SIFT_CPOINT_CHUNK_ROWS)
	{
		s->seg = seg;
		s->col = col;
		s->keys = palloc(nkeys * sizeof(int64));
		memcpy(s->keys, keys, nkeys * sizeof(int64));
		s->nkeys = nkeys;
		s->kchunk = SIFT_CPOINT_CHUNK_FIRST;
		s->seen = palloc0(((Size) pl[col].nslots + 63) / 64 * sizeof(uint64));
		return s;
	}
	s->seg = seg;
	s->col = col;
	for (int k = 0; k < nkeys && s->nlists <= TCUR_MAX; k++)
		point_probe(seg, col, pl + col, keys[k], stream_slot, s);
	/* more lists than the stream merges are decoded whole (sift_cseg_point_tids) */
	if (s->nlists > TCUR_MAX)
		return NULL;
	s->one = palloc(Max(s->nslots, 1) * sizeof(SiftKey));
	for (int i = 0; i < s->nslots; i++)
	{
		uint64		pay = TID_PAYLOAD(s->slots[i]);
		TidCur	   *t;
		uint32		hdr[2];
		uint32		off;
		uint64		base;

		if ((pay & 0xFFFF) != SIFT_CPOINT_TID_LIST)
		{
			s->one[s->none++] = pay;
			rows++;
			continue;
		}
		off = (uint32) (pay >> 16);
		base = pl[col].at + (uint64) pl[col].nslots * sizeof(uint64) + (uint64) off * sizeof(uint32);
		if (pay >> 48 || off >= pl[col].words || pl[col].words - off < 2)
			corrupt(seg, "The point directory of leaf %d has a list at word %llu of %u.", col, (unsigned long long) (pay >> 16), pl[col].words);
		seg_read(seg, base, sizeof(hdr), hdr);
		if (hdr[0] < 2 || hdr[0] > pl[col].count || hdr[1] < hdr[0] || hdr[1] > 6 * hdr[0] || (uint64) off + 2 + (hdr[1] + 3) / 4 > pl[col].words)
			corrupt(seg, "The point directory of leaf %d has a list of %u addresses in %u bytes at word %u of %u.", col, hdr[0], hdr[1], off, pl[col].words);
		t = palloc(sizeof(TidCur));
		/* the fields one by one: a compound literal would clear and copy the buffer */
		t->at = base + sizeof(hdr);
		t->unread = hdr[1];
		t->remaining = hdr[0];
		t->taken = 0;
		t->prev = 0;
		t->have = false;
		t->off = off;
		t->bpos = t->blen = 0;
		t->chunk = 256;
		s->cur[s->n++] = t;
		rows += hdr[0];
	}
	/* a lookup that finds more rows is not a point lookup: the units answer it */
	if (rows > SIFT_CPOINT_MAX_ROWS)
		return NULL;
	if (s->none > 1)
	{
		uint32		u = 1;

		sift_sort_u64(s->one, s->none);
		for (uint32 i = 1; i < s->none; i++)
			if (s->one[u - 1] != s->one[i])
				s->one[u++] = s->one[i];
		s->none = u;
	}
	for (int i = 0; i < s->n; i++)
		tcur_advance(s, s->cur[i]);
	return s;
}

/*
 * The stream of the lookup of the codes at places [from, to) of the byte order of a dictionary read in place (a prefix or a range of a text leaf), looked up a chunk at a time
 * without making the keys: NULL when the leaf has no addresses or the segment is too large for chunks (the caller makes the keys)
 */
SiftPointStream *
sift_cseg_point_stream_open_places(SiftColSegment *seg, int col, uint32 from, uint32 to)
{
	const PointLeaf *pl = seg_points_read(seg);
	SiftPointStream *s;

	if (pl == NULL || pl[col].nslots == 0 || !pl[col].tids || (uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows > SIFT_CPOINT_CHUNK_ROWS)
		return NULL;
	s = palloc0(sizeof(SiftPointStream));
	s->seg = seg;
	s->col = col;
	s->pfrom = from;
	s->nkeys = to - from;
	s->kchunk = SIFT_CPOINT_CHUNK_FIRST;
	s->seen = palloc0(((Size) pl[col].nslots + 63) / 64 * sizeof(uint64));
	return s;
}

/* The codes at places [from, to) of the byte order of a dictionary read in place into keys, to - from of them */
void
sift_cseg_dict_codes(SiftColSegment *seg, int col, uint32 from, uint32 to, int64 *keys)
{
	DictLook   *l = seg_dict_look(seg, col);
	uint32	   *codes = palloc((Size) (to - from) * sizeof(uint32));

	sift_cseg_dict_perm_read(l, from, to - from, codes);
	for (uint32 i = 0; i < to - from; i++)
		keys[i] = codes[i];
	pfree(codes);
}

uint32
sift_cseg_point_stream_next(SiftPointStream *s, SiftKey *out, uint32 max)
{
	uint32		n = 0;

	if (s->nkeys > 0)
	{
		while (n < max && (s->pbuf < s->nbuf || stream_chunk(s)))
		{
			uint32		take = Min(max - n, s->nbuf - s->pbuf);

			memcpy(out + n, s->buf + s->pbuf, take * sizeof(SiftKey));
			s->pbuf += take;
			n += take;
		}
		return n;
	}
	while (n < max)
	{
		TidCur	   *best = NULL;
		SiftKey		head = s->pone < s->none ? s->one[s->pone] : PG_UINT64_MAX;

		for (int i = 0; i < s->n; i++)
			if (s->cur[i]->have && s->cur[i]->head < head)
			{
				best = s->cur[i];
				head = best->head;
			}
		if (best == NULL && s->pone == s->none)
			break;
		if (!s->any || head != s->last)
			out[n++] = head;
		s->last = head;
		s->any = true;
		if (best)
			tcur_advance(s, best);
		else
			s->pone++;
	}
	return n;
}

typedef struct TidKey
{
	int64		key;
	SiftKey		tid;
} TidKey;

static int
tid_key_cmp(const void *a, const void *b)
{
	const TidKey *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->tid > y->tid) - (x->tid < y->tid);
}

bool
sift_cseg_point_check_tids(SiftColSegment *seg, int col, const int64 *key, const uint64 *nulls, const SiftKey *tid, uint32 n, uint64 *listed)
{
	const PointLeaf *pl = seg_points_read(seg);
	TidKey	   *e = palloc(Max(n, 1) * sizeof(TidKey));
	uint32		m = 0;
	bool		ok = true;

	Assert(pl != NULL && pl[col].tids);
	for (uint32 i = 0; i < n; i++)
		if (!(nulls && (nulls[i / 64] >> (i % 64) & 1)))
			e[m++] = (TidKey) {key[i], tid[i]};
	qsort(e, m, sizeof(TidKey), tid_key_cmp);
	for (uint32 i = 0, j; i < m && ok; i = j)
	{
		TidAcc		a = {.seg = seg,.col = col,.pl = pl + col};
		uint32		k = 0;

		for (j = i; j < m && e[j].key == e[i].key; j++)
			;
		point_probe(seg, col, a.pl, e[i].key, point_add_tids, &a);
		if (a.n > 0)
			sift_sort_u64(a.t, a.n);
		for (uint32 r = i; r < j && ok; r++)
		{
			while (k < a.n && a.t[k] < e[r].tid)
				k++;
			ok = k < a.n && a.t[k] == e[r].tid;
		}
		if (a.t)
			pfree(a.t);
	}
	pfree(e);
	*listed += m;
	return ok;
}

uint32
sift_cseg_point_count(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].count : 0;
}

/* the distinct keys of the leaf's directory */
uint32
sift_cseg_point_nkeys(SiftColSegment *seg, int col)
{
	const PointLeaf *pl = seg_points_read(seg);

	return pl ? pl[col].nkeys : 0;
}
