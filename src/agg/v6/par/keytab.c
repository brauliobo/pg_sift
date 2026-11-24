/*
 * agg/v6/par/keytab.c - the key table of agg/v6/par/keytab.h. Nothing here calls the server.
 */
#include "postgres.h"

#include "agg/kern6.h"
#include "agg/v6/par/keytab.h"
#include "agg/v6/v6_distinct_u64.h"

#define KEYTAB_FIRST	32

uint64
sift_key_hash(const SiftKeyShape *sh, const uint64 *w, const uint8 *p, uint32 len)
{
	uint64		h = 0;

	for (int i = 0; i < sh->kw; i++)
		h = sift_du64_mix(h + w[i] + UINT64CONST(0x9e3779b97f4a7c15));
	return sh->str ? sift_aggk6_hash1(h ^ sift_aggk6_hash_lenpfx(p, len)) : h;
}

static inline uint32
slot_id(uint64 s)
{
	return (uint32) s - 1;
}

static inline uint64
slot_of(uint64 h, uint32 id)
{
	return (h & ~UINT64CONST(0xffffffff)) | (id + 1);
}

static inline bool
key_equal(const SiftKeyTab *t, const uint64 *e, const SiftKey *k)
{
	uint64		diff = e[0] ^ k->h;

	for (int i = 0; i < t->sh.kw; i++)
		diff |= e[SIFT_KEY_HEAD + i] ^ k->w[i];
	return diff == 0 && (uint32) e[2] == k->len && (k->len == 0 || memcmp((const void *) (uintptr_t) e[1], k->p, k->len) == 0);
}

static uint64
slots_empty(const SiftKeyTab *t, uint64 h)
{
	uint64		j = h & (t->scap - 1);

	while (t->slot[j] != 0)
		j = (j + 1) & (t->scap - 1);
	return j;
}

static bool
slots_alloc(SiftKeyTab *t, uint32 scap)
{
	uint64	   *s = sift_arena_alloc0(t->a, (size_t) scap * sizeof(uint64), SIFT_ARENA_ALIGN);

	if (s == NULL)
		return false;
	t->scap = scap;
	t->slot = s;
	for (uint32 id = 0; id < t->count; id++)
	{
		const uint64 h = sift_keytab_at(t, id)[0];

		t->slot[slots_empty(t, h)] = slot_of(h, id);
	}
	return true;
}

static bool
ents_alloc(SiftKeyTab *t, uint32 ecap)
{
	uint64	   *e = sift_arena_alloc(t->a, (size_t) ecap * t->ew * sizeof(uint64), SIFT_ARENA_ALIGN);

	if (e == NULL)
		return false;
	if (t->count > 0)
		memcpy(e, t->ent, (size_t) t->count * t->ew * sizeof(uint64));
	t->ent = e;
	t->ecap = ecap;
	return true;
}

bool
sift_keytab_init(SiftKeyTab *t, SiftArena *a, SiftKeyShape sh, bool borrow)
{
	*t = (SiftKeyTab) {.a = a,.sh = sh,.borrow = borrow,.ew = SIFT_KEY_HEAD + sh.kw + sh.npay};
	return ents_alloc(t, KEYTAB_FIRST) && slots_alloc(t, 2 * KEYTAB_FIRST);
}

static bool
grow(SiftKeyTab *t)
{
	if (t->count == t->ecap && !ents_alloc(t, t->ecap * 2))
		return false;
	return (uint64) (t->count + 1) * 2 <= t->scap || slots_alloc(t, t->scap * 2);
}

static uint32
append(SiftKeyTab *t, const SiftKey *k)
{
	const uint8 *p = k->p;
	uint64	   *e;

	if (!t->borrow && k->len > 0)
	{
		uint8	   *copy = sift_arena_alloc(t->a, k->len, 1);

		if (copy == NULL)
			return SIFT_KEYTAB_NONE;
		memcpy(copy, k->p, k->len);
		p = copy;
	}
	if (!grow(t))
		return SIFT_KEYTAB_NONE;
	e = sift_keytab_at(t, t->count);
	memset(e, 0, (size_t) t->ew * sizeof(uint64));
	e[0] = k->h;
	e[1] = (uint64) (uintptr_t) p;
	e[2] = k->len;
	if (t->sh.kw > 0)
		memcpy(e + SIFT_KEY_HEAD, k->w, t->sh.kw * sizeof(uint64));
	t->slot[slots_empty(t, k->h)] = slot_of(k->h, t->count);
	return t->count++;
}

uint32
sift_keytab_intern(SiftKeyTab *t, const SiftKey *k)
{
	for (uint64 j = k->h & (t->scap - 1); t->slot[j] != 0; j = (j + 1) & (t->scap - 1))
		if ((uint32) (t->slot[j] >> 32) == (uint32) (k->h >> 32) && key_equal(t, sift_keytab_at(t, slot_id(t->slot[j])), k))
			return slot_id(t->slot[j]);
	return append(t, k);
}
