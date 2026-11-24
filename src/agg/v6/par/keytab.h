/*
 * agg/v6/par/keytab.h - the keys of a thread, in pure C: a table that gives each distinct key (up to SIFT_PAR_GROUP_MAX_KEYS words and at most one text) a number, 0, 1, 2 ... in the order the keys came,
 * in the arena of the thread. No server call: an arena with no room is SIFT_KEYTAB_NONE.
 *
 * A key is equal to another when its hash, its words and its bytes are: the hash alone never says that two keys are the same. The entry of a key is ew words of 8 bytes, the hash, the place of the
 * bytes, their length, the words, then npay words of the caller, zero at the first sight of the key; an entry is at the same place until the table grows (and then it is copied). The bytes of a text
 * are copied into the arena, unless the table borrows them: the keys of another table of the same run (sift_keytab_intern_of), which stay where they are until the run is done.
 */
#ifndef SIFT_AGG_V6_PAR_KEYTAB_H
#define SIFT_AGG_V6_PAR_KEYTAB_H

#include "postgres.h"

#include "agg/v6/par/group.h"
#include "core/par/arena.h"

#define SIFT_KEYTAB_NONE	UINT32_MAX
#define SIFT_KEY_HEAD		3

typedef struct SiftKeyShape
{
	uint8		kw;				/* the words of a key */
	bool		str;			/* and a text */
	uint8		npay;			/* the words of an entry that belong to the caller */
} SiftKeyShape;

typedef struct SiftKey
{
	uint64		h;				/* sift_key_hash of the rest */
	const uint64 *w;
	const uint8 *p;
	uint32		len;
} SiftKey;

typedef struct SiftKeyTab
{
	SiftArena  *a;
	SiftKeyShape sh;
	bool		borrow;
	uint32		ew;
	uint32		count;
	uint32		ecap;
	uint32		scap;
	uint64	   *ent;
	uint64	   *slot;
} SiftKeyTab;

/* The hash of a key: the words and the bytes, in an order that makes {1, "a"} and {"a", 1} two keys of two shapes, not one */
extern uint64 sift_key_hash(const SiftKeyShape *sh, const uint64 *w, const uint8 *p, uint32 len);

/* false when the arena has no room for the first entries */
extern bool sift_keytab_init(SiftKeyTab *t, SiftArena *a, SiftKeyShape sh, bool borrow);

/* The number of the key (k->h is its hash), a new one when it is new; SIFT_KEYTAB_NONE when the arena has no room: the table is then as it was */
extern uint32 sift_keytab_intern(SiftKeyTab *t, const SiftKey *k);

static inline uint64 *
sift_keytab_at(const SiftKeyTab *t, uint32 id)
{
	return t->ent + (size_t) id * t->ew;
}

static inline uint64 *
sift_keytab_pay(const SiftKeyTab *t, uint64 *e)
{
	return e + SIFT_KEY_HEAD + t->sh.kw;
}

/* Brings in the slot of a hash, and (once that has arrived) the entry its slot holds: the keys of a batch are looked up a few at a time ahead */
static inline void
sift_keytab_prefetch_slot(const SiftKeyTab *t, uint64 h)
{
	__builtin_prefetch(&t->slot[h & (t->scap - 1)]);
}

static inline void
sift_keytab_prefetch_ent(const SiftKeyTab *t, uint64 h)
{
	const uint64 s = t->slot[h & (t->scap - 1)];

	if (s != 0)
		__builtin_prefetch(sift_keytab_at(t, (uint32) s - 1));
}

static inline SiftKey
sift_keytab_key(const uint64 *e)
{
	return (SiftKey) {.h = e[0],.w = e + SIFT_KEY_HEAD,.p = (const uint8 *) (uintptr_t) e[1],.len = (uint32) e[2]};
}

/* The number, in t, of the key of entry e of a table of the shape of t (a table that borrows takes the bytes of e as they are) */
static inline uint32
sift_keytab_intern_of(SiftKeyTab *t, const uint64 *e)
{
	const SiftKey k = sift_keytab_key(e);

	return sift_keytab_intern(t, &k);
}

#endif							/* SIFT_AGG_V6_PAR_KEYTAB_H */
