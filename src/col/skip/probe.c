/*
 * col/skip/probe.c - what the skipping indexes of a part say of a predicate (col/skip_index.h). A predicate is made executable once for the part: the keys it names are hashed, the strings of the
 * dictionary of the part it selects are found, the windows of its pattern are hashed. A granule is then probed with the summaries its column has, the exact ones first, and one that holds no row
 * that can pass is never read. A summary says "may match" for a granule it cannot decide, and "prune" only for one that it proves holds no row: no row is lost.
 */
/* check-memory: allow-allocs every array here follows the columns, the granules or the predicates of one part, whose rows are bounded by the build buffer, not the rows of the table */
#include "postgres.h"

#include "col/f2/f2_pred.h"
#include "col/granule/granule_filter.h"
#include "col/skip/skip_int.h"
#include "core/cmp.h"
#include "core/page_io.h"

typedef struct EntryMeta
{
	uint32	   *off;			/* the end of the payload of each granule, from the start of the data: ngranules + 1 entries */
	uint8	   *flags;
} EntryMeta;

struct SiftSkipReader
{
	MemoryContext cx;			/* what the reader keeps between granules lives here, whatever context a probe runs in */
	uint64		sec_off;
	uint64		sec_len;
	SiftSkipIndexHeader h;
	SiftSkipColDesc *desc;
	EntryMeta  *meta;
	uint8	   *buf;
	uint64		buf_cap;
};

#define MAX_TRIGRAMS	32

struct SiftSkipQuery
{
	const SiftColPred *p;
	bool		is_int;
	int			ent[SIFT_SKIP_NKINDS + 1];	/* the entry of each kind that can say something of the predicate, -1 for none */
	uint64	   *hashes;			/* the keys of an equality or IN, hashed as the writer hashed them */
	uint32		nhashes;
	uint64		tri[MAX_TRIGRAMS];
	uint32		ntri;
	SiftDictPredCache *dict;
};

SiftSkipReader *
sift_skip_open(Relation rel, const SiftPart *part)
{
	const SiftSecDirEntry *sec = sift_part_section(part, SEC_SKIP_INDEX);
	SiftSkipReader *r;
	uint32		ng = part->hdr.ngranules;

	if (sec == NULL)
		return NULL;
	r = palloc0(sizeof(SiftSkipReader));
	r->cx = CurrentMemoryContext;
	r->sec_off = sec->off;
	r->sec_len = sec->len;
	if (sec->len < sizeof(r->h))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The skipping indexes of the part are shorter than their header.");
	sift_part_read_bytes(rel, part, sec->off, sizeof(r->h), &r->h);
	if (r->h.magic != SIFT_SKIP_MAGIC || r->h.version != SIFT_SKIP_VERSION || r->h.ngranules != ng ||
		sec->len < sizeof(r->h) + (uint64) r->h.nentries * sizeof(SiftSkipColDesc))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The skipping indexes of the part do not describe its granules.");
	r->desc = palloc(Max(r->h.nentries, 1) * sizeof(SiftSkipColDesc));
	r->meta = palloc0(Max(r->h.nentries, 1) * sizeof(EntryMeta));
	sift_part_read_bytes(rel, part, sec->off + sizeof(r->h), (uint64) r->h.nentries * sizeof(SiftSkipColDesc), r->desc);
	for (uint32 i = 0; i < r->h.nentries; i++)
	{
		const SiftSkipColDesc *d = &r->desc[i];

		if (d->col >= part->hdr.ncols || d->kind < 1 || d->kind > SIFT_SKIP_NKINDS || (uint64) d->meta_off + MAXALIGN((uint64) (ng + 1) * 4 + ng) > d->data_off || d->data_off > sec->len)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "An entry of the skipping indexes of the part lies outside it.");
	}
	return r;
}

static int
find_entry(const SiftSkipReader *r, int col, SiftSkipKind kind)
{
	for (uint32 i = 0; i < r->h.nentries; i++)
		if (r->desc[i].col == col && r->desc[i].kind == kind)
			return (int) i;
	return -1;
}

/* The distinct hashes of the three byte windows of s that a trigram filter is asked about; windows of ASCII only when the match folds case */
static uint32
windows(const char *s, uint32 len, bool ascii_only, uint64 *out)
{
	uint32		n = 0;

	for (uint32 i = 0; i + 3 <= len && n < MAX_TRIGRAMS; i++)
	{
		uint64		h;
		bool		dup = false;

		if (ascii_only && (((uint8) s[i] | (uint8) s[i + 1] | (uint8) s[i + 2]) & 0x80))
			continue;
		h = sift_skip_hash_trigram(sift_skip_trigram(s + i));
		for (uint32 j = 0; j < n && !dup; j++)
			dup = out[j] == h;
		if (!dup)
			out[n++] = h;
	}
	return n;
}

static bool
usable(const SiftColDef *d)
{
	return d->parent < 0 && d->lane == 0 && !sift_col_def_is_wide(d) && !(d->flags & SIFT_COLD_LOSSY) && d->type != SIFT_COLT_VARLENA && d->type != SIFT_COLT_TIMETZ;
}

/* The folded windows of an ICONTAINS pattern are tested whatever the leaf; the windows of the other texts only where the bytes of the leaf decide the predicate */
static bool
trigram_applies(const SiftColDef *def, const SiftColPred *p)
{
	if (p->op == SIFT_COLOP_ICONTAINS)
		return p->nvals == 1 && !sift_f2_collation_undecided(p);
	return (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_PREFIX || p->op == SIFT_COLOP_CONTAINS || p->op == SIFT_COLOP_SUFFIX) && !sift_f2_undecided(def, p);
}

SiftSkipQuery *
sift_skip_query(const SiftSkipReader *r, const SiftColDef *def, const SiftColPred *p, const SiftPartDict *dict)
{
	SiftSkipQuery *q;
	bool		any = false;
	bool		point = p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN;

	if (r == NULL || !usable(def) || p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL || p->op == SIFT_COLOP_SHAPE || p->op == SIFT_COLOP_KEY_EXISTS || p->op == SIFT_COLOP_JSON_TYPE)
		return NULL;
	q = palloc0(sizeof(SiftSkipQuery));
	q->p = p;
	for (int k = 0; k <= SIFT_SKIP_NKINDS; k++)
		q->ent[k] = -1;
	if (def->store == SIFT_COLS_INT)
	{
		if (p->op < SIFT_COLOP_EQ || p->op > SIFT_COLOP_NOTIN)
			return NULL;
		q->is_int = true;
		q->ent[SIFT_SKIP_INT_BITSET64] = find_entry(r, p->col, SIFT_SKIP_INT_BITSET64);
		q->ent[SIFT_SKIP_EXACT_SET] = find_entry(r, p->col, SIFT_SKIP_EXACT_SET);
		if (point)
		{
			q->ent[SIFT_SKIP_BLOOM_SB] = find_entry(r, p->col, SIFT_SKIP_BLOOM_SB);
			q->hashes = palloc(Max(p->nvals, 1) * sizeof(uint64));
			for (int j = 0; j < p->nvals; j++)
				q->hashes[q->nhashes++] = sift_skip_hash_i64(p->vals[j].v.i);
		}
	}
	else if (def->store == SIFT_COLS_STR)
	{
		const SiftColValue *v = &p->vals[0];

		if (dict != NULL && p->op != SIFT_COLOP_ICONTAINS && !SIFT_COLOP_IS_EXT(p->op) && !sift_f2_undecided(def, p))
		{
			q->ent[SIFT_SKIP_DICT_BITSET] = find_entry(r, p->col, SIFT_SKIP_DICT_BITSET);
			if (q->ent[SIFT_SKIP_DICT_BITSET] >= 0)
				q->dict = sift_dict_pred_prepare(p, dict->n, dict->off, dict->bytes);
		}
		if (point && !sift_f2_undecided(def, p))
		{
			q->ent[SIFT_SKIP_EXACT_SET] = find_entry(r, p->col, SIFT_SKIP_EXACT_SET);
			q->ent[SIFT_SKIP_BLOOM_SB] = find_entry(r, p->col, SIFT_SKIP_BLOOM_SB);
			q->hashes = palloc(Max(p->nvals, 1) * sizeof(uint64));
			for (int j = 0; j < p->nvals; j++)
				q->hashes[q->nhashes++] = sift_skip_hash_bytes(p->vals[j].v.s, p->vals[j].len);
		}
		if (trigram_applies(def, p))
			q->ent[SIFT_SKIP_TRIGRAM_SB] = find_entry(r, p->col, SIFT_SKIP_TRIGRAM_SB);
		if (q->ent[SIFT_SKIP_TRIGRAM_SB] >= 0)
		{
			q->ntri = windows(v->v.s, v->len, p->op == SIFT_COLOP_ICONTAINS, q->tri);
			if (q->ntri == 0)
				q->ent[SIFT_SKIP_TRIGRAM_SB] = -1;
		}
	}
	for (int k = 1; k <= SIFT_SKIP_NKINDS; k++)
		any |= q->ent[k] >= 0;
	return any ? q : NULL;
}

static const EntryMeta *
load_meta(Relation rel, const SiftPart *part, SiftSkipReader *r, int e)
{
	EntryMeta  *m = &r->meta[e];
	uint32		ng = r->h.ngranules;
	uint64		at = r->sec_off + r->desc[e].meta_off;

	if (m->off == NULL)
	{
		m->off = MemoryContextAlloc(r->cx, (ng + 1) * sizeof(uint32));
		m->flags = MemoryContextAlloc(r->cx, Max(ng, 1));
		sift_part_read_bytes(rel, part, at, (uint64) (ng + 1) * sizeof(uint32), m->off);
		sift_part_read_bytes(rel, part, at + (uint64) (ng + 1) * sizeof(uint32), ng, m->flags);
	}
	return m;
}

static bool
int_holds(const SiftColPred *p, int64 v)
{
	bool		in = false;

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return v == p->vals[0].v.i;
		case SIFT_COLOP_NE:
			return v != p->vals[0].v.i;
		case SIFT_COLOP_LT:
			return v < p->vals[0].v.i;
		case SIFT_COLOP_LE:
			return v <= p->vals[0].v.i;
		case SIFT_COLOP_GT:
			return v > p->vals[0].v.i;
		case SIFT_COLOP_GE:
			return v >= p->vals[0].v.i;
		default:
			for (int j = 0; j < p->nvals && !in; j++)
				in = v == p->vals[j].v.i;
			return in == (p->op == SIFT_COLOP_IN);
	}
}

/* Whether any, and whether every one, of the values of an exact summary passes */
typedef struct Held
{
	bool		any;
	bool		all;
} Held;

static void
held(Held *h, const SiftColPred *p, int64 v)
{
	bool		yes = int_holds(p, v);

	h->any |= yes;
	h->all &= yes;
}

static SiftSkipVerdict
verdict_of(Held h, bool no_nulls)
{
	return !h.any ? SIFT_SKIP_PRUNE : (h.all && no_nulls ? SIFT_SKIP_ALL_MATCH : SIFT_SKIP_MAY_MATCH);
}

static bool
hash_in(const uint64 *sorted, uint32 nd, uint64 h)
{
	return bsearch(&h, sorted, nd, sizeof(uint64), sift_cmp_u64) != NULL;
}

static SiftSkipVerdict
judge(const SiftSkipQuery *q, const SiftSkipColDesc *d, const uint8 *pay, uint32 len, bool no_nulls)
{
	Held		h = {false, true};

	switch (d->kind)
	{
		case SIFT_SKIP_INT_BITSET64:
			{
				SiftSkipIntBitset64 bs;

				if (len != sizeof(bs))
					return SIFT_SKIP_MAY_MATCH;
				memcpy(&bs, pay, sizeof(bs));
				for (int b = 0; b < SIFT_SKIP_SPAN; b++)
					if ((bs.bits >> b) & 1)
						held(&h, q->p, (int64) ((uint64) bs.base + b));
				return verdict_of(h, no_nulls);
			}
		case SIFT_SKIP_EXACT_SET:
			{
				uint32		nd;

				if (len < 8)
					return SIFT_SKIP_MAY_MATCH;
				memcpy(&nd, pay, 4);
				if ((uint64) len != 8 + (uint64) nd * 8)
					return SIFT_SKIP_MAY_MATCH;
				if (q->is_int)
				{
					for (uint32 i = 0; i < nd; i++)
					{
						int64		v;

						memcpy(&v, pay + 8 + (uint64) i * 8, 8);
						held(&h, q->p, v);
					}
					return verdict_of(h, no_nulls);
				}
				for (uint32 j = 0; j < q->nhashes; j++)
					if (hash_in((const uint64 *) (pay + 8), nd, q->hashes[j]))
						return SIFT_SKIP_MAY_MATCH;
				return SIFT_SKIP_PRUNE;
			}
		case SIFT_SKIP_DICT_BITSET:
			{
				uint64		w,
							any = 0,
							rest = 0;

				if (len != (uint64) d->param * 8 || q->dict == NULL)
					return SIFT_SKIP_MAY_MATCH;
				for (uint32 i = 0; i < d->param; i++)
				{
					memcpy(&w, pay + (uint64) i * 8, 8);
					any |= w & (i < q->dict->nwords64 ? q->dict->bits[i] : 0);
					rest |= w & ~(i < q->dict->nwords64 ? q->dict->bits[i] : 0);
				}
				return any == 0 ? SIFT_SKIP_PRUNE : (rest == 0 && no_nulls ? SIFT_SKIP_ALL_MATCH : SIFT_SKIP_MAY_MATCH);
			}
		case SIFT_SKIP_BLOOM_SB:
		case SIFT_SKIP_TRIGRAM_SB:
			if (len == 0 || len % SIFT_SBBF_BLOCK_BYTES != 0)
				return SIFT_SKIP_MAY_MATCH;
			if (d->kind == SIFT_SKIP_BLOOM_SB)
				return sift_sbbf_probe_any(pay, len / SIFT_SBBF_BLOCK_BYTES, q->hashes, q->nhashes) ? SIFT_SKIP_MAY_MATCH : SIFT_SKIP_PRUNE;
			return sift_sbbf_probe_all(pay, len / SIFT_SBBF_BLOCK_BYTES, q->tri, q->ntri) ? SIFT_SKIP_MAY_MATCH : SIFT_SKIP_PRUNE;
		default:
			return SIFT_SKIP_MAY_MATCH;
	}
}

SiftSkipVerdict
sift_skip_probe(Relation rel, const SiftPart *part, SiftSkipReader *r, const SiftSkipQuery *q, uint32 g, bool no_nulls)
{
	SiftSkipVerdict out = SIFT_SKIP_MAY_MATCH;

	for (int k = 1; k <= SIFT_SKIP_NKINDS; k++)
	{
		int			e = q->ent[k];
		const EntryMeta *m;
		const SiftSkipColDesc *d;
		uint32		len;
		SiftSkipVerdict v;

		if (e < 0)
			continue;
		d = &r->desc[e];
		m = load_meta(rel, part, r, e);
		if (m->flags[g] == SIFT_SKIP_GRAN_EMPTY)
			return SIFT_SKIP_PRUNE;
		if (m->flags[g] != SIFT_SKIP_GRAN_PRESENT)
			continue;
		if (m->off[g + 1] < m->off[g] || (uint64) d->data_off + m->off[g + 1] > r->sec_len)
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "A payload of the skipping indexes of the part lies outside it.");
		len = m->off[g + 1] - m->off[g];
		if (len > r->buf_cap)
		{
			r->buf = r->buf ? repalloc(r->buf, len) : MemoryContextAlloc(r->cx, len);
			r->buf_cap = len;
		}
		sift_part_read_bytes(rel, part, r->sec_off + d->data_off + m->off[g], len, r->buf);
		v = judge(q, d, r->buf, len, no_nulls);
		if (v == SIFT_SKIP_PRUNE)
			return v;
		if (v == SIFT_SKIP_ALL_MATCH)
			out = v;
	}
	return out;
}
