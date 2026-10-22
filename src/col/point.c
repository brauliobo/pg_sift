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
