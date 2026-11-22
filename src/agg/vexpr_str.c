/*
 * agg/vexpr_str.c - the text and calendar kernels of agg/vexpr_str.h.
 */
#include "core/host.h"

#include "agg/vexpr_str.h"
#include "core/hash.h"

/* ---- the arena of the strings of a batch ---- */

static SiftVstrBlock *
block_new(SiftVstrArena *ar, uint32 cap)
{
	SiftVstrBlock *b = MemoryContextAlloc(ar->cx, offsetof(SiftVstrBlock, data) + cap);

	b->next = NULL;
	b->cap = cap;
	b->used = 0;
	return b;
}

void
sift_vstr_arena_init(SiftVstrArena *ar, MemoryContext cx)
{
	ar->cx = cx;
	ar->head = NULL;
}

char *
sift_vstr_arena_alloc(SiftVstrArena *ar, uint32 len)
{
	SiftVstrBlock *b = ar->head;
	char	   *p;

	if (b == NULL || b->cap - b->used < len)
	{
		b = block_new(ar, Max(SIFT_VSTR_ARENA_BLOCK, len));
		b->next = ar->head;
		ar->head = b;
	}
	p = b->data + b->used;
	b->used += len;
	return p;
}

void
sift_vstr_arena_reset(SiftVstrArena *ar)
{
	SiftVstrBlock *keep = NULL;

	for (SiftVstrBlock *b = ar->head, *next; b != NULL; b = next)
	{
		next = b->next;
		if (keep == NULL && b->cap == SIFT_VSTR_ARENA_BLOCK)
			keep = b;
		else
			pfree(b);
	}
	if (keep != NULL)
	{
		keep->next = NULL;
		keep->used = 0;
	}
	ar->head = keep;
}

void
sift_vstr_arena_free(SiftVstrArena *ar)
{
	for (SiftVstrBlock *b = ar->head, *next; b != NULL; b = next)
	{
		next = b->next;
		pfree(b);
	}
	ar->head = NULL;
}

/* ---- bytes ---- */

bool
sift_vstr_is_ascii(const char *p, uint32 len)
{
	uint32		i = 0;
	uint64		acc = 0;

	for (; i + 8 <= len; i += 8)
	{
		uint64		w;

		memcpy(&w, p + i, 8);
		acc |= w;
	}
	for (; i < len; i++)
		acc |= (uint8) p[i];
	return (acc & UINT64CONST(0x8080808080808080)) == 0;
}

bool
sift_vstr_lower_ascii(char *dst, const char *src, uint32 len)
{
	if (!sift_vstr_is_ascii(src, len))
		return false;
	for (uint32 i = 0; i < len; i++)
		dst[i] = (char) (src[i] + ((uint8) (src[i] - 'A') < 26 ? 32 : 0));
	return true;
}

static void
lower_bytes(char *dst, const char *src, uint32 len)
{
	for (uint32 i = 0; i < len; i++)
		dst[i] = (char) ((uint8) (src[i] - 'A') < 26 ? src[i] + 32 : src[i]);
}

int
sift_vstr_cmp_bytes(const char *a, uint32 alen, const char *b, uint32 blen)
{
	const int	c = memcmp(a, b, Min(alen, blen));

	return c != 0 ? (c < 0 ? -1 : 1) : (alen < blen ? -1 : alen > blen);
}

static const char *
find_bytes(const char *s, const char *end, const char *d, uint32 dlen)
{
	for (; (uint32) (end - s) >= dlen; s++)
	{
		s = memchr(s, d[0], (size_t) (end - s) - dlen + 1);
		if (s == NULL)
			return NULL;
		if (memcmp(s, d, dlen) == 0)
			return s;
	}
	return NULL;
}

int
sift_vstr_split_part(const char *s, uint32 len, const char *d, uint32 dlen, int32 field, const char **out, uint32 *olen)
{
	const char *end = s + len,
			   *start = s,
			   *at;
	int64		nth = field;

	if (field == 0)
		return -1;
	*out = s;
	*olen = 0;
	if (len == 0)
		return 0;
	if (dlen == 0)
	{
		if (field == 1 || field == -1)
			*olen = len;
		return 0;
	}
	if (field < 0)
	{
		int64		fields = 1;

		for (at = find_bytes(s, end, d, dlen); at != NULL; at = find_bytes(at + dlen, end, d, dlen))
			fields++;
		nth = fields + field + 1;
		if (nth <= 0)
			return 0;
	}
	for (int64 k = 1; k < nth; k++)
	{
		at = find_bytes(start, end, d, dlen);
		if (at == NULL)
			return 0;
		start = at + dlen;
	}
	at = find_bytes(start, end, d, dlen);
	*out = start;
	*olen = (uint32) ((at != NULL ? at : end) - start);
	return 0;
}

bool
sift_vstr_trim_set(bool *set, const char *chars, uint32 n)
{
	memset(set, 0, 256);
	for (uint32 i = 0; i < n; i++)
	{
		if ((uint8) chars[i] >= 0x80)
			return false;
		set[(uint8) chars[i]] = true;
	}
	return true;
}

void
sift_vstr_btrim(const char *s, uint32 len, const bool *set, const char **out, uint32 *olen)
{
	uint32		lo = 0,
				hi = len;

	while (lo < hi && set[(uint8) s[lo]])
		lo++;
	while (hi > lo && set[(uint8) s[hi - 1]])
		hi--;
	*out = s + lo;
	*olen = hi - lo;
}

/* ---- the pipeline ---- */

void
sift_vstr_step_lower(SiftVstrPipe *p)
{
	Assert(p->nsteps < SIFT_VSTR_MAX_STEPS);
	p->step[p->nsteps++] = (SiftVstrStep) {.op = SVS_LOWER};
}

bool
sift_vstr_step_btrim(SiftVstrPipe *p, const char *chars, uint32 n)
{
	SiftVstrStep *s = &p->step[p->nsteps];

	Assert(p->nsteps < SIFT_VSTR_MAX_STEPS);
	*s = (SiftVstrStep) {.op = SVS_BTRIM};
	if (!sift_vstr_trim_set(s->trim, chars, n))
		return false;
	p->nsteps++;
	return true;
}

bool
sift_vstr_step_split_part(SiftVstrPipe *p, const char *delim, uint32 dlen, int32 field)
{
	Assert(p->nsteps < SIFT_VSTR_MAX_STEPS);
	if (field == 0)
		return false;
	p->step[p->nsteps++] = (SiftVstrStep) {.op = SVS_SPLIT_PART,.arg = delim,.arglen = dlen,.field = field};
	return true;
}

static bool
valid_at(const uint64 *valid, uint32 r)
{
	return valid == NULL || ((valid[r >> 6] >> (r & 63)) & 1);
}

/* the lower of one string: in tmp when it is of at most 12 bytes, else in the arena; false when this pipeline cannot make it */
static bool
lower_one(const SiftVstrPipe *p, SiftVstrArena *ar, char *tmp, const char **s, uint32 *len)
{
	const bool	ascii = sift_vstr_is_ascii(*s, *len);
	char	   *dst;

	if (ascii ? !p->ascii_fold : !p->c_collation)
	{
		uint32		olen;
		char	   *r = p->lower_fn != NULL ? p->lower_fn(p->lower_arg, ar, *s, *len, &olen) : NULL;

		if (r == NULL)
			return false;
		*s = r;
		*len = olen;
		return true;
	}
	dst = *len <= 12 ? tmp : sift_vstr_arena_alloc(ar, *len);
	lower_bytes(dst, *s, *len);
	*s = dst;
	return true;
}

bool
sift_vstr_pipe_run(const SiftVstrPipe *p, SiftVstrArena *ar, const SiftVec *in, const uint64 *active, uint16 n, SiftStrView *out, uint64 *valid_out)
{
	const SiftStrView *src = in->data;
	const uint64 *valid = in->all_valid ? NULL : in->valid;
	const uint32 words = ((uint32) n + 63) / 64;

	memset(valid_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);
			const char *s;
			uint32		len;
			char		tmp[12];

			if (r >= n || !valid_at(valid, r))
				continue;
			s = sift_vstr_bytes(&src[r]);
			len = src[r].len;
			for (int k = 0; k < p->nsteps; k++)
			{
				const SiftVstrStep *st = &p->step[k];

				if (st->op == SVS_LOWER)
				{
					if (!lower_one(p, ar, tmp, &s, &len))
						return false;
				}
				else if (st->op == SVS_BTRIM)
					sift_vstr_btrim(s, len, st->trim, &s, &len);
				else
					sift_vstr_split_part(s, len, st->arg, st->arglen, st->field, &s, &len);
			}
			out[r] = sift_vstr_make(s, len);
			valid_out[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
	return true;
}

/* ---- comparisons and IN lists ---- */

void
sift_vstr_cmp_const(const SiftStrView *v, const uint64 *valid, const uint64 *active, uint16 n, SiftVstrCmp op, const char *c, uint32 clen, SiftVstrCmpFn fn, void *fn_arg,
					uint64 *true_out, uint64 *known_out)
{
	const uint32 words = ((uint32) n + 63) / 64;

	memset(true_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	memset(known_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);
			const char *s;
			int			cmp;
			bool		hit;

			if (r >= n || !valid_at(valid, r))
				continue;
			s = sift_vstr_bytes(&v[r]);
			if (op == SVC_EQ || op == SVC_NE)
				cmp = (v[r].len == clen && memcmp(s, c, clen) == 0) ? 0 : 1;
			else
				cmp = fn != NULL ? fn(fn_arg, s, v[r].len, c, clen) : sift_vstr_cmp_bytes(s, v[r].len, c, clen);
			switch (op)
			{
				case SVC_EQ:
					hit = cmp == 0;
					break;
				case SVC_NE:
					hit = cmp != 0;
					break;
				case SVC_LT:
					hit = cmp < 0;
					break;
				case SVC_LE:
					hit = cmp <= 0;
					break;
				case SVC_GT:
					hit = cmp > 0;
					break;
				default:
					hit = cmp >= 0;
					break;
			}
			known_out[r >> 6] |= UINT64CONST(1) << (r & 63);
			true_out[r >> 6] |= (uint64) hit << (r & 63);
		}
}

typedef struct SetItem
{
	const char *p;
	uint32		len;
} SetItem;

static int
item_cmp(const void *a, const void *b)
{
	const SetItem *x = a,
			   *y = b;

	return sift_vstr_cmp_bytes(x->p, x->len, y->p, y->len);
}

/* The slot hash of the strings of a set: whole words folded, then the tail */
static uint32
set_hash(const unsigned char *p, uint32 len)
{
	uint64		h = len;
	uint64		w;

	for (; len >= 8; p += 8, len -= 8)
	{
		memcpy(&w, p, 8);
		h = sift_fold64(h, w);
	}
	w = 0;
	memcpy(&w, p, len);
	return (uint32) sift_fmix64(sift_fold64(h, w));
}

SiftVstrSet *
sift_vstr_set_build(const char *const *p, const uint32 *len, const bool *isnull, uint32 n)
{
	SiftVstrSet *s = palloc0(sizeof(SiftVstrSet));
	SetItem    *it = palloc(Max(n, 1) * sizeof(SetItem));
	uint32		m = 0;
	uint64		total = 0;
	char	   *at;

	for (uint32 i = 0; i < n; i++)
	{
		if (isnull != NULL && isnull[i])
			s->has_null = true;
		else
		{
			it[m++] = (SetItem) {p[i], len[i]};
			total += len[i];
		}
	}
	qsort(it, m, sizeof(SetItem), item_cmp);
	s->v = MemoryContextAllocAligned(CurrentMemoryContext, Max(m, 1) * sizeof(SiftStrView), _Alignof(SiftStrView), 0);
	s->bytes = palloc(Max(total, 1));
	at = s->bytes;
	for (uint32 i = 0; i < m; i++)
	{
		if (i > 0 && item_cmp(&it[i], &it[i - 1]) == 0)
			continue;
		memcpy(at, it[i].p, it[i].len);
		s->v[s->n++] = sift_vstr_make(at, it[i].len);
		s->len_mask |= UINT64CONST(1) << (it[i].len & 63);
		at += it[i].len;
	}
	pfree(it);
	if (s->n >= SIFT_VSTR_HASH_MIN)
	{
		uint32		slots = SIFT_VSTR_HASH_MIN * 2;

		while (slots < s->n * 2)
			slots <<= 1;
		s->mask = slots - 1;
		s->tab = palloc0((Size) (s->mask + 1) * sizeof(uint32));
		for (uint32 i = 0; i < s->n; i++)
		{
			uint32		h = set_hash((const unsigned char *) sift_vstr_bytes(&s->v[i]), s->v[i].len) & s->mask;

			while (s->tab[h])
				h = (h + 1) & s->mask;
			s->tab[h] = i + 1;
		}
	}
	return s;
}

bool
sift_vstr_set_has(const SiftVstrSet *s, const char *p, uint32 len)
{
	uint32		lo = 0,
				hi = s->n;

	if (s->tab != NULL)
	{
		uint32		h;

		if (!((s->len_mask >> (len & 63)) & 1))
			return false;
		for (h = set_hash((const unsigned char *) p, len) & s->mask; s->tab[h]; h = (h + 1) & s->mask)
		{
			const SiftStrView *e = &s->v[s->tab[h] - 1];

			if (e->len == len && memcmp(sift_vstr_bytes(e), p, len) == 0)
				return true;
		}
		return false;
	}

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;
		const int	c = sift_vstr_cmp_bytes(sift_vstr_bytes(&s->v[mid]), s->v[mid].len, p, len);

		if (c == 0)
			return true;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

void
sift_vstr_set_free(SiftVstrSet *s)
{
	pfree(s->v);
	pfree(s->bytes);
	if (s->tab)
		pfree(s->tab);
	pfree(s);
}

void
sift_vstr_set_dict_bits(const SiftVstrSet *s, const SiftStrView *dict, uint32 n, uint64 *bits)
{
	memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		if (sift_vstr_set_has(s, sift_vstr_bytes(&dict[i]), dict[i].len))
			bits[i >> 6] |= UINT64CONST(1) << (i & 63);
}

void
sift_vstr_in_set(const SiftStrView *v, const uint64 *valid, const uint64 *active, uint16 n, const SiftVstrSet *s, uint64 *true_out, uint64 *known_out)
{
	const uint32 words = ((uint32) n + 63) / 64;

	memset(true_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	memset(known_out, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);
			bool		found;

			if (r >= n || !valid_at(valid, r))
				continue;
			found = sift_vstr_set_has(s, sift_vstr_bytes(&v[r]), v[r].len);
			if (found || !s->has_null)
				known_out[r >> 6] |= UINT64CONST(1) << (r & 63);
			true_out[r >> 6] |= (uint64) found << (r & 63);
		}
}
