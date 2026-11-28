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
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"
#include "col/f2/scope/spool.h"
#include "col/scope/scope.h"
#include "core/memtrack.h"
#include "core/store/ops.h"

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

		CHECK_FOR_INTERRUPTS();
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
		CHECK_FOR_INTERRUPTS();
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
					keys = keys ? repalloc(keys, sizeof(int64) * (cap *= 2)) : palloc(sizeof(int64) * (cap = 64));
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

/*
 * The pairs the joined path of a scope of two paths gives (the members of the via relation, coded by the member map mm, with the keys of their groups, coded by km), into pairs; NULL: the rows are
 * only met, so that a writer's maps hold every key and member
 */
static void
scan_hops(Relation heap, Relation index, const SiftScopeSpec *spec, SiftKeyMap *mm, SiftKeyMap *km, Snapshot snap, SiftMemTracker *mem, SiftScopeSpool *pairs)
{
	SiftScopeSpool *groups = pairs != NULL ? sift_scope_spool_new(mem) : NULL,
			   *hop = pairs != NULL ? sift_scope_spool_new(mem) : NULL;
	Relation	joined,
				via;
	AttrNumber	g1,
				k1,
				g2,
				m2;
	const Oid	b1 = sift_scope_hop_index(heap, index, spec, 1, &joined, &g1, &k1),
				b2 = sift_scope_hop_index(heap, index, spec, 2, &via, &g2, &m2);

	scan_pairs(b1, joined, g1, NULL, k1, km, snap, groups);
	scan_pairs(b2, via, g2, NULL, m2, mm, snap, hop);
	if (pairs != NULL)
	{
		join_via(groups, hop, pairs);
		sift_scope_spool_free(groups);
		sift_scope_spool_free(hop);
	}
	table_close(via, NoLock);
	table_close(joined, NoLock);
}

/* An array of n of width bytes for the map: exactly what it holds (palloc of 0 for none), the statement failing with the name of what needs more than one allocation */
static void *
map_array(uint32 n, Size width)
{
	return palloc(sift_alloc_grow(0, (Size) n * width, "the pair map of a scope"));
}

/* The distinct keys of the sorted (key, 0) rows */
static int64 *
distinct_keys(SiftScopeSpool *keys, uint32 *nk)
{
	int64	   *out;
	int64		a,
				b,
				prev = 0;

	*nk = 0;
	for (; sift_scope_spool_next(keys, &a, &b); prev = a)
	{
		CHECK_FOR_INTERRUPTS();
		*nk += *nk == 0 || a != prev;
	}
	out = map_array(*nk, sizeof(int64));
	sift_scope_spool_rewind(keys);
	for (uint32 k = 0; sift_scope_spool_next(keys, &a, &b);)
		if (k == 0 || out[k - 1] != a)
			out[k++] = a;
	return out;
}

/*
 * The map of pairs that went to a sort, made as sift_spm_build makes it of the pairs of an array: the keys of the pairs sorted and distinct, then the keys of each member by their ordinals; a
 * multiset keeps the repeated pairs. A first read counts the members and the pairs and spools the keys, so that each array is allocated once at its size.
 */
static SiftScopePairMap *
sorted_map(SiftScopeSpool *pairs, SiftMemTracker *mem, bool multiset)
{
	SiftScopePairMap *pm = palloc0(sizeof(*pm));
	SiftScopeSpool *keys = sift_scope_spool_new(mem);
	uint32		nmem = 0,
				n = 0;
	int64		a,
				b,
				pa = 0,
				pb = 0;

	sift_scope_spool_sort(pairs);
	for (; sift_scope_spool_next(pairs, &a, &b); pa = a, pb = b)
	{
		CHECK_FOR_INTERRUPTS();
		if (!multiset && n > 0 && a == pa && b == pb)
			continue;
		nmem += n == 0 || a != pa;
		n++;
		sift_scope_spool_put(keys, b, 0);
	}
	sift_scope_spool_sort(keys);
	pm->scope_keys = distinct_keys(keys, &pm->nkeys);
	sift_scope_spool_free(keys);
	pm->member_ids = map_array(nmem, sizeof(int64));
	pm->offsets = map_array(nmem + 1, sizeof(uint32));
	pm->key_ordinals = map_array(n, sizeof(uint32));
	sift_scope_spool_rewind(pairs);
	for (; sift_scope_spool_next(pairs, &a, &b); pa = a, pb = b)
	{
		CHECK_FOR_INTERRUPTS();
		if (!multiset && pm->npairs > 0 && a == pa && b == pb)
			continue;
		if (pm->npairs == 0 || a != pa)
		{
			pm->member_ids[pm->nmembers] = a;
			pm->offsets[pm->nmembers++] = pm->npairs;
		}
		pm->key_ordinals[pm->npairs++] = (uint32) sift_spm_key_ordinal(pm, b);
	}
	pm->offsets[pm->nmembers] = pm->npairs;
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
		if (sc->spec->joined != NULL)
			scan_hops(heap, index, sc->spec, mm, km, snap, &mem, NULL);
		collect(km, false);
		collect(mm, false);
	}
	scan_pairs(b0, rel, m0, mm, k0, km, snap, pairs);
	table_close(rel, NoLock);
	if (sc->spec->joined != NULL)
		scan_hops(heap, index, sc->spec, mm, km, snap, &mem, pairs);
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

/* How many of the keys the pairs give the member */
static uint32
owned_keys(const SiftScopePairMap *pm, int64 member, const int64 *keys, uint32 nkeys)
{
	uint32		c = 0;

	for (uint32 k = 0; k < nkeys; k++)
		c += sift_spm_owns_key(pm, member, keys[k]);
	return c;
}

/* The members of the pairs of the scope keys as the snapshot sees them, ascending, as values of the member column (the codes of a string member decoded), and how many of the keys each has a pair
 * with: the pairs of the declaration, its predicates and both of its paths included */
void
sift_f2s_key_members(Relation heap, Relation index, int ord, Snapshot snap, SiftKeyMap *km, const int64 *keys, uint32 nkeys, Oid member_type, SiftScopeMembers *out)
{
	SiftF2ScopeCfg sc;
	SiftScopePairMap *pm;
	SiftKeyMap *mm;

	sift_f2s_cfg(index, sift_f2_store_open(index), ord, &sc);
	pm = sift_f2s_load_pairs(heap, index, &sc, snap, km, false);
	mm = sift_f2s_mem_map(sc.mem);
	out->member = palloc(Max(pm->nmembers, 1) * sizeof(Datum));
	out->nkeys = palloc(Max(pm->nmembers, 1) * sizeof(uint32));
	out->n = 0;
	for (uint32 i = 0; i < pm->nmembers; i++)
	{
		const uint32 c = owned_keys(pm, pm->member_ids[i], keys, nkeys);

		if (c == 0)
			continue;
		out->member[out->n] = sift_scope_key_datum(member_type, mm, pm->member_ids[i]);
		out->nkeys[out->n++] = c;
	}
	sift_spm_free(pm);
}
