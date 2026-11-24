/*
 * agg/v6/par/group_key.c - the key of the groups of agg/v6/par/group.h in a slot, a record and a row (agg/v6/par/group_key.h). Nothing here calls the server: the memory is the arena of a table.
 */
#include "postgres.h"

#include "agg/v6/par/group_key.h"

#define ROWS_HALF_SLOT_MAX	(UINT64CONST(1) << 31)

static inline uint32
pad8(uint32 len)
{
	return len == SIFT_STR_NULL ? 0 : (len + 7) & ~7u;
}

/* ---- the table ---- */

bool
pgk_layout(SiftCompact *proto, const SiftParGroupSpec *sp, uint64 rows_bound)
{
	const bool	alone = sp->nk == 1 && sp->strs == 1 && sp->nsum == 0;
	const bool	key1_u32 = sp->key1_u32 || (sp->nk == 2 && sift_par_group_is_str(sp, 1));

	*proto = (SiftCompact) {.kw = sp->nk,.strk = alone ? 0 : -1,.nsum = sp->nsum};
	return cmp_layout(proto, key1_u32 && rows_bound < ROWS_HALF_SLOT_MAX);
}

bool
sift_par_group_spec_holds(const SiftParGroupSpec *sp)
{
	SiftCompact proto;

	return sp->nk >= 1 && sp->nk <= SIFT_PAR_GROUP_MAX_KEYS && (sp->strs >> sp->nk) == 0 && sp->nsum <= SIFT_PAR_GROUP_MAX_SUM && sp->topk <= SIFT_PAR_GROUP_TOPK_MAX &&
		(sp->order <= sp->nsum || sp->order == SIFT_PAR_GROUP_BY_KEY) && pgk_layout(&proto, sp, 0);
}

void
pgk_slot_row(const SiftCompact *c, const SiftParGroupSpec *sp, const uint64 *s, SiftParGroupRow *r)
{
	*r = (SiftParGroupRow) {0};
	if (c->strm && s == c->null_group)
		r->str[0].len = SIFT_STR_NULL;
	else if (c->strm)
	{
		const char *p;

		sstr_slot_str(c, s, &p, &r->str[0].len);
		r->str[0].p = (const uint8 *) p;
	}
	else
		for (int j = 0; j < sp->nk; j++)
		{
			if (!sift_par_group_is_str(sp, j))
				r->key[j] = j == sp->nk - 1 ? s[j] & c->last_mask : s[j];
			else if ((uint32) s[j] == CMP_NO_ID)
				r->str[j].len = SIFT_STR_NULL;
			else
				r->str[j] = (SiftStrRef) {(const uint8 *) c->in.arena + c->in.str[(uint32) s[j]].off, c->in.str[(uint32) s[j]].len};
		}
	r->count = cmp_count(c, s);
	for (int m = 0; m < sp->nsum; m++)
		r->sum[m] = s[c->sum0 + m];
}

static inline uint32
text_h32(const SiftStrRef *t)
{
	return cmp_h32(cmp_str_hash((const char *) t->p, t->len));
}

/* The words of the key of the row, with the number of each text in the strings of the table: false when they have no room */
static bool
key_words(SiftCompact *c, const SiftParGroupSpec *sp, const SiftParGroupRow *r, const uint32 *h32, uint64 *w)
{
	for (int j = 0; j < sp->nk; j++)
	{
		const SiftStrRef *t = &r->str[j];

		if (!sift_par_group_is_str(sp, j))
			w[j] = r->key[j];
		else if (t->len == SIFT_STR_NULL)
			w[j] = CMP_NO_ID;
		else if ((w[j] = cmp_intern_h(c, h32 != NULL ? h32[j] : text_h32(t), (const char *) t->p, t->len)) == CMP_NO_ID)
			return false;
	}
	return true;
}

/* The slot of the key of the row, made when the key is new (the table has room for it); NULL when the arena has no room */
static uint64 *
slot_of(SiftCompact *c, const SiftParGroupSpec *sp, const SiftParGroupRow *r, const uint32 *h32)
{
	const SiftStrRef *t = &r->str[0];
	uint64		w[SIFT_PAR_GROUP_MAX_KEYS];

	if (c->strm && t->len == SIFT_STR_NULL)
		return c->null_group;
	if (!cmp_reserve(c, 1, UINT64_MAX))
		return NULL;
	if (c->strm)
		return sstr_upsert(c, h32 != NULL ? h32[0] : text_h32(t), (const char *) t->p, t->len);
	if (!key_words(c, sp, r, h32, w))
		return NULL;
	return cmp_find(c, cmp_key_hash(c, w), w);
}

bool
pgk_add(SiftCompact *c, const SiftParGroupSpec *sp, const SiftParGroupRow *r, const uint32 *h32)
{
	uint64	   *s = slot_of(c, sp, r, h32);

	if (s == NULL)
		return false;
	cmp_fold(c, s, r->count, r->sum);
	return true;
}

/* ---- the records ---- */

uint32
pgk_word_rec_size(const SiftParGroupSpec *sp, bool partial)
{
	const uint32 n = sp->nk + sp->nsum;

	return 8 * (partial ? n + 1 : n <= 1 ? 1 : cmp_slot_words(n));
}

uint32
pgk_rec_size(const SiftParGroupSpec *sp, bool partial, const SiftParGroupRow *r)
{
	uint32		size = 8 * (sp->nk + sp->nsum + partial);

	if (!sp->strs)
		return pgk_word_rec_size(sp, partial);
	for (int j = 0; j < sp->nk; j++)
		size += sift_par_group_is_str(sp, j) ? pad8(r->str[j].len) : 0;
	return size;
}

uint64
pgk_batch_bytes(const SiftParGroupSpec *sp, const SiftParGroupBatch *b, Size *strbytes)
{
	uint64		bytes = (uint64) b->n * (sp->strs ? 8 * (sp->nk + sp->nsum) : pgk_word_rec_size(sp, false));

	*strbytes = 0;
	for (int j = 0; j < sp->nk; j++)
		for (uint32 i = 0; sift_par_group_is_str(sp, j) && i < b->n; i++)
		{
			bytes += pad8(b->str[j][i].len);
			*strbytes += b->str[j][i].len == SIFT_STR_NULL ? 0 : b->str[j][i].len;
		}
	return bytes;
}

/* The hash of the key of the row, which is its partition, and in h32 that of each of its texts */
static uint64
row_hash(const SiftCompact *c, const SiftParGroupSpec *sp, const SiftParGroupRow *r, uint32 *h32)
{
	uint64		h = 0;

	if (!sp->strs)
		return cmp_key_hash(c, r->key);
	for (int j = 0; j < sp->nk; j++)
	{
		const bool	text = sift_par_group_is_str(sp, j);
		const uint64 x = !text ? r->key[j] : r->str[j].len == SIFT_STR_NULL ? 0 : cmp_str_hash((const char *) r->str[j].p, r->str[j].len);

		h32[j] = cmp_h32(x);
		h = sp->nk == 1 ? x : sift_aggk6_hash1(x ^ h);
	}
	return h;
}

static bool
put_words(SiftRadixOut *o, uint32 part, const SiftParGroupSpec *sp, bool partial, const SiftParGroupRow *r)
{
	const uint32 words = pgk_word_rec_size(sp, partial) / 8;
	uint64	   *w = sift_radix_reserve(o, part, 8 * words);
	uint32		i = sp->nk;

	if (w == NULL)
		return false;
	memcpy(w, r->key, sp->nk * sizeof(uint64));
	if (partial)
		w[i++] = r->count;
	for (uint32 m = 0; m < sp->nsum; m++)
		w[i++] = r->sum[m];
	for (; i < words; i++)
		w[i] = 0;
	return true;
}

static bool
put_mixed(SiftRadixOut *o, uint32 part, const SiftParGroupSpec *sp, bool partial, const SiftParGroupRow *r, const uint32 *h32)
{
	uint8	   *b = sift_radix_reserve(o, part, pgk_rec_size(sp, partial, r));
	uint8	   *q = b;

	if (b == NULL)
		return false;
	for (int j = 0; j < sp->nk; j++)
	{
		const SiftStrRef *t = &r->str[j];
		const uint32 body = pad8(t->len);
		uint64		hdr = r->key[j];

		if (sift_par_group_is_str(sp, j))
			hdr = ((uint64) t->len << 32) | h32[j];
		memcpy(q, &hdr, 8);
		q += 8;
		if (sift_par_group_is_str(sp, j) && body > 0)
		{
			memcpy(q, t->p, t->len);
			memset(q + t->len, 0, body - t->len);
			q += body;
		}
	}
	if (partial)
	{
		memcpy(q, &r->count, 8);
		q += 8;
	}
	memcpy(q, r->sum, sp->nsum * sizeof(uint64));
	return true;
}

bool
pgk_put(const SiftCompact *c, const SiftParGroupSpec *sp, SiftRadixOut *o, bool partial, const SiftParGroupRow *r)
{
	uint32		h32[SIFT_PAR_GROUP_MAX_KEYS];
	const uint32 part = sift_radix_part(row_hash(c, sp, r, h32));

	return sp->strs ? put_mixed(o, part, sp, partial, r, h32) : put_words(o, part, sp, partial, r);
}

uint32
pgk_parse(const SiftParGroupSpec *sp, bool partial, const uint8 *b, SiftParGroupRow *r, uint32 *h32)
{
	const uint8 *q = b;

	for (int j = 0; j < sp->nk; j++)
	{
		uint64		x;

		memcpy(&x, q, 8);
		q += 8;
		if (!sift_par_group_is_str(sp, j))
			r->key[j] = x;
		else
		{
			r->str[j] = (SiftStrRef) {q, (uint32) (x >> 32)};
			h32[j] = (uint32) x;
			q += pad8(r->str[j].len);
		}
	}
	r->count = 1;
	if (partial)
		memcpy(&r->count, q, 8);
	memcpy(r->sum, q + 8 * partial, sp->nsum * sizeof(uint64));
	return pgk_rec_size(sp, partial, r);
}

/* ---- the order ---- */

int
pgk_cmp(const SiftParGroupSpec *sp, const SiftParGroupRow *a, const SiftParGroupRow *b)
{
	for (int j = 0; j < sp->nk; j++)
	{
		const SiftStrRef *x = &a->str[j],
				   *y = &b->str[j];
		int			c;

		if (!sift_par_group_is_str(sp, j))
			c = a->key[j] == b->key[j] ? 0 : a->key[j] < b->key[j] ? -1 : 1;
		else if (x->len == SIFT_STR_NULL || y->len == SIFT_STR_NULL)
			c = x->len == y->len ? 0 : x->len == SIFT_STR_NULL ? -1 : 1;
		else if ((c = memcmp(x->p, y->p, Min(x->len, y->len))) == 0)
			c = x->len == y->len ? 0 : x->len < y->len ? -1 : 1;
		if (c != 0)
			return c;
	}
	return 0;
}
