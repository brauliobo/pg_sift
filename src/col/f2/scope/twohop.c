/*
 * col/f2/scope/twohop.c - the pairs (member, scope key) of a scope of format major 2, read from the btrees its declaration records, as the snapshot sees them: the pairs relation itself, and for a
 * scope of two paths the union of those and of the members of the groups the joined path gives each key (the joined relation holds (group, key), the via relation (group, member)). The pairs are
 * collected within maintenance_work_mem: past it they go to sorts that write their runs to temporary files (spool.h), and the map is made from their sorted rows.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"
#include "col/f2/scope/spool.h"
#include "col/scope/scope.h"
#include "core/memtrack.h"

bool
sift_f2s_int_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID;
}

/* The rows (a, b) of the btree of rel under snap: the pairs when a is the member and b the key; the (group, key) or (group, member) rows of a hop. A string column is read as the codes of its map; out NULL: the rows are only met, so that a writer's map holds every key */
static void
scan_pairs(Oid btree, Relation rel, AttrNumber a, SiftKeyMap *ma, AttrNumber b, SiftKeyMap *mb, Snapshot snap, SiftScopeSpool *out)
{
	Relation	ix = index_open(btree, AccessShareLock);
	IndexScanDesc scan = sift_index_beginscan(rel, ix, snap, 0);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	const Oid	ta = TupleDescAttr(rel->rd_att, a - 1)->atttypid,
				tb = TupleDescAttr(rel->rd_att, b - 1)->atttypid;

	index_rescan(scan, NULL, 0, NULL, 0);
	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		bool		na,
					nb;
		const Datum x = slot_getattr(slot, a, &na),
					y = slot_getattr(slot, b, &nb);

		if (!na && !nb)
		{
			const int64 code_a = sift_scope_key_code(ta, ma, x),
						code_b = sift_scope_key_code(tb, mb, y);

			if (out != NULL)
				sift_scope_spool_put(out, code_a, code_b);
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(ix, NoLock);
}

/* The members of each group the joined path gives a key: the (group, member) rows of the via relation that meet the (group, key) rows, both read ascending (a group's keys once each) */
static void
join_via(SiftScopeSpool *groups, SiftScopeSpool *hop, SiftScopeSpool *out)
{
	int64	   *keys = NULL;
	int64		g,
				k,
				hg,
				member,
				run = 0;
	uint32		nk = 0,
				cap = 0;
	bool		have = false,
				more;

	sift_scope_spool_sort(groups);
	sift_scope_spool_sort(hop);
	more = sift_scope_spool_next(groups, &g, &k);
	while (sift_scope_spool_next(hop, &hg, &member))
	{
		while (more && g < hg)
			more = sift_scope_spool_next(groups, &g, &k);
		if (!have || run != hg)
		{
			nk = 0;
			for (; more && g == hg; more = sift_scope_spool_next(groups, &g, &k))
			{
				if (nk > 0 && keys[nk - 1] == k)
					continue;
				if (nk == cap)
					keys = keys ? repalloc_huge(keys, sizeof(int64) * (cap *= 2)) : palloc(sizeof(int64) * (cap = 64));
				keys[nk++] = k;
			}
			run = hg;
			have = true;
		}
		for (uint32 i = 0; i < nk; i++)
			sift_scope_spool_put(out, member, keys[i]);
	}
	if (keys != NULL)
		pfree(keys);
}

static void
grow(void **p, uint32 *cap, uint32 need, Size width)
{
	if (need <= *cap)
		return;
	*cap = Max(need, *cap * 2);
	*p = *p ? repalloc(*p, (Size) *cap * width) : palloc((Size) *cap * width);
}

/* The map of pairs that went to a sort, made as sift_spm_build makes it of the pairs of an array: the keys of the pairs sorted and distinct, then the keys of each member by their ordinals; a multiset keeps the repeated pairs */
static SiftScopePairMap *
sorted_map(SiftScopeSpool *pairs, SiftMemTracker *mem, bool multiset)
{
	SiftScopePairMap *pm = palloc0(sizeof(*pm));
	SiftScopeSpool *keys = sift_scope_spool_new(mem);
	int64	   *scope_keys = NULL,
			   *members = NULL;
	uint32	   *offsets = NULL,
			   *ords = NULL;
	uint32		nk = 0,
				kcap = 0,
				nmem = 0,
				mcap = 0,
				ocap = 0,
				n = 0,
				ncap = 0;
	int64		a,
				b,
				pa = 0,
				pb = 0;

	sift_scope_spool_sort(pairs);
	while (sift_scope_spool_next(pairs, &a, &b))
		sift_scope_spool_put(keys, b, 0);
	sift_scope_spool_sort(keys);
	while (sift_scope_spool_next(keys, &a, &b))
		if (nk == 0 || scope_keys[nk - 1] != a)
		{
			grow((void **) &scope_keys, &kcap, nk + 1, sizeof(int64));
			scope_keys[nk++] = a;
		}
	sift_scope_spool_free(keys);
	pm->scope_keys = scope_keys;
	pm->nkeys = nk;
	sift_scope_spool_rewind(pairs);
	while (sift_scope_spool_next(pairs, &a, &b))
	{
		if (!multiset && n > 0 && a == pa && b == pb)
			continue;
		if (n == 0 || a != pa)
		{
			grow((void **) &members, &mcap, nmem + 1, sizeof(int64));
			grow((void **) &offsets, &ocap, nmem + 2, sizeof(uint32));
			members[nmem] = a;
			offsets[nmem++] = n;
		}
		grow((void **) &ords, &ncap, n + 1, sizeof(uint32));
		ords[n++] = (uint32) sift_spm_key_ordinal(pm, b);
		pa = a;
		pb = b;
	}
	offsets[nmem] = n;
	pm->scope_keys = repalloc(scope_keys, sizeof(int64) * nk);
	pm->member_ids = repalloc(members, sizeof(int64) * nmem);
	pm->offsets = repalloc(offsets, sizeof(uint32) * (nmem + 1));
	pm->key_ordinals = repalloc(ords, sizeof(uint32) * n);
	pm->nmembers = nmem;
	pm->npairs = n;
	return pm;
}

/* The map of the codes of the scope key of the pairs relation: NULL for an integer key; a writer's interns the keys the store lacks, all of them as one run (the pass that meets them comes first) */
SiftKeyMap *
sift_f2s_key_map(Relation heap, Relation index, const SiftScopeSpec *spec, bool write)
{
	Relation	rel;
	AttrNumber	member,
				key;
	SiftKeyMap *km;

	if (heap == index)
		(void) sift_scope_relation_index(heap, index, spec, &rel, &member, &key);
	else
		sift_scope_relation(heap, index, spec, &rel, &member, &key);
	km = sift_keymap_open(rel, key, write);
	table_close(rel, NoLock);
	return km;
}

static void
check_pair_types(Relation rel, AttrNumber member, AttrNumber key, const char *what)
{
	const Oid	tkey = TupleDescAttr(rel->rd_att, key - 1)->atttypid;

	if (!sift_f2s_pair_types(TupleDescAttr(rel->rd_att, member - 1)->atttypid, tkey))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the pairs of scope \"%s\" are not integers or strings", what),
						errhint("Format major 2 summarizes scopes whose member is an integer, text, varchar or uuid column and whose key is an integer, date, timestamp, timestamptz, text, varchar or uuid column.")));
}

static void
collect(SiftKeyMap *m, bool on)
{
	if (m != NULL)
		sift_keymap_collect(m, on);
}

SiftScopePairMap *
sift_f2s_load_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, bool intern)
{
	SiftMemTracker mem;
	SiftScopeSpool *pairs;
	Relation	rel;
	AttrNumber	m0,
				k0;
	const Oid	b0 = sift_scope_relation_index(heap, index, sc->spec, &rel, &m0, &k0);
	SiftKeyMap *mm = sift_f2s_mem_map(sc->mem);
	SiftScopePairMap *pm;
	const bool	multiset = sift_scope_pairs_multiset(index, sc->spec);

	check_pair_types(rel, m0, k0, sc->spec->key_name);
	sift_mem_init(&mem, "scope pairs", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 1, 1);
	pairs = sift_scope_spool_new(&mem);
	if (intern && (km != NULL || mm != NULL))
	{
		collect(km, true);
		collect(mm, true);
		scan_pairs(b0, rel, m0, mm, k0, km, snap, NULL);
		collect(km, false);
		collect(mm, false);
	}
	scan_pairs(b0, rel, m0, mm, k0, km, snap, pairs);
	table_close(rel, NoLock);
	if (sc->spec->joined != NULL)
	{
		SiftScopeSpool *groups = sift_scope_spool_new(&mem),
				   *hop = sift_scope_spool_new(&mem);
		Relation	joined,
					via;
		AttrNumber	g1,
					k1,
					g2,
					m2;
		const Oid	b1 = sift_scope_hop_index(heap, index, sc->spec, 1, &joined, &g1, &k1),
					b2 = sift_scope_hop_index(heap, index, sc->spec, 2, &via, &g2, &m2);

		scan_pairs(b1, joined, g1, NULL, k1, km, snap, groups);
		scan_pairs(b2, via, g2, NULL, m2, NULL, snap, hop);
		join_via(groups, hop, pairs);
		sift_scope_spool_free(groups);
		sift_scope_spool_free(hop);
		table_close(via, NoLock);
		table_close(joined, NoLock);
	}
	if (sift_scope_spool_spilled(pairs))
		pm = sorted_map(pairs, &mem, multiset);
	else
	{
		uint32		n;
		SiftScopeMemberKey *rows = sift_scope_spool_rows(pairs, &n);

		pm = sift_spm_build(rows, n, multiset);
	}
	sift_scope_spool_free(pairs);
	sift_mem_end(&mem);
	return pm;
}

/* The members of the pairs of one scope key as the snapshot sees them, ascending, palloc'd in *out: the pairs of the declaration, its predicates and both of its paths included */
uint32
sift_f2s_key_members(Relation heap, Relation index, int ord, Snapshot snap, int64 key, int64 **out)
{
	SiftF2ScopeCfg sc;
	SiftScopePairMap *pm;
	uint32		n = 0;

	sift_f2s_cfg(index, sift_f2_store_open(index), ord, &sc);
	pm = sift_f2s_load_pairs(heap, index, &sc, snap, sift_f2s_key_map(heap, index, sc.spec, false), false);
	*out = palloc(Max(pm->nmembers, 1) * sizeof(int64));
	for (uint32 i = 0; i < pm->nmembers; i++)
		if (sift_spm_owns_key(pm, pm->member_ids[i], key))
			(*out)[n++] = pm->member_ids[i];
	sift_spm_free(pm);
	return n;
}
