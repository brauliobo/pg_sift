/*
 * col/ktab.c - the key table of a segment (col/ktab.h). Portable (core/host.h only). The writer and sift.col_verify call the same accumulator, so the stored
 * values are what the units say.
 *
 * A blob: a KtHdr, then ng records of the keys in ascending order (the NULL key's last): the key and the rows (KtGroup up to m), then nm KtMeas.
 */

#include "core/host.h"

#include "col/ktab.h"
#include "core/cmp.h"

#define KT_MEAS		SIFT_KTAB_MEAS
#define KT_NONE		UINT32_MAX

typedef struct KtMeas
{
	uint32		nn;
	uint32		pad;
	int64		mn,
				mx;
	uint64		sum[2];				/* low and high word of the exact sum */
} KtMeas;

typedef struct KtGroup
{
	int64		key;				/* 0 for the NULL key */
	uint32		cnt;
	uint32		pad;
	KtMeas		m[KT_MEAS];
} KtGroup;

typedef struct KtHdr
{
	uint32		leaf;
	uint32		ng;
	uint16		nm;
	uint16		nullg;				/* the last record is the NULL key's */
	int16		meas[KT_MEAS];
} KtHdr;

StaticAssertDecl(sizeof(KtMeas) == 40 && sizeof(KtGroup) == 16 + KT_MEAS * 40 && sizeof(KtHdr) == SIFT_KTAB_HEADER_BYTES, "the key table records have no padding but their own");

#define KT_REC(nm)	(offsetof(KtGroup, m) + (nm) * sizeof(KtMeas))

typedef struct KtLeaf
{
	int			leaf;
	bool		dead;				/* more keys than a table holds, or a unit it could not read */
	int			nm;
	int16		meas[KT_MEAS];
	uint32		ng,
				cap,
				hsize,
				nullg;
	uint32	   *hg;					/* [hsize]: the group of the key in the slot, KT_NONE empty; the NULL group is not in it */
	KtGroup    *grp;				/* [cap], zero beyond ng */
} KtLeaf;

struct SiftCKtab
{
	const SiftColSchema *s;
	MemoryContext cx;
	int			nleaf;
	KtLeaf	   *leaf;
	bool		meas_ok[SIFT_COL_MAX_COLS];	/* a measure some unit could not give is left out of every table */
};

/* The integer leaves of the top level whose values order as their keys and add */
static bool
meas_leaf(const SiftColDef *d)
{
	return d->parent < 0 && d->kind == SIFT_COLK_SCALAR && d->store == SIFT_COLS_INT &&
		(d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_INT8 || d->type == SIFT_COLT_NUMERIC ||
		 d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ);
}

/* What an aggregate asks of a key most: decimals, then dates and times, then integers wide to narrow */
static int
meas_rank(const SiftColDef *d)
{
	return d->type == SIFT_COLT_NUMERIC ? 0 : (d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ) ? 1 :
		d->type == SIFT_COLT_INT8 ? 2 : d->type == SIFT_COLT_INT4 ? 3 : 4;
}

static int
ktab_measures(const SiftColSchema *s, int c, int16 *meas)
{
	int			n = 0;

	for (int rank = 0; rank <= 4; rank++)
		for (int m = 0; m < s->ncols && n < KT_MEAS; m++)
			if (m != c && meas_leaf(&s->cols[m]) && meas_rank(&s->cols[m]) == rank)
				meas[n++] = m;
	return n;
}

void
sift_ktab_spec(const SiftColSchema *s, SiftCPaggSpec *spec)
{
	for (int c = 0; c < s->ncols; c++)
		spec->ktn[c] = spec->key_table && spec->set[c] && meas_leaf(&s->cols[c]) ? ktab_measures(s, c, spec->ktm[c]) : 0;
}

static inline uint32
slot_of(int64 k, uint32 mask)
{
	return (uint32) (((uint64) k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & mask;
}

/* Doubles the groups and the hash table */
static void
grow(SiftCKtab *t, KtLeaf *l)
{
	MemoryContext old = MemoryContextSwitchTo(t->cx);
	uint32		cap = l->cap ? l->cap * 2 : 256;

	l->grp = l->grp ? repalloc(l->grp, cap * sizeof(KtGroup)) : palloc(cap * sizeof(KtGroup));
	memset(l->grp + l->cap, 0, (cap - l->cap) * sizeof(KtGroup));
	l->cap = cap;
	if (l->hg)
		pfree(l->hg);
	l->hsize = 2 * cap;
	l->hg = palloc(l->hsize * sizeof(uint32));
	memset(l->hg, 0xFF, l->hsize * sizeof(uint32));
	for (uint32 g = 0; g < l->ng; g++)
		if (g != l->nullg)
		{
			uint32		h = slot_of(l->grp[g].key, l->hsize - 1);

			while (l->hg[h] != KT_NONE)
				h = (h + 1) & (l->hsize - 1);
			l->hg[h] = g;
		}
	MemoryContextSwitchTo(old);
}

SiftCKtab *
sift_ktab_begin(const SiftColSchema *s, const bool *key, MemoryContext mcxt)
{
	SiftCKtab  *t = MemoryContextAllocZero(mcxt, sizeof(SiftCKtab));

	t->s = s;
	t->cx = mcxt;
	t->leaf = MemoryContextAllocZero(mcxt, s->ncols * sizeof(KtLeaf));
	for (int c = 0; c < s->ncols; c++)
	{
		KtLeaf	   *l = &t->leaf[t->nleaf];

		t->meas_ok[c] = true;
		if (!key[c] || !meas_leaf(&s->cols[c]))
			continue;
		l->leaf = c;
		l->nullg = KT_NONE;
		l->nm = ktab_measures(s, c, l->meas);
		grow(t, l);
		t->nleaf++;
	}
	return t->nleaf > 0 ? t : NULL;
}

/* A new group, zeroed (KT_NONE: the leaf holds too many keys) */
static uint32
add_group(SiftCKtab *t, KtLeaf *l)
{
	if (l->ng == SIFT_KTAB_MAX_GROUPS)
		return KT_NONE;
	if (l->ng == l->cap)
		grow(t, l);
	return l->ng++;
}

static KtGroup *
group_of(SiftCKtab *t, KtLeaf *l, bool isnull, int64 k)
{
	uint32		h = 0,
				mask;

	if (isnull)
	{
		if (l->nullg == KT_NONE && (l->nullg = add_group(t, l)) == KT_NONE)
			return NULL;
		return &l->grp[l->nullg];
	}
	mask = l->hsize - 1;
	for (h = slot_of(k, mask); l->hg[h] != KT_NONE && l->grp[l->hg[h]].key != k; h = (h + 1) & mask)
		;
	if (l->hg[h] == KT_NONE)
	{
		uint32		g = add_group(t, l);

		if (g == KT_NONE)
			return NULL;
		l->grp[g].key = k;
		mask = l->hsize - 1;
		for (h = slot_of(k, mask); l->hg[h] != KT_NONE; h = (h + 1) & mask)
			;
		l->hg[h] = g;
	}
	return &l->grp[l->hg[h]];
}

static inline bool
is_null(const uint64 *nulls, uint32 i)
{
	return nulls != NULL && ((nulls[i / 64] >> (i % 64)) & 1);
}

static bool
usable(const SiftCPaggCol *c, uint32 nrows)
{
	if (!c->ready || c->n != nrows)
		return false;
	for (uint32 w = 0; c->ovf != NULL && w < (nrows + 63) / 64; w++)
		if (c->ovf[w])
			return false;
	return true;
}

/* The leading measures of the leaf that every unit gave */
static int
live_meas(const SiftCKtab *t, const KtLeaf *l)
{
	int			nm = 0;

	while (nm < l->nm && t->meas_ok[l->meas[nm]])
		nm++;
	return nm;
}

void
sift_ktab_add(SiftCKtab *t, const SiftCPaggCol *cols, uint32 nrows)
{
	for (int m = 0; m < t->s->ncols; m++)
		if (t->meas_ok[m] && meas_leaf(&t->s->cols[m]) && !usable(&cols[m], nrows))
			t->meas_ok[m] = false;
	for (int i = 0; i < t->nleaf; i++)
	{
		KtLeaf	   *l = &t->leaf[i];
		const SiftCPaggCol *kc = &cols[l->leaf];
		int			nm = live_meas(t, l);

		l->dead = l->dead || !usable(kc, nrows);
		if (l->dead)
			continue;
		for (uint32 r = 0; r < nrows; r++)
		{
			KtGroup    *g = group_of(t, l, is_null(kc->nulls, r), kc->key[r]);

			if (g == NULL)
			{
				l->dead = true;
				break;
			}
			g->cnt++;
			for (int j = 0; j < nm; j++)
			{
				const SiftCPaggCol *mc = &cols[l->meas[j]];
				KtMeas	   *x = &g->m[j];
				int64		v = mc->key[r];
				uint64		lo = x->sum[0];

				if (is_null(mc->nulls, r))
					continue;
				x->sum[0] += (uint64) v;
				x->sum[1] += (uint64) (v >> 63) + (x->sum[0] < lo);
				if (x->nn++ == 0)
					x->mn = x->mx = v;
				else
				{
					x->mn = Min(x->mn, v);
					x->mx = Max(x->mx, v);
				}
			}
		}
	}
}

static int
cmp_key(const void *a, const void *b)
{
	return SIFT_CMP3(((const KtGroup *) a)->key, ((const KtGroup *) b)->key);
}

static Size
blob_size(uint32 ng, int nm)
{
	return sizeof(KtHdr) + (Size) ng * KT_REC(nm);
}

static void
put_blob(const KtLeaf *l, int nm, StringInfo out)
{
	KtGroup    *s = palloc(l->ng * sizeof(KtGroup));
	uint32		n = 0;
	KtHdr		h = {l->leaf, l->ng, nm, l->nullg != KT_NONE};

	for (uint32 g = 0; g < l->ng; g++)
		if (g != l->nullg)
			s[n++] = l->grp[g];
	qsort(s, n, sizeof(KtGroup), cmp_key);
	if (l->nullg != KT_NONE)
		s[n++] = l->grp[l->nullg];
	memcpy(h.meas, l->meas, nm * sizeof(int16));
	appendBinaryStringInfo(out, (const char *) &h, sizeof(h));
	for (uint32 i = 0; i < n; i++)
		appendBinaryStringInfo(out, (const char *) &s[i], KT_REC(nm));
	pfree(s);
}

void
sift_ktab_finish(SiftCKtab *t, Size bytes, StringInfo out)
{
	Size		budget = bytes / 1000 * SIFT_KTAB_PERMILLE;

	for (int i = 0; i < t->nleaf; i++)
	{
		KtLeaf	   *l = &t->leaf[i];
		int			nm = live_meas(t, l);

		if (l->dead || l->ng == 0)
			continue;
		while (nm > 0 && blob_size(l->ng, nm) > budget)
			nm--;
		if (blob_size(l->ng, nm) > budget)
			continue;
		budget -= blob_size(l->ng, nm);
		put_blob(l, nm, out);
	}
}

bool
sift_ktab_check(const SiftCKtab *t, const SiftCHistDim *d)
{
	const KtLeaf *l = NULL;
	StringInfoData blob;
	SiftCHistDim e;

	for (int i = 0; i < t->nleaf; i++)
		if (t->leaf[i].leaf == d->leaf)
			l = &t->leaf[i];
	if (l == NULL || l->dead || d->nmeas > live_meas(t, l))
		return false;
	initStringInfo(&blob);
	put_blob(l, d->nmeas, &blob);
	if (sift_ktab_decode(t->s, (uint8 *) blob.data, blob.len, t->cx, &e) != blob.len || e.ngroups != d->ngroups || e.has_null != d->has_null ||
		memcmp(e.key, d->key, e.ngroups * sizeof(int64)) != 0 || memcmp(e.count, d->count, e.ngroups * sizeof(uint32)) != 0 ||
		memcmp(e.meas, d->meas, d->nmeas * sizeof(int16)) != 0)
		return false;
	for (int m = 0; m < d->nmeas; m++)
		if (memcmp(e.nn[m], d->nn[m], e.ngroups * sizeof(uint32)) != 0 || memcmp(e.sum[m], d->sum[m], e.ngroups * sizeof(SiftCI128)) != 0 ||
			memcmp(e.min[m], d->min[m], e.ngroups * sizeof(int64)) != 0 || memcmp(e.max[m], d->max[m], e.ngroups * sizeof(int64)) != 0)
			return false;
	return true;
}

uint32
sift_ktab_decode(const SiftColSchema *s, const uint8 *p, uint32 len, MemoryContext cx, SiftCHistDim *out)
{
	KtHdr		h;
	MemoryContext old;
	Size		size;

	if (len < sizeof(h))
		return 0;
	memcpy(&h, p, sizeof(h));
	if (h.ng == 0 || h.ng > SIFT_KTAB_MAX_GROUPS || h.nm > KT_MEAS || h.nullg > 1 || h.leaf >= (uint32) s->ncols || !meas_leaf(&s->cols[h.leaf]) ||
		(size = blob_size(h.ng, h.nm)) > len)
		return 0;
	for (int m = 0; m < KT_MEAS; m++)
		if (m < h.nm ? (h.meas[m] < 0 || h.meas[m] >= s->ncols || !meas_leaf(&s->cols[h.meas[m]])) : h.meas[m] != 0)
			return 0;
	memset(out, 0, sizeof(*out));
	out->leaf = h.leaf;
	out->ngroups = h.ng;
	out->has_null = h.nullg;
	out->minmax = true;
	out->nmeas = h.nm;
	old = MemoryContextSwitchTo(cx);
	out->key = palloc(h.ng * sizeof(int64));
	out->count = palloc(h.ng * sizeof(uint32));
	for (int m = 0; m < h.nm; m++)
	{
		out->meas[m] = h.meas[m];
		out->nn[m] = palloc(h.ng * sizeof(uint32));
		out->sum[m] = palloc(h.ng * sizeof(SiftCI128));
		out->min[m] = palloc(h.ng * sizeof(int64));
		out->max[m] = palloc(h.ng * sizeof(int64));
	}
	MemoryContextSwitchTo(old);
	for (uint32 i = 0; i < h.ng; i++)
	{
		KtGroup		g = {0};

		memcpy(&g, p + sizeof(h) + (Size) i * KT_REC(h.nm), KT_REC(h.nm));
		out->key[i] = g.key;
		out->count[i] = g.cnt;
		if (g.pad || (h.nullg && i == h.ng - 1 && g.key != 0))
			return 0;
		for (int m = 0; m < h.nm; m++)
		{
			if (g.m[m].pad)
				return 0;
			out->nn[m][i] = g.m[m].nn;
			out->sum[m][i] = (SiftCI128) (((unsigned __int128) g.m[m].sum[1] << 64) | g.m[m].sum[0]);
			out->min[m][i] = g.m[m].mn;
			out->max[m][i] = g.m[m].mx;
		}
		if (i > 0 && i + h.nullg < h.ng && g.key <= out->key[i - 1])
			return 0;
	}
	return size;
}
