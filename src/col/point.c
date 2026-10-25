/*
 * col/point.c - the point directory of a leaf in a segment (col/point.h). Portable (core/host.h only).
 */
#include "core/host.h"

#include "col/point.h"
#include "col/preagg.h"
#include "col/setsum.h"

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
	uint32		row;
} PointEnt;

static int
ent_cmp(const void *a, const void *b)
{
	const PointEnt *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->row > y->row) - (x->row < y->row);
}

void
sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	PointEnt   *e = palloc(n * sizeof(PointEnt));
	uint32		nk = 0,
				nw = 0,
				at = 0;
	uint64	   *t;
	uint32	   *w;

	for (uint32 i = 0; i < n; i++)
		e[i] = (PointEnt) {key[i], row[i]};
	qsort(e, n, sizeof(PointEnt), ent_cmp);
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		nk++;
		if (j - i > 1)
			nw += 1 + (j - i);
	}
	*nslots = sift_cpoint_slots(nk);
	*nkeys = nk;
	*words = nw;
	t = palloc(*nslots * sizeof(uint64));
	memset(t, 0xFF, *nslots * sizeof(uint64));
	w = palloc(Max(nw, 1) * sizeof(uint32));
	for (uint32 i = 0, j; i < n; i = j)
	{
		uint64		h = sift_cpoint_hash(e[i].key);
		uint32		p = sift_cpoint_pos(h, *nslots);
		uint32		payload = e[i].row;

		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		if (j - i > 1)
		{
			payload = SIFT_CPOINT_LIST | at;
			w[at++] = j - i;
			for (uint32 k = i; k < j; k++)
				w[at++] = e[k].row;
		}
		while (t[p] != SIFT_CPOINT_EMPTY)
			p = (p + 1) & (*nslots - 1);
		t[p] = (h & 0xFFFFFFFF) << 32 | payload;
	}
	appendBinaryStringInfo(out, (const char *) t, (int) (*nslots * sizeof(uint64)));
	appendBinaryStringInfo(out, (const char *) w, (int) (nw * sizeof(uint32)));
	if (nw % 2)
		appendBinaryStringInfo(out, "\0\0\0\0", 4);
	pfree(t);
	pfree(w);
	pfree(e);
}

bool
sift_cpoint_get_varint(const uint8 **p, const uint8 *end, uint64 *v)
{
	uint64		x = 0;

	for (int shift = 0; shift <= 63 && *p < end; shift += 7)
	{
		uint8		b = *(*p)++;

		x |= (uint64) (b & 0x7F) << shift;
		if (!(b & 0x80))
		{
			*v = x;
			return true;
		}
	}
	return false;
}

typedef struct TidEnt
{
	int64		key;
	uint64		tid;
} TidEnt;

static int
tid_ent_cmp(const void *a, const void *b)
{
	const TidEnt *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->tid > y->tid) - (x->tid < y->tid);
}

void
sift_cpoint_build_tids(const int64 *key, const uint64 *tid, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words)
{
	TidEnt	   *e = palloc(n * sizeof(TidEnt));
	uint32		nk = 0,
				nw = 0;
	uint64	   *t;
	StringInfoData lists;

	for (uint32 i = 0; i < n; i++)
		e[i] = (TidEnt) {key[i], tid[i]};
	qsort(e, n, sizeof(TidEnt), tid_ent_cmp);
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
		uint64		payload = e[i].tid;

		for (j = i + 1; j < n && e[j].key == e[i].key; j++)
			;
		if (j - i > 1)
		{
			StringInfoData body;
			uint32		cnt = j - i,
						nbytes;
			uint64		prev = 0;

			initStringInfo(&body);
			for (uint32 k = i; k < j; k++)
			{
				uint64		code = sift_cpoint_tid_code(e[k].tid);

				sift_col_put_varint(&body, code - prev);
				prev = code;
			}
			nbytes = body.len;
			payload = (uint64) (lists.len / sizeof(uint32)) << 16 | SIFT_CPOINT_TID_LIST;
			appendBinaryStringInfo(&lists, (const char *) &cnt, sizeof(cnt));
			appendBinaryStringInfo(&lists, (const char *) &nbytes, sizeof(nbytes));
			appendBinaryStringInfo(&lists, body.data, body.len);
			while (lists.len % 4)
				appendStringInfoChar(&lists, '\0');
			pfree(body.data);
		}
		while (t[p] != SIFT_CPOINT_EMPTY)
			p = (p + 1) & (*nslots - 1);
		t[p] = (h & 0xFFFF) << 48 | payload;
	}
	nw = lists.len / sizeof(uint32);
	*words = nw;
	appendBinaryStringInfo(out, (const char *) t, (int) (*nslots * sizeof(uint64)));
	appendBinaryStringInfo(out, lists.data, lists.len);
	if (nw % 2)
		appendBinaryStringInfo(out, "\0\0\0\0", 4);
	pfree(lists.data);
	pfree(t);
	pfree(e);
}
