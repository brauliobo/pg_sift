/*
 * agg/group/cmp.c - the layout, the growth and the numbers of the strings of the compact group table (agg/group/cmp.h). Nothing here calls the server but to allocate, and a table of an
 * arena does not even do that.
 */
#include "postgres.h"

#include "agg/group/cmp.h"

#define CMP_ARRAY_ALIGN	8

bool
cmp_layout(SiftCompact *c, bool key1_u32)
{
	int			need = c->kw + 1 + c->nsum + c->has_gid;

	c->strm = c->strk == 0 && c->kw == 1 && c->nsum == 0 && !c->has_gid;
	c->cnt_hi = c->strm || (key1_u32 && c->kw == 2 && c->strk < 0 && !c->has_gid && cmp_slot_words(need - 1) < cmp_slot_words(need));
	need -= c->cnt_hi;
	if (need > CMP_MAX_WORDS)
		return false;
	c->sw = cmp_slot_words(need);
	c->sum0 = c->cnt_hi ? c->kw : c->kw + 1;
	c->cw = c->cnt_hi ? 1 : c->kw;
	c->cshift = c->cnt_hi ? 32 : 0;
	c->cinc = UINT64CONST(1) << c->cshift;
	c->last_mask = c->cnt_hi ? UINT32_MAX : UINT64_MAX;
	c->gidw = c->sum0 + c->nsum;
	return true;
}

bool
cmp_init(SiftCompact *c, uint64 cap)
{
	c->cap = cap;
	c->mask = cap - 1;
	c->count = 0;
	c->tab = cmp_alloc(c, cap * c->sw * sizeof(uint64), SIFT_ARENA_ALIGN, true);
	return c->tab != NULL;
}

/* ---- the numbers of the strings ---- */

static uint32
intern_slot(const Intern *in, uint64 h, const char *p, uint32 len)
{
	uint32		i = (uint32) h & (in->cap - 1);

	while (in->ht[i] != 0 && !(in->str[in->ht[i] - 1].hash == h && in->str[in->ht[i] - 1].len == len && memcmp(in->arena + in->str[in->ht[i] - 1].off, p, len) == 0))
		i = (i + 1) & (in->cap - 1);
	return i;
}

/* A larger array, with what the old one held: repalloc on a backend; in an arena the old one stays where it is. NULL when the arena has no room */
static void *
regrow(const SiftCompact *c, void *old, Size oldn, Size newn)
{
	void	   *p;

	if (c->arena == NULL && old != NULL)
		return repalloc(old, newn);
	p = cmp_alloc(c, newn, CMP_ARRAY_ALIGN, false);
	if (p != NULL && old != NULL)
		memcpy(p, old, oldn);
	return p;
}

/* The slots of the string table for n strings, the strings that number them and the bytes of the arena that hold need bytes: 0 past MaxAllocSize, where a table of the backend stops */
static uint32
ht_cap_for(const Intern *in, uint64 n)
{
	uint64		cap = in->cap;

	while (n * 2 > cap)
		cap = cap == 0 ? 1024 : cap * 2;
	return cap * sizeof(uint32) > MaxAllocSize ? 0 : (uint32) cap;
}

static uint32
str_cap_for(const Intern *in, uint64 n)
{
	const uint64 cap = n <= in->str_cap ? in->str_cap : Max(Max(1024, (uint64) in->str_cap * 2), n);

	return cap * sizeof(IStr) > MaxAllocSize ? 0 : (uint32) cap;
}

static Size
arena_cap_for(const Intern *in, Size need)
{
	if (in->arena != NULL && need <= in->arena_cap)
		return in->arena_cap;
	return need > MaxAllocSize ? 0 : Min(Max(Max(in->arena_cap * 2, need), 4096), MaxAllocSize);
}

Size
cmp_grow_peak(const SiftCompact *c, Size bytes, uint32 n)
{
	const Intern *in = &c->in;
	const uint32 hcap = n == 0 ? in->cap : ht_cap_for(in, (uint64) in->n + n);
	const uint32 scap = n == 0 ? in->str_cap : str_cap_for(in, (uint64) in->n + n);
	const Size	acap = arena_cap_for(in, in->used + bytes);

	if ((n > 0 && (hcap == 0 || scap == 0)) || acap == 0)
		return SIZE_MAX;
	return cmp_bytes(c) + (hcap != in->cap ? (Size) hcap * sizeof(uint32) : 0) + (scap != in->str_cap ? (Size) scap * sizeof(IStr) : 0) + (acap != in->arena_cap ? acap : 0);
}

static bool
intern_grow(SiftCompact *c)
{
	Intern	   *in = &c->in;
	const uint32 ncap = ht_cap_for(in, (uint64) in->cap / 2 + 1);
	uint32	   *ht = ncap == 0 ? NULL : cmp_alloc(c, (Size) ncap * sizeof(uint32), CMP_ARRAY_ALIGN, true);

	if (ht == NULL)
		return false;
	for (uint32 id = 0; id < in->n; id++)
	{
		uint32		i = (uint32) in->str[id].hash & (ncap - 1);

		while (ht[i] != 0)
			i = (i + 1) & (ncap - 1);
		ht[i] = id + 1;
	}
	if (in->ht != NULL)
		cmp_free(c, in->ht);
	in->ht = ht;
	in->cap = ncap;
	return true;
}

bool
cmp_intern_reserve(SiftCompact *c, uint32 n, Size bytes)
{
	Intern	   *in = &c->in;

	while ((Size) (in->n + n) * 2 > in->cap)
		if (!intern_grow(c))
			return false;
	if (in->n + n > in->str_cap)
	{
		const uint32 cap = str_cap_for(in, (uint64) in->n + n);
		IStr	   *str = cap == 0 ? NULL : regrow(c, in->str, (Size) in->n * sizeof(IStr), (Size) cap * sizeof(IStr));

		if (str == NULL)
			return false;
		in->str = str;
		in->str_cap = cap;
	}
	return sstr_put(c, NULL, bytes);
}

uint32
cmp_intern_h(SiftCompact *c, uint64 h, const char *p, uint32 len)
{
	Intern	   *in = &c->in;
	uint32		i;

	if ((in->n + 1) * 2 > in->cap && !intern_grow(c))
		return CMP_NO_ID;
	i = intern_slot(in, h, p, len);
	if (in->ht[i] != 0)
		return in->ht[i] - 1;
	if (in->n == in->str_cap && !cmp_intern_reserve(c, 1, len))
		return CMP_NO_ID;
	if (!sstr_put(c, NULL, len))
		return CMP_NO_ID;
	if (len > 0)
		memcpy(in->arena + in->used, p, len);
	in->str[in->n] = (IStr) {h, (uint32) in->used, len};
	in->used += len;
	in->ht[i] = ++in->n;
	return in->n - 1;
}

/* room for len more bytes of strings (the bytes themselves when p is not NULL) */
bool
sstr_put(SiftCompact *c, const char *p, uint32 len)
{
	Intern	   *in = &c->in;

	if (in->arena == NULL || in->used + len > in->arena_cap)
	{
		const Size	cap = arena_cap_for(in, in->used + len);
		char	   *bytes = cap == 0 ? NULL : regrow(c, in->arena, in->used, cap);

		if (bytes == NULL)
			return false;
		in->arena = bytes;
		in->arena_cap = cap;
	}
	if (p != NULL)
	{
		if (len > 0)
			memcpy(in->arena + in->used, p, len);
		in->used += len;
	}
	return true;
}

static Size
cmp_intern_bytes(const Intern *in)
{
	return (Size) in->cap * sizeof(uint32) + (Size) in->str_cap * sizeof(IStr) + in->arena_cap;
}

/* ---- the table ---- */

uint64
cmp_bytes(const SiftCompact *c)
{
	return c->cap * c->sw * sizeof(uint64) + cmp_intern_bytes(&c->in) + (Size) c->code_cap * (c->strm ? sizeof(uint64) : sizeof(uint32));
}

void
sstr_move(SiftCompact *c, const uint64 *old, uint64 ocap)
{
	for (uint64 i = 0; i < ocap; i++)
	{
		const uint64 *s = old + i * 2;
		uint64		j;

		if (cmp_count(c, s) == 0)
			continue;
		for (j = sl_h32(s) & c->mask; cmp_count(c, sstr_slot(c, j)) != 0; j = (j + 1) & c->mask)
			;
		memcpy(sstr_slot(c, j), s, 2 * sizeof(uint64));
		c->count++;
	}
}

static bool
cmp_grow(SiftCompact *c, uint64 budget)
{
	uint64	   *old = c->tab;
	const uint64 ocap = c->cap;
	uint64	   *tab;

	if ((ocap * 3 * c->sw * sizeof(uint64)) + cmp_intern_bytes(&c->in) > budget)
		return false;
	tab = cmp_alloc(c, ocap * 2 * c->sw * sizeof(uint64), SIFT_ARENA_ALIGN, true);
	if (tab == NULL)
		return false;
	c->cap = ocap * 2;
	c->mask = c->cap - 1;
	c->tab = tab;
	c->count = 0;
	c->by_n = 0;
	if (c->strm)
		sstr_move(c, old, ocap);
	for (uint64 i = 0; !c->strm && i < ocap; i++)
	{
		const uint64 *s = old + i * c->sw;

		if (cmp_count(c, s) != 0)
			memcpy(cmp_find(c, cmp_key_hash(c, s), s), s, c->sw * sizeof(uint64));
	}
	cmp_free(c, old);
	return true;
}

bool
cmp_reserve(SiftCompact *c, uint64 n, uint64 budget)
{
	while ((c->count + n) * 8 > c->cap * 5)
		if (!cmp_grow(c, budget))
			return false;
	return true;
}

void
cmp_free_all(SiftCompact *c)
{
	if (c->in.ht != NULL)
		cmp_free(c, c->in.ht);
	if (c->in.str != NULL)
		cmp_free(c, c->in.str);
	if (c->in.arena != NULL)
		cmp_free(c, c->in.arena);
	cmp_free(c, c->tab);
}
