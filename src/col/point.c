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

const char *
sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, leaf_ok, "not a plain integer, text or uuid column of the top level");
}

const char *
sift_cpoint_syntax(const char *opt)
{
	return sift_cpagg_leaf_list(opt, NULL, NULL, NULL, NULL, NULL);
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
sift_cpoint_slots(uint32 n)
{
	uint64		s = 16;

	while (s * 3 < (uint64) n * 4)
		s *= 2;
	return (uint32) s;
}

typedef struct PointEnt
{
	int64		key;
	uint64		val;				/* the row, or the heap address of the row */
} PointEnt;

static int
ent_cmp(const void *a, const void *b)
{
	const PointEnt *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return SIFT_CMP3(x->val, y->val);
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

/* ... of several heap addresses: the count, the bytes and the deltas of their codes as varints, padded to a word */
static uint64
tid_list(StringInfo lists, const PointEnt *e, uint32 cnt)
{
	StringInfoData body;
	uint32		nbytes;
	uint64		prev = 0,
				payload = (uint64) (lists->len / sizeof(uint32)) << 16 | SIFT_CPOINT_TID_LIST;

	initStringInfo(&body);
	for (uint32 k = 0; k < cnt; k++)
	{
		uint64		code = sift_cpoint_tid_code(e[k].val);

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
	return payload;
}

/* The directory of the keys, by the rows (row) or by the heap addresses (tid): a key with several values points at a list in the words after the slots */
static void
point_build(const int64 *key, const uint32 *row, const uint64 *tid, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	PointEnt   *e = palloc(n * sizeof(PointEnt));
	uint32		nk = 0;
	uint64	   *t;
	StringInfoData lists;

	for (uint32 i = 0; i < n; i++)
		e[i] = (PointEnt) {key[i], tid ? tid[i] : row[i]};
	qsort(e, n, sizeof(PointEnt), ent_cmp);
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		nk++;
	}
	*nslots = sift_cpoint_slots(nk);
	*nkeys = nk;
	t = palloc(*nslots * sizeof(uint64));
	memset(t, 0xFF, *nslots * sizeof(uint64));
	initStringInfo(&lists);
	for (uint32 i = 0, j; i < n; i = j)
	{
		uint64		h = sift_cpoint_hash(e[i].key);
		uint32		p = sift_cpoint_pos(h, *nslots);
		uint64		payload = e[i].val;

		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		if (j - i > 1)
			payload = tid ? tid_list(&lists, e + i, j - i) : row_list(&lists, e + i, j - i);
		while (t[p] != SIFT_CPOINT_EMPTY)
			p = (p + 1) & (*nslots - 1);
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
sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	point_build(key, row, NULL, n, out, nslots, nkeys, words);
}

void
sift_cpoint_build_tids(const int64 *key, const uint64 *tid, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	point_build(key, NULL, tid, n, out, nslots, nkeys, words);
}
