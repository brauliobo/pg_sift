/*
 * col/runs.c - the sorted run of a leaf in a unit (docs/COL.md 2.14). Portable (core/host.h only).
 *
 * Blob of a unit and leaf (little endian): u8 kind, u8 width, u16 flags (0), u32 n (rows of the unit), u32 nvals, u32 nnull, then the entries.
 *     ORD (kind 1): width = bytes of an entry (1 up to 256 rows, 2 up to 65536, else 4); n entries, each the ordinal of a row: first the nvals rows with a value
 *                   ascending by (key, ordinal), then the nnull NULL rows ascending, then the rows whose value overflowed the leaf ascending.
 *     IDENT (kind 2): no entries, width 0: the unit holds no NULL or overflowed row and its keys do not fall from one row to the next, so the run is
 *                   the rows in order (nvals = n).
 * The key is the chunk's (sift_crun_key). Ties sort by ordinal, which is the order of the heap TIDs of the unit.
 */

#include "core/host.h"

#include "col/runs.h"
#include "core/cmp.h"

typedef struct RunEnt
{
	int64		key;
	uint32		ord;
} RunEnt;

static int
ent_cmp(const void *a, const void *b)
{
	const RunEnt *x = a,
			   *y = b;

	return x->key != y->key ? SIFT_CMP3(x->key, y->key) : SIFT_CMP3(x->ord, y->ord);
}

static bool
isset64(const uint64 *bits, uint32 i)
{
	return bits != NULL && (bits[i / 64] >> (i % 64) & 1);
}

static uint8
width_of(uint32 n)
{
	return n <= 256 ? 1 : n <= 65536 ? 2 : 4;
}

uint32
sift_crun_bytes(uint8 kind, uint32 nrows)
{
	return SIFT_CRUN_HDR + (kind == SIFT_CRUN_ORD ? (uint32) width_of(nrows) * nrows : 0);
}

bool
sift_crun_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	if (d->kind != SIFT_COLK_SCALAR || d->parent >= 0 || (d->path != NULL && d->path[0] != '\0') || (d->flags & SIFT_COLD_LOSSY) || sift_col_def_needs_wide(d))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_NUMERIC:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return d->store == SIFT_COLS_INT;
		case SIFT_COLT_FLOAT4:
		case SIFT_COLT_FLOAT8:
			return d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;
		default:
			return false;
	}
}

const char *
sift_crun_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, sift_crun_leaf_ok, "not an integer, date, timestamp, numeric or float column");
}

const char *
sift_crun_syntax(const char *opt)
{
	return sift_cpagg_leaf_list(opt, NULL, NULL, NULL, NULL, NULL);
}

static void
put_head(StringInfo out, uint8 kind, uint8 width, uint32 n, uint32 nvals, uint32 nnull)
{
	uint8		h[SIFT_CRUN_HDR] = {kind, width};

	memcpy(h + 4, &n, sizeof(n));
	memcpy(h + 8, &nvals, sizeof(nvals));
	memcpy(h + 12, &nnull, sizeof(nnull));
	appendBinaryStringInfo(out, (const char *) h, sizeof(h));
}

static void
put_entry(StringInfo out, uint8 width, uint32 v)
{
	uint8		b[4] = {(uint8) v, (uint8) (v >> 8), (uint8) (v >> 16), (uint8) (v >> 24)};

	appendBinaryStringInfo(out, (const char *) b, width);
}

bool
sift_crun_build(const SiftColDef *def, const SiftCPaggCol *c, StringInfo out)
{
	uint32		n = c->n,
				nvals = 0,
				nnull = 0;
	uint8		w = width_of(n);
	bool		ident = true;
	RunEnt	   *e;

	if (n < 2 || !c->ready)
		return false;
	e = palloc(n * sizeof(RunEnt));
	for (uint32 i = 0; i < n; i++)
	{
		if (isset64(c->ovf, i))
			ident = false;
		else if (isset64(c->nulls, i))
		{
			nnull++;
			ident = false;
		}
		else
		{
			e[nvals] = (RunEnt) {sift_crun_key(def, c->key[i]), i};
			ident = ident && (nvals == 0 || e[nvals].key >= e[nvals - 1].key);
			nvals++;
		}
	}
	if (ident)
		put_head(out, SIFT_CRUN_IDENT, 0, n, n, 0);
	else
	{
		put_head(out, SIFT_CRUN_ORD, w, n, nvals, nnull);
		qsort(e, nvals, sizeof(RunEnt), ent_cmp);
		for (uint32 i = 0; i < nvals; i++)
			put_entry(out, w, e[i].ord);
		for (uint32 i = 0; i < n; i++)
			if (isset64(c->nulls, i) && !isset64(c->ovf, i))
				put_entry(out, w, i);
		for (uint32 i = 0; i < n; i++)
			if (isset64(c->ovf, i))
				put_entry(out, w, i);
	}
	pfree(e);
	return true;
}

bool
sift_crun_head(const uint8 *blob, uint32 len, uint32 nrows, SiftCRun *r)
{
	uint32		n,
				nvals,
				nnull;

	if (len < SIFT_CRUN_HDR || blob[2] != 0 || blob[3] != 0)
		return false;
	memcpy(&n, blob + 4, sizeof(n));
	memcpy(&nvals, blob + 8, sizeof(nvals));
	memcpy(&nnull, blob + 12, sizeof(nnull));
	if (n != nrows || nvals > n || nnull > n - nvals)
		return false;
	if (blob[0] == SIFT_CRUN_IDENT)
	{
		if (blob[1] != 0 || nvals != n || nnull != 0 || len != SIFT_CRUN_HDR)
			return false;
	}
	else if (blob[0] != SIFT_CRUN_ORD || blob[1] != width_of(n) || len != sift_crun_bytes(SIFT_CRUN_ORD, n))
		return false;
	*r = (SiftCRun) {blob[0], blob[1], n, nvals, nnull};
	return true;
}

bool
sift_crun_entries(const SiftCRun *r, const uint8 *e, uint32 pos, uint32 cnt, uint32 *out)
{
	if (pos > r->n || cnt > r->n - pos)
		return false;
	if (r->kind == SIFT_CRUN_IDENT)
	{
		for (uint32 i = 0; i < cnt; i++)
			out[i] = pos + i;
		return true;
	}
	for (uint32 i = 0; i < cnt; i++)
	{
		uint32		v = 0;

		for (uint32 b = 0; b < r->width; b++)
			v |= (uint32) e[(uint64) i * r->width + b] << (8 * b);
		if (v >= r->n)
			return false;
		out[i] = v;
	}
	return true;
}

bool
sift_crun_matches(const SiftColDef *def, const SiftCPaggCol *c, const uint8 *blob, uint32 len)
{
	StringInfoData want;
	bool		same;

	initStringInfo(&want);
	same = sift_crun_build(def, c, &want) && want.len == (int) len && memcmp(want.data, blob, len) == 0;
	pfree(want.data);
	return same;
}
