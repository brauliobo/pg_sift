/*
 * col/point.c - the point directory of a leaf in a segment (col/point.h). Portable (core/host.h only).
 */
#include "core/host.h"

#include "col/point.h"
#include "col/preagg.h"
#include "col/setsum.h"
#include "core/cmp.h"

static bool
leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return d->parent < 0 && sift_cset_leaf_ok(s, d);
}

/* 'auto: a, b' is the choice of the build (col/keycols.c) and reads as the list 'a, b'; 'auto' is none */
const char *
sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	bool		autolist;

	return sift_cpagg_leaf_list(sift_cpagg_auto_list(opt, &autolist), s, names, leaf, leaf_ok, "not a plain integer, text or uuid column of the top level");
}

const char *
sift_cpoint_syntax(const char *opt)
{
	bool		autolist;

	return sift_cpagg_leaf_list(sift_cpagg_auto_list(opt, &autolist), NULL, NULL, NULL, NULL, NULL);
}

uint64
sift_cpoint_hash(int64 key)
{
	uint64		x = (uint64) key;

	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64CONST(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

uint32
sift_cpoint_slots(uint32 n, bool exact)
{
	uint64		s = 16;

	if (exact)
		return (uint32) Max(((uint64) n * 4 + 2) / 3, s);
	while (s * 3 < (uint64) n * 4)
		s *= 2;
	return (uint32) s;
}

typedef struct PointEnt
{
	int64		key;
	uint64		val;				/* the row, or the heap address of the row */
} PointEnt;

static inline int
ent_cmp(const PointEnt *x, const PointEnt *y)
{
	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return SIFT_CMP3(x->val, y->val);
}

#define ENT_SORT_LESS(x, y) (ent_cmp(x, y) < 0)
SIFT_DEFINE_SORT(ent_sort_cmp, PointEnt, ENT_SORT_LESS)

/* a copy of the entries for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
ent_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define ENT_KEY(x) ((x)->key)
#define ENT_NOKEY(x) 0
#define ENT_TIE(x) ((x)->val)
SIFT_DEFINE_RADIX_SORT(ent_sort, PointEnt, ENT_KEY, ENT_NOKEY, ENT_TIE, ent_copy, pfree, ent_sort_cmp)

static int
fp_cmp(const void *a, const void *b)
{
	return SIFT_CMP3(*(const uint32 *) a, *(const uint32 *) b);
}

uint64
sift_cpoint_pk_bytes(const SiftCPackPar *par, uint32 n)
{
	return (((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7)) + ((((uint64) n * (par->rbits + par->pbits) + 7) / 8 + 7) & ~UINT64CONST(7));
}

static void
pk_put(uint8 *buf, uint64 at, uint32 n, uint64 v)
{
	uint64		w;

	memcpy(&w, buf + at / 8, sizeof(w));
	w |= v << (at % 8);
	memcpy(buf + at / 8, &w, sizeof(w));
}

/* floor(log2(x)) of x >= 1 */
static uint32
log2_floor(uint64 x)
{
	return 63 - (uint32) __builtin_clzll(x);
}

/* the bucket of each of the n entries (sorted by k) are counted: the most in one */
static uint32
pk_maxrun(const PointEnt *e, uint32 n, uint32 rbits)
{
	uint32		most = 0;

	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && (uint64) e[j].key >> rbits == (uint64) e[i].key >> rbits; j++)
			;
		most = Max(most, j - i);
	}
	return most;
}

/*
 * The packed table of the entries e (sorted by key and code; e[].val is the payload code, at most maxcode). Dense when the keys lie in a range that gives a
 * remainder of fewer bits than the hash does, else by the hash of the key (the entries are then sorted again by k). The entries' key is rewritten to k.
 */
static bool
pk_build(const PointEnt *src, uint32 n, uint64 maxcode, uint64 pbase, uint32 pmul, uint64 limit, SiftCPackPar *par, StringInfo out, uint32 *nkeys)
{
	uint32		pbits = log2_floor(maxcode | 1) + 1,
				lg = 0;
	uint64		span = (uint64) src[n - 1].key - (uint64) src[0].key,
				range = span + 1;
	PointEnt   *e;
	uint8	   *buf;
	uint32	   *dir;
	uint64		bytes,
				dirbytes,
				at = 0;
	uint32		keys = 0,
				b = 0;
	bool		ok = false;

	*par = (SiftCPackPar) {.pbase = pbase,.pmul = pmul,.pbits = pbits};
	if (pbits > 48)
		return false;
	e = palloc(n * sizeof(PointEnt));
	memcpy(e, src, n * sizeof(PointEnt));
	if (span < (UINT64CONST(1) << 56))
	{
		uint32		r = log2_floor(Max(1, range * SIFT_CPOINT_PK_BUCKET / n));

		if (r < SIFT_CPOINT_PK_RBITS)
		{
			int64		kmin = e[0].key;

			for (uint32 i = 0; i < n; i++)
				e[i].key = (int64) ((uint64) e[i].key - (uint64) kmin);
			if (pk_maxrun(e, n, r) <= SIFT_CPOINT_PK_RUN)
			{
				par->dense = 1;
				par->rbits = r;
				par->kmin = kmin;
				par->nbuckets = (uint32) (((range - 1) >> r) + 1);
			}
			else
				memcpy(e, src, n * sizeof(PointEnt));
		}
	}
	if (!par->dense)
	{
		lg = n < 2 * SIFT_CPOINT_PK_BUCKET ? 0 : log2_floor(n / SIFT_CPOINT_PK_BUCKET);
		par->rbits = SIFT_CPOINT_PK_RBITS;
		par->nbuckets = UINT32_C(1) << lg;
		for (uint32 i = 0; i < n; i++)
			e[i].key = (int64) (sift_cpoint_hash(e[i].key) >> (64 - lg - par->rbits));
		ent_sort(e, n);
		if (pk_maxrun(e, n, par->rbits) > SIFT_CPOINT_PK_RUN)
			goto done;
	}
	bytes = sift_cpoint_pk_bytes(par, n);
	if (limit > 0 && bytes >= limit)
		goto done;
	dirbytes = ((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7);
	buf = palloc0(bytes + 16);
	dir = (uint32 *) buf;
	for (uint32 i = 0; i < n; i++)
	{
		uint32		bk = (uint32) ((uint64) e[i].key >> par->rbits);

		for (; b <= bk; b++)
			dir[b] = i;
		keys += i == 0 || e[i].key != e[i - 1].key;
		pk_put(buf + dirbytes, at, par->rbits, (uint64) e[i].key & ((UINT64CONST(1) << par->rbits) - 1));
		pk_put(buf + dirbytes, at + par->rbits, pbits, e[i].val);
		at += par->rbits + pbits;
	}
	for (; b <= par->nbuckets; b++)
		dir[b] = n;
	appendBinaryStringInfo(out, (const char *) buf, (int) bytes);
	pfree(buf);
	*nkeys = keys;
	ok = true;
done:
	pfree(e);
	if (!ok)
		*par = (SiftCPackPar) {0};
	return ok;
}

/* A key of several rows: the count and the rows as words after the slots; the payload names the list */
static uint64
row_list(StringInfo lists, const PointEnt *e, uint32 cnt)
{
	uint64		payload = SIFT_CPOINT_LIST | (uint32) (lists->len / sizeof(uint32));

	appendBinaryStringInfo(lists, (const char *) &cnt, sizeof(cnt));
	for (uint32 k = 0; k < cnt; k++)
	{
		uint32		r = (uint32) e[k].val;

		appendBinaryStringInfo(lists, (const char *) &r, sizeof(r));
	}
	return payload;
}

/*
 * ... of several heap addresses (tid), or of the rows of the values of a list: the count, the bytes and the deltas of their codes as varints, padded to a word.
 * The first code of a list of rows is the row itself (it may be 0).
 */
static uint64
delta_list(StringInfo lists, const PointEnt *e, uint32 cnt, bool tid)
{
	StringInfoData body;
	uint32		nbytes,
				word = lists->len / sizeof(uint32);
	uint64		prev = 0;

	initStringInfo(&body);
	for (uint32 k = 0; k < cnt; k++)
	{
		uint64		code = tid ? sift_cpoint_tid_code(e[k].val) : e[k].val;

		sift_col_put_varint(&body, code - prev);
		prev = code;
	}
	nbytes = body.len;
	appendBinaryStringInfo(lists, (const char *) &cnt, sizeof(cnt));
	appendBinaryStringInfo(lists, (const char *) &nbytes, sizeof(nbytes));
	appendBinaryStringInfo(lists, body.data, body.len);
	while (lists->len % 4)
		appendStringInfoChar(lists, '\0');
	pfree(body.data);
	return tid ? (uint64) word << 16 | SIFT_CPOINT_TID_LIST : SIFT_CPOINT_LIST | word;
}

/*
 * The packed table of the entries e (n of them, sorted, nk distinct keys) when it is smaller than the slots and the lists of the keys would be (a list is 2 bytes
 * a row when it holds addresses, 4 when it holds rows), as pk_build does it: the payload code is the row, or the address as an offset in the range of the addresses.
 */
static bool
pk_try(const PointEnt *e, uint32 n, uint32 nk, bool tid, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	uint64		old = (uint64) sift_cpoint_slots(nk, true) * sizeof(uint64),
				maxcode = 0,
				pbase = UINT64_MAX;
	uint32		pmul = 1,
				at = out->len;
	PointEnt   *q = palloc(n * sizeof(PointEnt));
	bool		ok;

	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		if (j - i > 1)
			old += 8 + (tid ? 2 : 4) * (uint64) (j - i);
		if (j - i > SIFT_CPOINT_PK_RUN)
		{
			pfree(q);
			return false;
		}
	}
	if (tid)
	{
		uint32		maxoff = 0;

		for (uint32 i = 0; i < n; i++)
		{
			pbase = Min(pbase, e[i].val >> 16);
			maxoff = Max(maxoff, (uint32) (e[i].val & 0xFFFF));
		}
		pmul = maxoff + 1;
	}
	else
		pbase = 0;
	for (uint32 i = 0; i < n; i++)
	{
		q[i] = (PointEnt) {e[i].key, tid ? ((e[i].val >> 16) - pbase) * pmul + (e[i].val & 0xFFFF) : e[i].val};
		maxcode = Max(maxcode, q[i].val);
	}
	ok = pk_build(q, n, maxcode, pbase, pmul, old, par, out, nkeys);
	pfree(q);
	if (ok)
	{
		*nslots = n;
		*words = (out->len - at) / sizeof(uint32);
	}
	return ok;
}

/*
 * The directory of the keys, by the rows (row) or by the heap addresses (tid): a key with several values points at a list in the words after the slots.
 * dense > 0 is the table of the values of a list: a row is listed once for a key, the lists are the delta lists, a key in more than `dense` rows has the
 * payload SIFT_CPOINT_DENSE and no list, and a table in which two keys share a fingerprint is not built (no slots): its slots must name the key. *nrows is
 * the rows the lists hold, a dense key counting one.
 */
static void
point_build(const int64 *key, const uint32 *row, const uint64 *tid, uint32 n, uint32 dense, bool exact, bool packed, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words, uint32 *nrows)
{
	PointEnt   *e = palloc(n * sizeof(PointEnt));
	uint32		nk = 0,
				nr = 0;
	uint64	   *t;
	StringInfoData lists;

	for (uint32 i = 0; i < n; i++)
		e[i] = (PointEnt) {key[i], tid ? tid[i] : row[i]};
	ent_sort(e, n);
	if (dense > 0)
	{
		uint32		m = 0;

		for (uint32 i = 0; i < n; i++)
			if (m == 0 || e[i].key != e[m - 1].key || e[i].val != e[m - 1].val)
				e[m++] = e[i];
		n = m;
	}
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		nk++;
		nr += dense > 0 && j - i > dense ? 1 : j - i;
	}
	*par = (SiftCPackPar) {0};
	if (packed && dense == 0 && n > 0 && pk_try(e, n, nk, tid != NULL, par, out, nslots, nkeys, words))
	{
		*nrows = n;
		pfree(e);
		return;
	}
	if (dense > 0)
	{
		uint32	   *fp = palloc(Max(nk, 1) * sizeof(uint32));
		uint32		m = 0;
		bool		clash = false;

		for (uint32 i = 0, j; i < n; i = j)
		{
			for (j = i + 1; j < n && e[j].key == e[i].key; j++)
				;
			fp[m++] = (uint32) (sift_cpoint_hash(e[i].key) & 0xFFFFFFFF);
		}
		qsort(fp, m, sizeof(uint32), fp_cmp);
		for (uint32 i = 1; i < m && !clash; i++)
			clash = fp[i] == fp[i - 1];
		pfree(fp);
		if (clash)
		{
			*nslots = *nkeys = *words = *nrows = 0;
			pfree(e);
			return;
		}
	}
	*nslots = sift_cpoint_slots(nk, exact);
	*nkeys = nk;
	*nrows = nr;
	t = palloc(*nslots * sizeof(uint64));
	memset(t, 0xFF, *nslots * sizeof(uint64));
	initStringInfo(&lists);
	for (uint32 i = 0, j; i < n; i = j)
	{
		uint64		h = sift_cpoint_hash(e[i].key);
		uint32		p = sift_cpoint_pos(h, *nslots, exact);
		uint64		payload = e[i].val;

		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		if (dense > 0 && j - i > dense)
			payload = SIFT_CPOINT_DENSE;
		else if (j - i > 1)
			payload = tid || dense > 0 ? delta_list(&lists, e + i, j - i, tid != NULL) : row_list(&lists, e + i, j - i);
		while (t[p] != SIFT_CPOINT_EMPTY)
			p = sift_cpoint_next(p, *nslots);
		t[p] = tid ? (h & 0xFFFF) << 48 | payload : (h & 0xFFFFFFFF) << 32 | payload;
	}
	*words = lists.len / sizeof(uint32);
	appendBinaryStringInfo(out, (const char *) t, (int) (*nslots * sizeof(uint64)));
	appendBinaryStringInfo(out, lists.data, lists.len);
	if (*words % 2)
		appendBinaryStringInfo(out, "\0\0\0\0", 4);
	pfree(lists.data);
	pfree(t);
	pfree(e);
}

void
sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, uint32 dense, bool exact, bool packed, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words, uint32 *nrows)
{
	point_build(key, row, NULL, n, dense, exact, packed, par, out, nslots, nkeys, words, nrows);
}

void
sift_cpoint_build_tids(const int64 *key, const uint64 *tid, uint32 n, bool exact, bool packed, SiftCPackPar *par, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	uint32		nrows;

	point_build(key, NULL, tid, n, 0, exact, packed, par, out, nslots, nkeys, words, &nrows);
}
