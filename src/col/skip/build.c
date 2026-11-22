/*
 * col/skip/build.c - the skipping indexes of the rows of a part (col/skip_index.h): the exact summaries of every column that has few distinct values in a granule, then, while their bytes stay
 * within sift_aux_skip_budget of the part, the Bloom filters of the columns that have many.
 */
/* check-memory: allow-allocs every array here follows the columns, the granules or the predicates of one part, whose rows are bounded by the build buffer, not the rows of the table */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/skip/skip_int.h"
#include "core/cmp.h"
#include "core/popcount.h"
#include "core/sbbf.h"

typedef struct Stream
{
	uint16		col;
	uint8		kind;
	uint16		param;
	StringInfoData data;
	uint32	   *plen;
	uint8	   *flags;
} Stream;

typedef struct Builder
{
	int			ng;
	Stream	  **s;
	int			ns;
	int			cap;
	uint64		bytes;			/* of the payloads of the filters kept */
	uint64		budget;
	uint64	   *tab;			/* scratch: the distinct hashes of a granule */
	uint32	   *tris;			/* scratch: the distinct trigrams of a granule */
	uint32     *tri_tab;
	int        *sidx;           /* the stream of (col, kind) at col * (SIFT_SKIP_NKINDS + 1) + kind, -1: none yet */
} Builder;

#define TAB_N			16384
#define TRI_TAB_N		8192

static Stream *
stream_new(Builder *b, int col, SiftSkipKind kind, int param)
{
	Stream	   *s = palloc0(sizeof(Stream));

	s->col = col;
	s->kind = kind;
	s->param = param;
	initStringInfo(&s->data);
	s->plen = palloc0(b->ng * sizeof(uint32));
	s->flags = palloc(b->ng);
	memset(s->flags, SIFT_SKIP_GRAN_NONE, b->ng);
	return s;
}

static void
stream_keep(Builder *b, Stream *s)
{
	if (b->ns == b->cap)
	{
		b->cap = Max(16, b->cap * 2);
		b->s = b->s ? repalloc(b->s, b->cap * sizeof(Stream *)) : palloc(b->cap * sizeof(Stream *));
	}
	b->s[b->ns++] = s;
}

/* The stream of (col, kind) a cheap summary goes to: the first granule that needs one makes it */
static Stream *
stream_of(Builder *b, int col, SiftSkipKind kind, int param)
{
	int slot = col * (SIFT_SKIP_NKINDS + 1) + kind;

	if (b->sidx[slot] < 0)
	{
		stream_keep(b, stream_new(b, col, kind, param));
		b->sidx[slot] = b->ns - 1;
	}
	return b->s[b->sidx[slot]];
}

static void
put(Stream *s, int g, const void *payload, uint32 len)
{
	s->flags[g] = SIFT_SKIP_GRAN_PRESENT;
	s->plen[g] = len;
	appendBinaryStringInfo(&s->data, payload, len);
}

static bool
col_eligible(const SiftColIn *c)
{
	if (c->group1 > 0 || c->eoff != NULL)
		return false;
	if (c->kind == SIFT_CELL_INT)
		return c->hint == SIFT_CHINT_AUTO || c->hint == SIFT_CHINT_NUM64 || c->hint == SIFT_CHINT_BOOL;
	return c->kind == SIFT_CELL_STR && (c->hint == SIFT_CHINT_AUTO || c->hint == SIFT_CHINT_VIEW || c->hint == SIFT_CHINT_UUID);
}

/* The string of row r of a column that keeps its strings, or only the codes of the dictionary of its part */
static inline void
str_at(const SiftColIn *c, int r, const char **p, uint32 *len)
{
	if (c->s != NULL)
	{
		*p = c->s[r];
		*len = c->slen[r];
	}
	else
	{
		*p = c->pdict->p[c->dcode[r]];
		*len = c->pdict->len[c->dcode[r]];
	}
}

static inline bool
is_null(const SiftColIn *c, int r)
{
	return c->isnull != NULL && c->isnull[r];
}

static inline uint64
row_hash(const SiftColIn *c, int r)
{
	const char *p;
	uint32		len;

	if (c->kind == SIFT_CELL_INT)
		return sift_skip_hash_i64(c->i[r]);
	str_at(c, r, &p, &len);
	return sift_skip_hash_bytes(p, len);
}

/* The distinct hashes of the values of the rows into b->tab; their number, or more than cap when they exceed it (the table is then incomplete) */
static int
distinct_hashes(Builder *b, const SiftColIn *c, int n, int cap)
{
	int			nd = 0;

	memset(b->tab, 0, TAB_N * sizeof(uint64));
	for (int r = 0; r < n; r++)
	{
		uint64		h;
		uint32		at;

		if (is_null(c, r))
			continue;
		h = row_hash(c, r);
		for (at = (uint32) (h >> 40) & (TAB_N - 1); b->tab[at] != 0 && b->tab[at] != h; at = (at + 1) & (TAB_N - 1))
			;
		if (b->tab[at] == 0)
		{
			b->tab[at] = h;
			if (++nd > cap)
				return nd;
		}
	}
	return nd;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* The hashes of the table, ascending */
static uint32
table_sorted(Builder *b, uint64 *out)
{
	uint32		m = 0;

	for (uint32 i = 0; i < TAB_N; i++)
		if (b->tab[i] != 0)
		{
			SIFT_INVARIANT(m < TAB_N, "a table of distinct hashes lists each once");
			out[m++] = b->tab[i];
		}
	qsort(out, m, sizeof(uint64), cmp_u64);
	return m;
}

static void
put_exact_hashes(Stream *s, int g, const uint64 *h, uint32 nd)
{
	StringInfoData p;

	initStringInfo(&p);
	appendBinaryStringInfo(&p, (const char *) &nd, 4);
	appendStringInfoSpaces(&p, 4);
	appendBinaryStringInfo(&p, (const char *) h, (uint64) nd * 8);
	put(s, g, p.data, p.len);
	pfree(p.data);
}

/* The summaries that are exact; true when the granule has more distinct values than they hold (a filter may be built for it) */
static bool
int_granule(Builder *b, int col, int g, const SiftColIn *c, int n, bool *empty)
{
	int64		min = PG_INT64_MAX,
				max = PG_INT64_MIN;
	int			nn = 0;
	int64		vals[SIFT_SKIP_EXACT_MAX + 1];
	int			nd = 0;
	bool		few = true;

	for (int r = 0; r < n; r++)
		if (!is_null(c, r))
		{
			min = Min(min, c->i[r]);
			max = Max(max, c->i[r]);
			nn++;
		}
	if (nn == 0)
	{
		*empty = true;
		return false;
	}
	if ((uint64) max - (uint64) min < SIFT_SKIP_SPAN)
	{
		SiftSkipIntBitset64 bs = {.base = min};

		for (int r = 0; r < n; r++)
			if (!is_null(c, r))
				bs.bits |= UINT64CONST(1) << (c->i[r] - min);
		put(stream_of(b, col, SIFT_SKIP_INT_BITSET64, 0), g, &bs, sizeof(bs));
		return false;
	}
	for (int r = 0; r < n && few; r++)
	{
		int			k = 0;

		if (is_null(c, r))
			continue;
		while (k < nd && vals[k] != c->i[r])
			k++;
		if (k == nd)
		{
			few = nd < SIFT_SKIP_EXACT_MAX;
			if (few)
				vals[nd++] = c->i[r];
		}
	}
	if (few)
	{
		StringInfoData p;

		qsort(vals, nd, sizeof(int64), sift_cmp_i64);
		initStringInfo(&p);
		appendBinaryStringInfo(&p, (const char *) &(uint32) {nd}, 4);
		appendStringInfoSpaces(&p, 4);
		appendBinaryStringInfo(&p, (const char *) vals, (uint64) nd * 8);
		put(stream_of(b, col, SIFT_SKIP_EXACT_SET, 0), g, p.data, p.len);
		pfree(p.data);
	}
	return !few;
}

static bool
str_granule(Builder *b, int col, int g, const SiftColIn *c, int n, const SiftDictEnc *dict, bool *empty)
{
	int			nn = 0,
				nd;
	uint64		h[SIFT_SKIP_EXACT_MAX];

	for (int r = 0; r < n; r++)
		nn += !is_null(c, r);
	if (nn == 0)
	{
		*empty = true;
		return false;
	}
	if (dict != NULL && dict->n <= SIFT_SKIP_DICT_MAX)
	{
		uint32		nwords = (dict->n + 63) / 64;
		uint64		bits[SIFT_SKIP_DICT_MAX / 64] = {0};

		for (int r = 0; r < n; r++)
		{
			const char *p;
			uint32		len,
						code;

			if (is_null(c, r))
				continue;
			str_at(c, r, &p, &len);
			code = c->dcode != NULL ? c->dcode[r] : sift_dict_code(dict, p, len);
			bits[code >> 6] |= UINT64CONST(1) << (code & 63);
		}
		put(stream_of(b, col, SIFT_SKIP_DICT_BITSET, nwords), g, bits, nwords * 8);
		return false;
	}
	nd = distinct_hashes(b, c, n, SIFT_SKIP_EXACT_MAX);
	if (nd > SIFT_SKIP_EXACT_MAX)
		return true;
	table_sorted(b, h);
	put_exact_hashes(stream_of(b, col, SIFT_SKIP_EXACT_SET, SIFT_SKIP_EXACT_HASHES), g, h, nd);
	return false;
}

static void
put_unsaturated(Stream *s, int g, const uint8 *blocks, uint32 nb)
{
	uint64		set = 0,
				len = (uint64) nb * SIFT_SBBF_BLOCK_BYTES;

	for (uint64 i = 0; i < len; i += 8)
	{
		uint64		w;

		memcpy(&w, blocks + i, 8);
		set += sift_pc64(w);
	}
	if (set * 100 < len * 8 * SIFT_SKIP_SATURATED_PCT)
		put(s, g, blocks, len);
}

static void
bloom_granule(Builder *b, Stream *s, int g, const SiftColIn *c, int n)
{
	uint32		nd,
				nb;
	uint8	   *blocks;

	if (n < SIFT_SKIP_FILTER_MIN_ROWS)
		return;
	nd = distinct_hashes(b, c, n, TAB_N);
	nb = sift_sbbf_nblocks(nd, SIFT_SBBF_BPK);
	blocks = palloc0((uint64) nb * SIFT_SBBF_BLOCK_BYTES);
	for (uint32 i = 0; i < TAB_N; i++)
		if (b->tab[i] != 0)
			sift_sbbf_insert(blocks, nb, b->tab[i]);
	put_unsaturated(s, g, blocks, nb);
	pfree(blocks);
}

/* The distinct trigrams of the strings of a granule into b->tris; their number, or -1 when more than SIFT_TRIGRAM_SATURATE */
static int
distinct_trigrams(Builder *b, const SiftColIn *c, int n)
{
	int			nt = 0;

	memset(b->tri_tab, 0, TRI_TAB_N * sizeof(uint32));
	for (int r = 0; r < n; r++)
	{
		const char *p;
		uint32		len;

		if (is_null(c, r))
			continue;
		str_at(c, r, &p, &len);
		for (uint32 i = 0; i + 3 <= len; i++)
		{
			uint32		t = sift_skip_trigram(p + i) + 1,
						at = (t * UINT32_C(0x9E3779B1)) >> 19;

			while (b->tri_tab[at] != 0 && b->tri_tab[at] != t)
				at = (at + 1) & (TRI_TAB_N - 1);
			if (b->tri_tab[at] == 0)
			{
				if (nt == SIFT_TRIGRAM_SATURATE)
					return -1;
				b->tri_tab[at] = t;
				b->tris[nt++] = t - 1;
			}
		}
	}
	return nt;
}

static void
trigram_granule(Builder *b, Stream *s, int g, const SiftColIn *c, int n)
{
	int			nt;
	uint32		nb;
	uint8	   *blocks;

	if (n < SIFT_SKIP_FILTER_MIN_ROWS || (nt = distinct_trigrams(b, c, n)) < 0)
		return;
	nb = Min(SIFT_TRIGRAM_MAX_BLOCKS, sift_sbbf_nblocks(nt, SIFT_SBBF_BPK));
	blocks = palloc0((uint64) nb * SIFT_SBBF_BLOCK_BYTES);
	for (int i = 0; i < nt; i++)
		sift_sbbf_insert(blocks, nb, sift_skip_hash_trigram(b->tris[i]));
	put_unsaturated(s, g, blocks, nb);
	pfree(blocks);
}

typedef struct Candidate
{
	int			col;
	SiftSkipKind kind;
	bool	   *want;			/* the granules the cheap summaries left */
} Candidate;

/* A filter of a column for the granules in want: kept when the bytes of all the filters stay within the budget */
static void
filter_stream(Builder *b, const SiftRows *rows, const int *gstart, const Candidate *cd)
{
	Stream	   *s = stream_new(b, cd->col, cd->kind, SIFT_SBBF_BPK);

	for (int g = 0; g < b->ng; g++)
	{
		int			from = gstart[g],
					n = gstart[g + 1] - from;
		SiftColIn	slice;

		if (!cd->want[g])
			continue;
		slice = sift_colin_slice(&rows->cols[cd->col], from);
		if (cd->kind == SIFT_SKIP_BLOOM_SB)
			bloom_granule(b, s, g, &slice, n);
		else
			trigram_granule(b, s, g, &slice, n);
	}
	if (b->bytes + s->data.len <= b->budget && s->data.len > 0)
	{
		b->bytes += s->data.len;
		stream_keep(b, s);
	}
	else
	{
		pfree(s->data.data);
		pfree(s->plen);
		pfree(s->flags);
		pfree(s);
	}
}

static int
cmp_stream(const void *a, const void *b)
{
	const Stream *x = *(Stream *const *) a,
			   *y = *(Stream *const *) b;

	return x->col != y->col ? (x->col > y->col) - (x->col < y->col) : (x->kind > y->kind) - (x->kind < y->kind);
}

static void
write_section(Builder *b, const bool *const *empty, StringInfo out)
{
	SiftSkipIndexHeader h = {.magic = SIFT_SKIP_MAGIC,.version = SIFT_SKIP_VERSION,.nentries = b->ns,.ngranules = b->ng};
	uint32		at = sizeof(h) + b->ns * sizeof(SiftSkipColDesc);
	SiftSkipColDesc *desc = palloc0(b->ns * sizeof(SiftSkipColDesc));

	qsort(b->s, b->ns, sizeof(Stream *), cmp_stream);
	for (int i = 0; i < b->ns; i++)
	{
		Stream	   *s = b->s[i];
		uint32		meta = MAXALIGN((uint64) (b->ng + 1) * 4 + b->ng);

		for (int g = 0; g < b->ng; g++)
			if (empty[s->col] != NULL && empty[s->col][g])
				s->flags[g] = SIFT_SKIP_GRAN_EMPTY;
		desc[i] = (SiftSkipColDesc) {.col = s->col,.kind = s->kind,.param = s->param,.meta_off = at,.data_off = at + meta};
		at += meta + s->data.len;
	}
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(out, (char *) desc, b->ns * sizeof(SiftSkipColDesc));
	for (int i = 0; i < b->ns; i++)
	{
		Stream	   *s = b->s[i];
		uint32		off = 0;

		for (int g = 0; g <= b->ng; g++)
		{
			appendBinaryStringInfo(out, (char *) &off, 4);
			if (g < b->ng)
				off += s->flags[g] == SIFT_SKIP_GRAN_PRESENT ? s->plen[g] : 0;
		}
		appendBinaryStringInfo(out, (char *) s->flags, b->ng);
		while ((out->len & 7) != 0)
			appendStringInfoChar(out, '\0');
		appendBinaryStringInfo(out, s->data.data, s->data.len);
	}
}

/* The average length of the strings of a column, from the first rows of it */
static uint64
avg_len(const SiftColIn *col)
{
	uint64		bytes = 0,
				cnt = 0;

	for (int i = 0; i < 4096 && col->slen != NULL && cnt < 1024; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			bytes += col->slen[i];
			cnt++;
		}
	return cnt ? bytes / cnt : 0;
}

bool
sift_skip_build(const SiftRows *rows, const int *gstart, int ng, const SiftDictEnc *const *dicts, const SiftColPromo *promos, const SiftAuxBudget *aux, StringInfo out)
{
	Builder		b = {.ng = ng,.budget = sift_aux_skip_budget(aux)};
	bool	  **empty = palloc0(Max(rows->ncols, 1) * sizeof(bool *));
	Candidate  *cand = palloc(Max(rows->ncols, 1) * 2 * sizeof(Candidate));
	int			ncand = 0;
	int			lead = rows->nsort > 0 ? rows->sort_cols[0] : -1;

	if (b.budget == 0)
		return false;
	b.sidx = palloc(Max(rows->ncols, 1) * (SIFT_SKIP_NKINDS + 1) * sizeof(int));
	memset(b.sidx, 0xFF, Max(rows->ncols, 1) * (SIFT_SKIP_NKINDS + 1) * sizeof(int));
	b.tab = palloc(TAB_N * sizeof(uint64));
	b.tris = palloc((SIFT_TRIGRAM_SATURATE + 1) * sizeof(uint32));
	b.tri_tab = palloc(TRI_TAB_N * sizeof(uint32));
	for (int c = 0; c < rows->ncols; c++)
	{
		const SiftColIn *col = &rows->cols[c];
		bool	   *wide,
				   *text;
		bool small_dict;
		SiftColIn coded;

		if (!col_eligible(col))
			continue;
		wide = palloc0(ng);
		text = palloc0(ng);
		empty[c] = palloc0(ng);
		small_dict = dicts != NULL && dicts[c] != NULL && dicts[c]->n <= SIFT_SKIP_DICT_MAX;
		coded = *col;
		if (coded.dcode == NULL && coded.pdict == NULL && dicts != NULL && dicts[c] != NULL && dicts[c]->row_codes != NULL)
			coded.dcode = dicts[c]->row_codes;
		for (int g = 0; g < ng; g++)
		{
			SiftColIn slice = sift_colin_slice(&coded, gstart[g]);
			int			n = gstart[g + 1] - gstart[g];

			wide[g] = col->kind == SIFT_CELL_INT ? int_granule(&b, c, g, &slice, n, &empty[c][g]) :
				str_granule(&b, c, g, &slice, n, dicts != NULL ? dicts[c] : NULL, &empty[c][g]);
			text[g] = col->kind == SIFT_CELL_STR && !empty[c][g] && !small_dict;
		}
		if ((c != lead || col->kind == SIFT_CELL_STR) && sift_aux_bloom_ok(aux, dicts != NULL && dicts[c] != NULL ? dicts[c]->n : UINT64_MAX))
			cand[ncand++] = (Candidate) {c, SIFT_SKIP_BLOOM_SB, wide};
		if (col->kind == SIFT_CELL_STR && !small_dict && sift_aux_trigram_ok(aux, true, avg_len(col), promos != NULL && promos[c].kind != SIFT_PROMO_NONE))
			cand[ncand++] = (Candidate) {c, SIFT_SKIP_TRIGRAM_SB, text};
	}
	for (int pass = 0; pass < 3; pass++)
		for (int i = 0; i < ncand; i++)
		{
			bool		str = rows->cols[cand[i].col].kind == SIFT_CELL_STR;

			if (pass == 0 ? (str && cand[i].kind == SIFT_SKIP_BLOOM_SB) : pass == 1 ? (str && cand[i].kind == SIFT_SKIP_TRIGRAM_SB) : !str)
				filter_stream(&b, rows, gstart, &cand[i]);
		}
	pfree(b.sidx);
	if (b.ns == 0)
		return false;
	write_section(&b, (const bool *const *) empty, out);
	return true;
}
