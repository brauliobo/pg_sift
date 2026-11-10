/*
 * col/scan/scope.c - what the reader of a scope index asks of the index and of its scope relation (col/scope.h)
 *
 * A scan that has a scope conjunct reads either the scope segments of the index (the rows replicated by scope key) or, when the verdict says it may not, the entity
 * rows of the members the scope relation lists for the keys: the verdict is made here, the members are read here. The stored read is exact only while the scope
 * segments describe the scope relation as the statement sees it (INTERFACE section 5: the journal of the relation says which members changed since the stamp);
 * until that lands the reader trusts them only under sift.test_scope_trust.
 */
/* check-memory: allow-allocs the arrays follow the keys of one scope conjunct and the pairs of the keys it names, not the rows of the table */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "core/compat.h"
#include "executor/tuptable.h"
#include "utils/rel.h"

#include "col/seg.h"
#include "col/scope.h"
#include "col/wbuf.h"

static int
scope_ordinal(const SiftScopeSpecs *sp, const char *key_name)
{
	for (int i = 0; sp != NULL && i < sp->n; i++)
		if (strcmp(sp->s[i]->key_name, key_name) == 0)
			return i;
	return -1;
}

/* A scope whose relation has no journal index is read as if it vouched under the test switch (the proof of concept); a release never does */
static bool
scope_trusted(Relation index, int scope)
{
#ifdef SIFT_TESTING
	return sift_test_scope_trust && !sift_scope_has_journal(index, scope);
#else
	return false;
#endif
}

/*
 * The stamp the scope segments were filled under for the scope, the oldest when they differ in their horizon (changed(S, X) for the oldest covers the rest); NULL, with *why set, when a
 * segment was never filled (lineage 0) or two were filled under journals of two lineages.
 */
static const char *
scope_stamp(Relation index, const SiftDirSnapshot *dir, int scope, SiftConnStamp *oldest)
{
	bool		first = true;

	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftConnStamp st;

		if (!sift_seg_is_scope(&dir->segs[i]))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		st = scope < seg->nscope ? seg->scope[scope] : (SiftConnStamp) {0};
		sift_cseg_close(seg);
		if (st.lineage == 0)
			return "unfilled";
		if (!first && st.lineage != oldest->lineage)
			return "several stamps";
		if (first || TransactionIdPrecedes(st.xmin, oldest->xmin))
			*oldest = st;
		first = false;
	}
	return NULL;
}

/*
 * The verdict of the journal of the scope relation on the scope segments for the snapshot (INTERFACE section 5): it vouches for their stamp, and no member changed in the relation since
 * it. A stored read is valid only for the members that did not change; until the patch reads those through the entity path, any change is a fallback.
 */
static const char *
scope_verdict(Relation index, const SiftDirSnapshot *dir, int scope, Snapshot snap)
{
	SiftConnStamp stamp;
	SiftConnKeys keys;
	SiftConnWhy why;
	const char *no;

	if ((no = scope_stamp(index, dir, scope, &stamp)) != NULL)
		return no;
	if ((why = sift_scope_vouch(index, scope, &stamp, snap)) != SIFT_CONN_OK)
		return sift_conn_why_name(why);
	if ((why = sift_scope_changed(index, scope, &stamp, snap, sift_scope_patch_keys, &keys)) != SIFT_CONN_OK)
		return sift_conn_why_name(why);
	no = keys.n > 0 ? "members changed" : NULL;
	sift_conn_keys_free(&keys);
	return no;
}

/*
 * NULL: the scope units of the keys are read; else why the statement reads the entity rows (the word EXPLAIN prints after "Scope Fallback"). The reasons come in the
 * order of the contract (INTERFACE section 6): the setting, the declaration, the shape of the request, then the verdict on the segments.
 */
const char *
sift_scope_why(Relation index, const char *key_name, int nkeys, bool once, Snapshot snap)
{
	SiftDirSnapshot *dir;
	const char *why = NULL;
	int			scope = scope_ordinal(sift_scope_specs(index), key_name);
	uint64		first_scope = UINT64_MAX,
				last_entity = 0;

	if (!sift_agg_scope)
		return "disabled";
	if (scope < 0)
		return "no scope index";
	if (once && nkeys > 1)
		return "several keys, row counted once";
	if (!scope_trusted(index, scope) && !sift_scope_has_journal(index, scope))
		return "no journal";
	(void) sift_cwbuf_flush(index);
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
	{
		uint64		rows,
					pages;

		if (sift_seg_is_scope(&dir->segs[i]))
			first_scope = Min(first_scope, dir->segs[i].gen);
		else if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			last_entity = Max(last_entity, dir->segs[i].gen);
		if (!sift_seg_is_wbuf(&dir->segs[i]))
			continue;
		sift_cwbuf_summary(index, &dir->segs[i], &rows, &pages);
		if (rows > 0)
			why = "write buffer";
	}
	if (why == NULL && first_scope == UINT64_MAX)
		why = "unfilled";
	/*
	 * The build writes the scope segments after the entity segments, so a later entity segment (a promotion, a merge) holds rows, or the same rows under a new
	 * generation, that no scope entry describes: the stored read would miss them. Exact and simple until the promotion and the merge fill the scope family.
	 */
	if (why == NULL && last_entity > first_scope)
		why = "scope segments do not cover the rows";
	if (why == NULL && !scope_trusted(index, scope))
		why = scope_verdict(index, dir, scope, snap);
	pfree(dir);
	return why;
}

static int
cmp_i64(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

static int64
slot_key(TupleTableSlot *slot, AttrNumber att, bool *isnull)
{
	Datum		d = slot_getattr(slot, att, isnull);

	switch (TupleDescAttr(slot->tts_tupleDescriptor, att - 1)->atttypid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

/*
 * The member keys that have a pair with one of the scope keys in the scope relation, as the snapshot sees it: ascending, each once. The relation is read whole: the
 * unique btree of the declaration leads with the member, and the members of a scope key are what it cannot list.
 */
int64 *
sift_scope_members(Relation heap, Relation index, const char *key_name, Snapshot snap, const int64 *keys, int nkeys, uint32 *n)
{
	SiftScopeSpecs *sp = sift_scope_specs(index);
	Relation	rel;
	AttrNumber	member,
				skey;
	TupleTableSlot *slot;
	TableScanDesc scan;
	int64	   *sorted = palloc(Max(nkeys, 1) * sizeof(int64)),
			   *out = palloc(64 * sizeof(int64));
	uint32		cap = 64,
				uniq = 0;

	memcpy(sorted, keys, nkeys * sizeof(int64));
	qsort(sorted, nkeys, sizeof(int64), cmp_i64);
	(void) sift_scope_relation_index(heap, index, sp->s[scope_ordinal(sp, key_name)], &rel, &member, &skey);
	slot = table_slot_create(rel, NULL);
	scan = sift_table_beginscan(rel, snap, 0, NULL);
	*n = 0;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		kn,
					mn;
		int64		k = slot_key(slot, skey, &kn),
					m = slot_key(slot, member, &mn);

		if (kn || mn || bsearch(&k, sorted, nkeys, sizeof(int64), cmp_i64) == NULL)
			continue;
		if (*n == cap)
			out = repalloc(out, (cap *= 2) * sizeof(int64));
		out[(*n)++] = m;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	qsort(out, *n, sizeof(int64), cmp_i64);
	for (uint32 i = 0, k = 0; i < *n; i++)
		if (k == 0 || out[i] != out[k - 1])
		{
			out[k++] = out[i];
			uniq = k;
		}
	*n = uniq;
	return out;
}
