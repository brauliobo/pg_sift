/*
 * build.c - segment construction for the SQLite table: an in-memory inverted buffer (the role of SiftInvBuf in
 * src/bm25/build.c, whose heap build, spill files and parallel workers do not apply), written as an
 * immutable segment through the unmodified segment writer, and deletion of one document (sift_seg_delete_key).
 * The merge of segments is in merge.c.
 */
#include "api.h"
#include "core/cmp.h"
#include "core/hash.h"

#define BATCH			1024

/* ---------- inverted buffer ---------- */

typedef struct InvPosting
{
	uint32		ord;				/* document, in arrival order */
	uint32		poff;				/* position record in the arena */
	uint32		plen;
	uint32		tf;
} InvPosting;

typedef struct InvTerm
{
	char	   *bytes;
	uint32		hash;
	uint16		len;
	bool		sorted;				/* postings in arrival order are in ascending ord */
	bool		haspos;
	uint32		n,
				cap;
	InvPosting *p;
} InvTerm;

struct SqInv
{
	bool		positions;
	SiftKey    *key;
	uint16	   *dl;
	uint8	   *dead;
	uint32		ndocs,
				dcap,
				ndead;
	bool		docs_sorted;		/* keys ascend in arrival order */
	uint32	   *dmap;				/* key -> ord + 1, open addressing: a removed doc keeps its slot until its key is added again or the table is rebuilt */
	uint32		dmask;
	uint32		dused;				/* slots of dmap in use */
	InvTerm   **slots;				/* open addressing by term hash */
	uint32		smask;
	InvTerm   **terms;
	uint32		nterms,
				tcap;
	uint8	   *pos;
	Size		poslen,
				poscap;
	uint64		npost;
};

SqInv *
sq_inv_new(bool positions)
{
	SqInv	   *inv = calloc(1, sizeof(SqInv));

	inv->positions = positions;
	inv->docs_sorted = true;
	return inv;
}

/* MEM[MEM-PORT-02]: SqInv write buffer (FLUSH_POSTINGS=64M -> 1.6-2.4GiB RSS; eager 36KB on RO) */
/* The tables are made by the first write: a connection that only reads holds none */
static void
inv_tables(SqInv *inv)
{
	if (inv->dmap)
		return;
	inv->dmask = 1023;
	inv->dused = 0;
	inv->dmap = calloc(inv->dmask + 1, sizeof(uint32));
	inv->smask = 4095;
	inv->slots = calloc(inv->smask + 1, sizeof(InvTerm *));
}

uint64
sq_inv_bytes(const SqInv *inv)
{
	return (uint64) inv->npost * sizeof(InvPosting) + inv->poscap + (uint64) inv->dcap * (sizeof(SiftKey) + 3) +
		(inv->dmap ? (uint64) (inv->dmask + 1) * sizeof(uint32) + (uint64) (inv->smask + 1) * sizeof(InvTerm *) : 0) +
		(uint64) inv->nterms * (sizeof(InvTerm) + 16);
}

void
sq_inv_clear(SqInv *inv)
{
	for (uint32 i = 0; i < inv->nterms; i++)
	{
		free(inv->terms[i]->bytes);
		free(inv->terms[i]->p);
		free(inv->terms[i]);
	}
	free(inv->key);
	free(inv->dl);
	free(inv->dead);
	free(inv->terms);
	free(inv->pos);
	free(inv->dmap);
	free(inv->slots);
	inv->key = NULL;
	inv->dl = NULL;
	inv->dead = NULL;
	inv->terms = NULL;
	inv->pos = NULL;
	inv->ndocs = inv->dcap = inv->ndead = inv->nterms = inv->tcap = 0;
	inv->poslen = inv->poscap = 0;
	inv->npost = 0;
	inv->docs_sorted = true;
	inv->dmap = NULL;
	inv->slots = NULL;
	inv->dmask = inv->smask = inv->dused = 0;
}

void
sq_inv_free(SqInv *inv)
{
	sq_inv_clear(inv);
	free(inv);
}

bool
sq_inv_empty(const SqInv *inv)
{
	return inv->ndocs == inv->ndead;
}

uint64
sq_inv_postings(const SqInv *inv)
{
	return inv->npost;
}

static inline uint32
key_hash(SiftKey k)
{
	return (uint32) (sift_fib64(k) >> 32);
}

/* Rebuilds the table of the live docs, twice as large only when they fill half of it: the slots of removed docs are not carried over */
static void
dmap_grow(SqInv *inv)
{
	uint32		mask = inv->dmask;
	uint32	   *m;

	if ((inv->ndocs - inv->ndead + 1) * 2 > mask)
		mask = mask * 2 + 1;
	m = calloc(mask + 1, sizeof(uint32));
	inv->dused = 0;
	for (uint32 ord = 0; ord < inv->ndocs; ord++)
	{
		uint32		h = key_hash(inv->key[ord]) & mask;

		if (inv->dead[ord])
			continue;
		while (m[h])
			h = (h + 1) & mask;
		m[h] = ord + 1;
		inv->dused++;
	}
	free(inv->dmap);
	inv->dmap = m;
	inv->dmask = mask;
}

static uint32
add_doc(SqInv *inv, SiftKey key, uint32 dl)
{
	uint32		ord = inv->ndocs,
				h;

	inv_tables(inv);
	if (ord == inv->dcap)
	{
		inv->dcap = inv->dcap ? inv->dcap * 2 : 4096;
		inv->key = realloc(inv->key, inv->dcap * sizeof(SiftKey));
		inv->dl = realloc(inv->dl, inv->dcap * sizeof(uint16));
		inv->dead = realloc(inv->dead, inv->dcap);
	}
	if (ord > 0 && key <= inv->key[ord - 1])
		inv->docs_sorted = false;
	inv->key[ord] = key;
	inv->dl[ord] = Min(dl, SIFT_MAX_DL);
	inv->dead[ord] = 0;
	if ((inv->dused + 1) * 2 > inv->dmask)
		dmap_grow(inv);
	inv->ndocs++;
	h = key_hash(key) & inv->dmask;
	while (inv->dmap[h])
	{
		uint32		eord = inv->dmap[h] - 1;

		if (inv->key[eord] == key && inv->dead[eord])
		{
			inv->dmap[h] = ord + 1;
			return ord;
		}
		h = (h + 1) & inv->dmask;
	}
	inv->dmap[h] = ord + 1;
	inv->dused++;
	return ord;
}

static int64
find_doc(const SqInv *inv, SiftKey key)
{
	if (inv->dmap == NULL)
		return -1;
	for (uint32 h = key_hash(key) & inv->dmask; inv->dmap[h]; h = (h + 1) & inv->dmask)
	{
		uint32		ord = inv->dmap[h] - 1;

		if (inv->key[ord] == key && !inv->dead[ord])
			return ord;
	}
	return -1;
}

bool
sq_inv_remove(SqInv *inv, SiftKey key)
{
	int64		ord = find_doc(inv, key);

	if (ord < 0)
		return false;
	inv->dead[ord] = 1;
	inv->ndead++;
	return true;
}

static InvTerm *
term_get(SqInv *inv, const char *bytes, int len, uint32 hash)
{
	uint32		h;
	InvTerm    *t;

	inv_tables(inv);
	h = hash & inv->smask;
	for (; (t = inv->slots[h]) != NULL; h = (h + 1) & inv->smask)
		if (t->hash == hash && t->len == len && memcmp(t->bytes, bytes, len) == 0)
			return t;
	t = calloc(1, sizeof(InvTerm));
	t->bytes = malloc(len + 1);
	memcpy(t->bytes, bytes, len);
	t->hash = hash;
	t->len = len;
	t->sorted = true;
	if (inv->nterms == inv->tcap)
	{
		inv->tcap = inv->tcap ? inv->tcap * 2 : 4096;
		inv->terms = realloc(inv->terms, inv->tcap * sizeof(InvTerm *));
	}
	inv->terms[inv->nterms++] = t;
	inv->slots[h] = t;
	if (inv->nterms * 2 > inv->smask)
	{
		uint32		mask = inv->smask * 2 + 1;
		InvTerm   **s = calloc(mask + 1, sizeof(InvTerm *));

		for (uint32 i = 0; i < inv->nterms; i++)
		{
			uint32		k = inv->terms[i]->hash & mask;

			while (s[k])
				k = (k + 1) & mask;
			s[k] = inv->terms[i];
		}
		free(inv->slots);
		inv->slots = s;
		inv->smask = mask;
	}
	return t;
}

static void
add_posting(SqInv *inv, const char *bytes, int len, uint32 hash, uint32 ord, uint32 tf, const uint8 *pos, uint32 plen)
{
	InvTerm    *t = term_get(inv, bytes, len, hash);
	InvPosting *p;

	if (t->n == t->cap)
	{
		t->cap = t->cap ? t->cap * 2 : 4;
		t->p = realloc(t->p, t->cap * sizeof(InvPosting));
	}
	if (t->n > 0 && ord <= t->p[t->n - 1].ord)
		t->sorted = false;
	p = &t->p[t->n++];
	p->ord = ord;
	p->tf = tf;
	p->poff = p->plen = 0;
	if (inv->positions && pos != NULL)
	{
		if (inv->poslen + plen > inv->poscap)
		{
			inv->poscap = Max(inv->poslen + plen, Max(16384, inv->poscap * 2));
			inv->pos = realloc(inv->pos, inv->poscap);
		}
		memcpy(inv->pos + inv->poslen, pos, plen);
		p->poff = inv->poslen;
		p->plen = plen;
		inv->poslen += plen;
		t->haspos = true;
	}
	inv->npost++;
}

void
sq_inv_add(SqInv *inv, SiftKey key, const SiftTermFreq *terms, int nterms, uint32 dl)
{
	uint32		ord = add_doc(inv, key, dl);

	for (int i = 0; i < nterms; i++)
		add_posting(inv, terms[i].term, terms[i].len, terms[i].hash, ord, terms[i].tf, terms[i].pos, terms[i].poslen);
}

/* ---------- write ---------- */

static int
term_cmp(const void *a, const void *b)
{
	const InvTerm *x = *(InvTerm *const *) a,
			   *y = *(InvTerm *const *) b;

	return sift_term_cmp(x->bytes, x->len, y->bytes, y->len);
}

typedef struct RankCtx
{
	const SqInv *inv;
	const uint32 *rank;
} RankCtx;

static _Thread_local RankCtx rank_ctx;

static int
posting_cmp(const void *a, const void *b)
{
	uint32		x = rank_ctx.rank[((const InvPosting *) a)->ord],
				y = rank_ctx.rank[((const InvPosting *) b)->ord];

	return (x > y) - (x < y);
}

static int
ord_key_cmp(const void *a, const void *b)
{
	SiftKey		x = rank_ctx.inv->key[*(const uint32 *) a],
				y = rank_ctx.inv->key[*(const uint32 *) b];

	return (x > y) - (x < y);
}

/* ords of the live documents in key order, and the rank of every ord (UINT32_MAX when dead) */
static uint32 *
live_order(const SqInv *inv, uint32 **rank_out, uint32 *nlive)
{
	uint32	   *order = malloc(Max(inv->ndocs, 1) * sizeof(uint32)),
			   *rank = malloc(Max(inv->ndocs, 1) * sizeof(uint32));
	uint32		n = 0;

	for (uint32 i = 0; i < inv->ndocs; i++)
		if (!inv->dead[i])
			order[n++] = i;
	if (!inv->docs_sorted)
	{
		rank_ctx.inv = inv;
		qsort(order, n, sizeof(uint32), ord_key_cmp);
	}
	for (uint32 i = 0; i < inv->ndocs; i++)
		rank[i] = UINT32_MAX;
	for (uint32 i = 0; i < n; i++)
		rank[order[i]] = i;
	*rank_out = rank;
	*nlive = n;
	return order;
}

typedef struct Batch
{
	SiftPostingWriter *pw;
	int			n;
	SiftKey		keys[BATCH];
	uint8		tf[BATCH];
	uint16		dl[BATCH];
	uint32		plen[BATCH];
	uint8	   *pos;
	Size		poslen,
				poscap;
	bool		haspos;
} Batch;

static void
batch_flush(Batch *b)
{
	if (b->n)
		sift_postw_add(b->pw, b->keys, b->tf, b->dl, b->n, b->haspos ? b->pos : NULL, b->plen);
	b->n = 0;
	b->poslen = 0;
}

static void
write_term(SqInv *inv, SiftSegmentWriter *sw, Batch *b, InvTerm *t, const uint32 *rank, const uint32 *order)
{
	InvPosting *p = t->p;
	uint32		n = t->n;

	b->n = 0;
	b->haspos = t->haspos;
	b->pw = sift_segw_begin_term(sw, t->bytes, t->len);
	/* MEM[MEM-PORT-03]: SqInv dmap + sq_inv_write sort copies (3x dmap grow; 2x flush peak) */
	if (!t->sorted || !inv->docs_sorted)
	{
		rank_ctx.rank = rank;
		qsort(p, n, sizeof(InvPosting), posting_cmp);
	}
	for (uint32 i = 0; i < n; i++)
	{
		uint32		ord = p[i].ord;

		if (inv->dead[ord])
			continue;
		b->keys[b->n] = inv->key[ord];
		b->tf[b->n] = Min(p[i].tf, SIFT_MAX_TF);
		b->dl[b->n] = inv->dl[ord];
		if (b->haspos)
		{
			if (b->poslen + p[i].plen > b->poscap)
			{
				b->poscap = Max(b->poslen + p[i].plen, Max(8192, b->poscap * 2));
				b->pos = realloc(b->pos, b->poscap);
			}
			memcpy(b->pos + b->poslen, inv->pos + p[i].poff, p[i].plen);
			b->plen[b->n] = p[i].plen;
			b->poslen += p[i].plen;
		}
		if (++b->n == BATCH)
			batch_flush(b);
	}
	batch_flush(b);
	sift_segw_end_term(sw);
	free(t->p);
	t->p = NULL;
}

static bool
term_live(const SqInv *inv, const InvTerm *t)
{
	for (uint32 i = 0; i < t->n; i++)
		if (!inv->dead[t->p[i].ord])
			return true;
	return false;
}

void
sq_inv_write(SqInv *inv, SqStore *st)
{
	const SqOptions *o = sq_store_options(st);
	SiftScoreRef ref = {(float4) o->base.k1, (float4) o->base.b, 0};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift segment write", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftWriteTarget *wt = sift_wt_begin(sq_store_rel(st), SIFT_WRITE_SHARED);
	SiftSegmentWriter *sw;
	SiftSegEntry entry;
	Batch	   *b = calloc(1, sizeof(Batch));
	uint32	   *rank,
			   *order,
				nlive;
	InvTerm   **sorted;
	uint32		nsorted = 0;

	if (sq_inv_empty(inv))
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		free(b);
		sq_inv_clear(inv);
		return;
	}
	order = live_order(inv, &rank, &nlive);
	sw = sift_segw_begin(wt, sq_store_new_gen(st), &ref, cx, inv->positions);
	for (uint32 i = 0; i < nlive; i += BATCH)
	{
		SiftKey		keys[BATCH];
		uint16		dl[BATCH];
		int			k = Min(BATCH, nlive - i);

		for (int j = 0; j < k; j++)
		{
			keys[j] = inv->key[order[i + j]];
			dl[j] = inv->dl[order[i + j]];
		}
		sift_segw_add_docs(sw, keys, dl, k);
	}
	sorted = malloc(Max(inv->nterms, 1) * sizeof(InvTerm *));
	for (uint32 i = 0; i < inv->nterms; i++)
		if (term_live(inv, inv->terms[i]))
			sorted[nsorted++] = inv->terms[i];
	qsort(sorted, nsorted, sizeof(InvTerm *), term_cmp);
	for (uint32 i = 0; i < nsorted; i++)
		write_term(inv, sw, b, sorted[i], rank, order);
	sift_segw_finish(sw, &entry);
	entry.reserved = sq_wt_first(wt);
	sift_wt_finish(wt);
	sq_store_publish(st, &entry);
	free(sorted);
	free(order);
	free(rank);
	free(b->pos);
	free(b);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	sq_inv_clear(inv);
}

/* ---------- delete ---------- */

bool
sq_delete_key(SqStore *st, SiftKey key)
{
	const SiftDirSnapshot *dir = sq_store_dir(st);
	uint32		sb = sift_block_sb(sift_key_block(key));

	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftSegEntry e = dir->segs[i];
		SiftSegment *seg;
		bool		deleted;

		if (sb < e.sb_lo || sb > e.sb_hi)
			continue;
		seg = sift_seg_open(sq_store_rel(st), &e, CurrentMemoryContext);
		deleted = sift_seg_delete_key(seg, key);
		sift_seg_close(seg);
		if (deleted)
			return true;
	}
	return false;
}
