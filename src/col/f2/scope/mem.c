/*
 * col/f2/scope/mem.c - the text, varchar and uuid members of a scope of format major 2 (f2_scope_mem.h): the codes of the scope key store of the member column, read for the rows of a granule or a slab.
 */
#include "postgres.h"

#include "access/table.h"

#include "col/f2/scope/mem.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "col/granule/granule.h"
#include "col/keystore/keystore.h"
#include "col/scope_v6/overlay.h"
#include "core/cmp.h"

struct SiftF2Mem
{
	SiftKeyMap *map;			/* the codes of the pairs: a writer's interns the members the store lacks, a reader's gives a pair that gained one a negative code of its own */
	Oid			store_oid;
	SiftKeyStore *store;		/* the same store, open while a loop asks for the codes of rows (sift_f2s_mem_hold): a value it lacks has no code unless the map has given it one */
	int			holds;
};

bool
sift_f2s_mem_leaf(const SiftColDef *d)
{
	return sift_f2s_plain_leaf(d) && d->store == SIFT_COLS_STR && !(d->flags & SIFT_COLD_CODED) && (d->type == SIFT_COLT_TEXT || d->type == SIFT_COLT_UUID);
}

SiftF2Mem *
sift_f2s_mem_open(Relation heap, Relation index, const SiftF2Store *store, const SiftF2ScopeCfg *sc, bool write)
{
	Relation	rel;
	AttrNumber	member,
				key;
	SiftF2Mem  *mem;

	if (store->schema->cols[sc->member_leaf].store != SIFT_COLS_STR)
		return NULL;
	sift_scope_relation(heap, index, sc->spec, &rel, &member, &key);
	if (write)
		(void) sift_keystore_ensure(rel, index, member);
	mem = palloc(sizeof(SiftF2Mem));
	mem->map = sift_keymap_open(rel, member, write);
	mem->store_oid = sift_keystore_find(rel, member);
	mem->store = NULL;
	mem->holds = 0;
	table_close(rel, NoLock);
	return mem;
}

void
sift_f2s_mem_hold(SiftF2Mem *mem)
{
	if (mem->holds++ == 0)
	{
		mem->store = sift_keystore_open(mem->store_oid, AccessShareLock);
		sift_keystore_refresh(mem->store);
	}
}

void
sift_f2s_mem_release(SiftF2Mem *mem)
{
	if (--mem->holds == 0)
	{
		sift_keystore_close(mem->store);
		mem->store = NULL;
	}
}

SiftKeyMap *
sift_f2s_mem_map(const SiftF2Mem *mem)
{
	return mem != NULL ? mem->map : NULL;
}

bool
sift_f2s_mem_transient(const SiftF2Mem *mem)
{
	return mem != NULL && sift_keymap_transient_n(mem->map) > 0;
}

bool
sift_f2s_mem_code(const SiftF2Mem *mem, const char *s, uint32 len, int64 *code)
{
	Assert(mem->store != NULL);
	return (*code = sift_keystore_find_code(mem->store, s, len)) >= 0 || sift_keymap_find_bytes(mem->map, s, len, code);
}

const int64 *
sift_f2s_mem_ints(SiftF2Mem *mem, const SiftGranuleView *v, int leaf, const uint16 *at, uint32 n, const uint8 **nulls)
{
	int64	   *code = palloc0(Max(n, 1) * sizeof(int64));
	bool	   *none = palloc0(Max(n, 1));
	SiftStrCell *cell = palloc(Max(n, 1) * sizeof(SiftStrCell));
	const SiftStrCell *last = NULL;
	int64		last_code = 0;
	bool		last_has = false;

	if (at != NULL)
		sift_granule_point_strs(v, leaf, at, (uint16) n, cell);
	else
		sift_granule_strs(v, leaf, cell);
	sift_f2s_mem_hold(mem);
	for (uint32 i = 0; i < n; i++)
	{
		if (cell[i].isnull)
		{
			none[i] = true;
			continue;
		}
		if (last == NULL || last->len != cell[i].len || memcmp(last->p, cell[i].p, cell[i].len) != 0)
		{
			last_has = sift_f2s_mem_code(mem, cell[i].p, cell[i].len, &last_code);
			last = &cell[i];
		}
		code[i] = last_code;
		none[i] = !last_has;
	}
	sift_f2s_mem_release(mem);
	pfree(cell);
	*nulls = (const uint8 *) none;
	return code;
}

static bool
named_by(const int64 *words, uint32 nwords, const char *s, uint32 len)
{
	const int64 word = (int64) sift_member_hash_bytes(s, len);

	return bsearch(&word, words, nwords, sizeof(int64), sift_cmp_i64) != NULL;
}

static void
members_add(int64 **m, uint32 *n, uint32 *cap, int64 code)
{
	if (*n == *cap)
		*m = repalloc(*m, (*cap *= 2) * sizeof(int64));
	(*m)[(*n)++] = code;
}

bool
sift_f2s_mem_translate(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, int64 **changed, uint32 *nchanged)
{
	SiftF2Mem  *mem = sc->mem;
	uint32		n = 0,
				cap = 64,
				len;
	int64	   *m;

	if (mem == NULL || *nchanged == 0)
		return true;
	sift_spm_free(sift_f2s_load_pairs(heap, index, sc, snap, km, false));
	m = palloc(cap * sizeof(int64));
	sift_f2s_mem_hold(mem);
	for (uint64 c = 0, total = sift_keystore_ncodes(mem->store); c < total; c++)
	{
		const char *s = sift_keystore_string(mem->store, (uint32) c, &len);

		if (named_by(*changed, *nchanged, s, len))
			members_add(&m, &n, &cap, (int64) c);
	}
	sift_f2s_mem_release(mem);
	for (uint32 t = 0; t < sift_keymap_transient_n(mem->map); t++)
	{
		const char *s = sift_keymap_bytes(mem->map, -1 - (int64) t, &len);

		if (named_by(*changed, *nchanged, s, len))
			members_add(&m, &n, &cap, -1 - (int64) t);
	}
	if (n > SIFT_SCOPE_OVERLAY_MAX_PAIR_EDITS)
		return false;
	qsort(m, n, sizeof(int64), sift_cmp_i64);
	*changed = m;
	*nchanged = n;
	return true;
}

bool
sift_f2s_mem_slab(const SiftF2ScopeCfg *sc, const SiftRowBuf *rb, int r, int64 *member)
{
	if (rb->isnull[sc->member_leaf][r])
		return false;
	if (sc->mem == NULL)
	{
		*member = rb->i[sc->member_leaf][r];
		return true;
	}
	return sift_f2s_mem_code(sc->mem, rb->s[sc->member_leaf][r], rb->slen[sc->member_leaf][r], member);
}

bool
sift_f2_scope_owns_cell(SiftF2Scope *sc, const SiftF2ScopeCell *member, int64 key)
{
	const SiftF2ScopeParts *p = sc->entity.arg;
	int64		code = member->i;
	bool		known = !member->isnull;

	if (known && p->sc->mem != NULL)
	{
		sift_f2s_mem_hold(p->sc->mem);
		known = sift_f2s_mem_code(p->sc->mem, member->s, member->len, &code);
		sift_f2s_mem_release(p->sc->mem);
	}
	return known && sc->entity.owns(sc->entity.arg, code, key);
}
