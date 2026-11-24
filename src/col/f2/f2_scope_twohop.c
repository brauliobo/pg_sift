/*
 * col/f2/f2_scope_twohop.c - the pairs (member, scope key) of a scope of format major 2, read from the btrees its declaration records, as the snapshot sees them: the pairs relation itself, and for a
 * scope of two paths the union of those and of the members of the groups the joined path gives each key (the joined relation holds (group, key), the via relation (group, member)). Within
 * maintenance_work_mem.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"

#include "col/f2/f2_scope_int.h"
#include "col/scope.h"
#include "core/memtrack.h"

typedef struct Pairs
{
	SiftScopeMemberKey *v;
	uint32		n;
	uint32		cap;
} Pairs;

bool
sift_f2s_int_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID;
}

static int
pair_order(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static void
pairs_add(SiftMemTracker *t, Pairs *p, int64 member, int64 key)
{
	if (p->n == p->cap)
	{
		const uint32 ncap = Min(Max(1024, p->cap * 2), MaxAllocSize / sizeof(SiftScopeMemberKey));

		if (ncap == p->cap || sift_mem_would_exceed(t, (Size) (ncap - p->cap) * sizeof(SiftScopeMemberKey)))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the pairs of the scope do not fit in maintenance_work_mem"), errhint("Raise maintenance_work_mem.")));
		p->v = p->v ? repalloc(p->v, (Size) ncap * sizeof(SiftScopeMemberKey)) : sift_mem_alloc(t, t->root_cx, (Size) ncap * sizeof(SiftScopeMemberKey));
		p->cap = ncap;
	}
	p->v[p->n++] = (SiftScopeMemberKey) {member, key};
}

/* The rows (a, b) of the btree of rel under snap: the pairs when a is the member and b the key; the (group, key) or (group, member) rows of a hop. A string column is read as the codes of its map; out NULL: the rows are only met, so that a writer's map holds every key */
static void
scan_pairs(Oid btree, Relation rel, AttrNumber a, SiftKeyMap *ma, AttrNumber b, SiftKeyMap *mb, Snapshot snap, SiftMemTracker *mem, Pairs *out)
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
				pairs_add(mem, out, code_a, code_b);
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(ix, NoLock);
}

static uint32
first_of(const SiftScopeMemberKey *v, uint32 n, int64 group)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (v[mid].member < group)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static uint32
sort_unique(SiftScopeMemberKey *v, uint32 n)
{
	uint32		u = 0;

	qsort(v, n, sizeof(*v), pair_order);
	for (uint32 i = 0; i < n; i++)
		if (u == 0 || pair_order(&v[u - 1], &v[i]) != 0)
			v[u++] = v[i];
	return u;
}

/* The members of each group the joined path gives a key: the (group, member) rows of the via relation that meet the sorted (group, key) rows */
static void
join_via(const Pairs *groups, Oid btree, Relation via, AttrNumber group, AttrNumber member, Snapshot snap, SiftMemTracker *mem, Pairs *out)
{
	Pairs		hop = {0};

	scan_pairs(btree, via, group, NULL, member, NULL, snap, mem, &hop);
	for (uint32 i = 0; i < hop.n; i++)
		for (uint32 q = first_of(groups->v, groups->n, hop.v[i].member); q < groups->n && groups->v[q].member == hop.v[i].member; q++)
			pairs_add(mem, out, hop.v[i].key, groups->v[q].key);
}

/* The map of the codes of the scope key of the pairs relation: NULL for an integer key; a writer's interns the keys the store lacks, all of them as one run (the pass that meets them comes first) */
SiftKeyMap *
sift_f2s_key_map(Relation heap, Relation index, const SiftScopeSpec *spec, bool write)
{
	Relation	rel;
	AttrNumber	member,
				key;
	SiftKeyMap *km;

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
						errhint("Format major 2 summarizes scopes whose member is an integer column and whose key is an integer, text, varchar or uuid column.")));
}

SiftScopePairMap *
sift_f2s_load_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, bool intern)
{
	Pairs		pairs = {0};
	SiftMemTracker mem;
	Relation	rel;
	AttrNumber	m0,
				k0;
	const Oid	b0 = sift_scope_relation_index(heap, index, sc->spec, &rel, &m0, &k0);
	SiftScopePairMap *pm;

	check_pair_types(rel, m0, k0, sc->spec->key_name);
	sift_mem_init(&mem, "scope pairs", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 1, 1);
	if (intern && km != NULL)
	{
		sift_keymap_collect(km, true);
		scan_pairs(b0, rel, m0, NULL, k0, km, snap, &mem, NULL);
		sift_keymap_collect(km, false);
	}
	scan_pairs(b0, rel, m0, NULL, k0, km, snap, &mem, &pairs);
	table_close(rel, NoLock);
	if (sc->spec->joined != NULL)
	{
		Pairs		groups = {0};
		Relation	joined,
					via;
		AttrNumber	g1,
					k1,
					g2,
					m2;
		const Oid	b1 = sift_scope_hop_index(heap, index, sc->spec, 1, &joined, &g1, &k1),
					b2 = sift_scope_hop_index(heap, index, sc->spec, 2, &via, &g2, &m2);

		scan_pairs(b1, joined, g1, NULL, k1, km, snap, &mem, &groups);
		groups.n = sort_unique(groups.v, groups.n);
		join_via(&groups, b2, via, g2, m2, snap, &mem, &pairs);
		table_close(via, NoLock);
		table_close(joined, NoLock);
	}
	pm = sift_spm_build(pairs.v, pairs.n, sift_scope_pairs_multiset(index, sc->spec));
	sift_mem_end(&mem);
	return pm;
}
