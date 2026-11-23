/*
 * col/scope_v6/delta.c - keeps the summary valid while the entity parts change under it: dead rows and pair edits are taken out of (or put into) the
 * cubes of a key when it is read, with no rebuild.
 *
 * When no part has a deletion vector the cube of the key is returned as it is: one question for each part, no granule is looked at. Else the entity
 * hands over the cube of the key's rows that the snapshot does not see, read once for all keys (SiftScopeEntity.gone_cube), and it is subtracted in one step. When the entity keeps
 * too many such rows to share, or the subtraction cannot serve the query, the rows a deletion vector holds are subtracted from the key's cube granule by granule: a granule with few dead rows has only them decoded and subtracted; a granule with
 * more than SIFT_SCOPE_DELTA_GRAN_MAX of its rows or SIFT_SCOPE_DELTA_GRAN_PCT percent is replaced alone, its cube out and its live rows in, and
 * the rest of the cube stays. A minimum, maximum or distinct set that a dead row may have held, a float sum that the subtraction cancels (the
 * whole cube's or a group's), or too many dead rows in the key move the work down to the level-1 nodes: the nodes with no dead row are read as
 * they are, the others are corrected the same way, and one that cannot be corrected is rebuilt from its granules (the clean ones from their
 * cubes, the dirty ones from their live rows).
 * A pair edit adds or subtracts the cubes of the members that changed.
 */
/* check-memory: allow-allocs the arrays follow the granules of one key, not the rows of a table */
#include "core/cmp.h"
#include "core/host.h"

#include "col/scope_v6/int.h"

typedef enum RowMode
{
	ROWS_DEAD,
	ROWS_LIVE,
	ROWS_ALL
} RowMode;

/*
 * The cube of the dead, live or all rows of granule ref that belong to key r in month (SIFT_SCOPE_NO_MONTH: all). In a scope with a null filter
 * the rows it leaves out belong to no cube, whatever the mode. This is also how a ref of SIFT_SCOPE_INLINE_SCAN is read: its rows are its key's.
 */
static void
rows_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *ref, int32 month, const SiftScopeEntity *e, RowMode mode, SiftScopeShape shape, SiftScopeCubeAcc *out)
{
	SiftScopeRows rows;
	const uint8 *dead = NULL;
	const SiftScopeDictMap *map = NULL;
	bool		impure = ref->slice_id != SIFT_SCOPE_PURE_GRANULE;

	memset(&rows, 0, sizeof(rows));
	rows.want_filter = (m->hdr.flags & SIFT_SMETA_NULL_FILTERED) != 0;
	if (mode == ROWS_DEAD)
		e->dead_rows(e->arg, ref->global_granule_idx, &rows, &map);
	else
		e->granule_rows(e->arg, ref->global_granule_idx, &rows, &dead, &map);
	sift_scube_init(out, shape);
	for (uint32 i = 0; i < rows.n; i++)
	{
		if (rows.excluded != NULL && rows.excluded[i])
			continue;
		if (mode == ROWS_LIVE && dead != NULL && dead[i])
			continue;
		if (month != SIFT_SCOPE_NO_MONTH &&
			(rows.time == NULL || (rows.time_null != NULL && rows.time_null[i]) || sift_scope_month_of(rows.time[i]) != month))
			continue;
		if (impure && ((rows.member_null != NULL && rows.member_null[i]) || !e->owns(e->arg, rows.member[i], r->scope_key)))
			continue;
		sift_scube_add_row(out, &rows, i, map);
	}
}

/* What a subtraction left is good for the query: it reads no extremum or distinct set that went, and no float sum cancelled */
static bool
sub_ok(uint32 mask, const SiftScopeQueryNeeds *q)
{
	return !((q->minmax && (mask & SIFT_SUB_BOUNDS)) || (q->distinct && (mask & SIFT_SUB_DSETS)) || (mask & SIFT_SUB_FLOAT));
}

/* acc minus sub; false when the result may be wrong for the query */
bool
sift_scope_cube_take_out(SiftScopeCubeAcc *acc, const SiftScopeCubeAcc *sub, const SiftScopeQueryNeeds *q)
{
	return sub_ok(sift_scube_sub(acc, sub), q);
}

/* The contribution of one granule to the key's cube as the object holds it: its pre-aggregate cube, or the key's slice of it, else its rows */
static void
granule_gross(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *ref, int32 month, const SiftScopeEntity *e, SiftScopeShape shape, SiftScopeCubeAcc *out)
{
	SiftScopeCubeAcc c;
	const SiftScopeDictMap *map = NULL;

	if (ref->slice_id == SIFT_SCOPE_PURE_GRANULE)
	{
		sift_scube_init(out, shape);
		if (e->granule_cube(e->arg, ref->global_granule_idx, month, &c, &map))
		{
			sift_scube_merge(out, &c, map);
			sift_scube_free(&c);
		}
	}
	else if (month == SIFT_SCOPE_NO_MONTH && SIFT_SCOPE_REF_SLICED(ref->slice_id) && sift_scope_slice_cube(m, ref->slice_id, &c))
		*out = c;
	else
		rows_cube(m, r, ref, month, e, ROWS_ALL, shape, out);
}

/* Branch B: the granule alone is replaced in acc, its cube taken out and its live rows put in; false when what is left cannot serve the query */
static bool
replace_granule(SiftScopeCubeAcc *acc, const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *ref, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeDeltaStats *st)
{
	SiftScopeCubeAcc gross,
				live;
	bool		ok;

	granule_gross(m, r, ref, q->month, e, acc->shape, &gross);
	rows_cube(m, r, ref, q->month, e, ROWS_LIVE, acc->shape, &live);
	ok = sift_scope_cube_take_out(acc, &gross, q);
	sift_scube_merge(acc, &live, NULL);
	sift_scube_free(&gross);
	sift_scube_free(&live);
	st->surgical++;
	return ok;
}

static bool
heavy_granule(uint32 ndead, uint32 nrows)
{
	return ndead > SIFT_SCOPE_DELTA_GRAN_MAX || (uint64) ndead * 100 > (uint64) nrows * SIFT_SCOPE_DELTA_GRAN_PCT;
}

/* Takes the dead rows of one granule out of acc: when few, those rows alone; when many, the granule is replaced */
static bool
apply_dead(SiftScopeCubeAcc *acc, const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *ref, uint32 ndead, uint32 nrows, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeDeltaStats *st)
{
	SiftScopeCubeAcc dead;
	bool		ok;

	if (heavy_granule(ndead, nrows))
		return replace_granule(acc, m, r, ref, q, e, st);
	rows_cube(m, r, ref, q->month, e, ROWS_DEAD, acc->shape, &dead);
	ok = dead.row_count == 0 || sift_scope_cube_take_out(acc, &dead, q);
	sift_scube_free(&dead);
	st->subtracted++;
	return ok;
}

/* The contribution of one granule to the key's cube, from the object when nothing is dead and it can be read there, else from the live rows */
static void
granule_contribution(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *ref, uint32 ndead, int32 month, const SiftScopeEntity *e, SiftScopeShape shape, SiftScopeCubeAcc *acc, SiftScopeDeltaStats *st)
{
	SiftScopeCubeAcc c;
	const SiftScopeDictMap *map = NULL;
	bool		pure = ref->slice_id == SIFT_SCOPE_PURE_GRANULE;

	if (ndead == 0 && pure && e->granule_cube(e->arg, ref->global_granule_idx, month, &c, &map))
	{
		sift_scube_merge(acc, &c, map);
		sift_scube_free(&c);
		return;
	}
	if (ndead == 0 && SIFT_SCOPE_REF_SLICED(ref->slice_id) && month == SIFT_SCOPE_NO_MONTH && sift_scope_slice_cube(m, ref->slice_id, &c))
	{
		sift_scube_merge(acc, &c, NULL);
		sift_scube_free(&c);
		return;
	}
	rows_cube(m, r, ref, month, e, ROWS_LIVE, shape, &c);
	st->granule_scans++;
	sift_scube_merge(acc, &c, NULL);
	sift_scube_free(&c);
}

/* The level-1 node of a key with its dead rows out: from its cube when few, else rebuilt from its granules. false: the node has no row in the month */
static bool
node_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeL1Node *nd, const SiftScopeGranRef *refs, const uint32 *dc, const uint32 *nr, uint64 ndead, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *out, SiftScopeDeltaStats *st)
{
	bool		ok = ndead <= SIFT_SCOPE_DELTA_NODE_MAX;

	if (!sift_scope_node_cube(m, nd, q->month, out))
		return false;
	for (uint32 i = 0; ok && i < nd->nrefs; i++)
		if (dc[i] > 0)
			ok = apply_dead(out, m, r, &refs[i], dc[i], nr[i], q, e, st);
	if (!ok)
	{
		SiftScopeCubeAcc rebuilt;

		sift_scube_init(&rebuilt, out->shape);
		for (uint32 i = 0; i < nd->nrefs; i++)
			granule_contribution(m, r, &refs[i], dc[i], q->month, e, out->shape, &rebuilt, st);
		sift_scube_free(out);
		*out = rebuilt;
	}
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

/* The index of the part that holds a granule index */
static uint32
part_of(const SiftScopeMeta *m, uint32 gidx)
{
	uint32		lo = 0,
				hi = m->hdr.nparts;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (m->parts[mid].base_granule_idx + m->parts[mid].ngranules <= gidx)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Which parts have a deletion vector: one question for each, none for a retired part; false when none has */
static bool
dirty_parts(const SiftScopeMeta *m, const SiftScopeEntity *e, bool *dirty)
{
	bool		any = false;

	for (uint32 p = 0; p < m->hdr.nparts; p++)
		any |= dirty[p] = m->parts[p].part_gen != 0 && e->part_dirty(e->arg, m->parts[p].part_gen);
	return any;
}

/* The sum of the key's level-1 nodes, each with its dead rows out: the nodes that have none are read as they are */
static void
sum_nodes(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeGranRef *refs, const uint32 *dc, const uint32 *nr, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *sum, SiftScopeDeltaStats *st)
{
	uint32		at = 0;

	for (uint32 n = 0; n < r->l1_count; n++)
	{
		const SiftScopeL1Node *nd = &m->nodes[r->l1_first + n];
		SiftScopeCubeAcc nc;
		uint64		nd_dead = 0;

		for (uint32 i = 0; i < nd->nrefs; i++)
			nd_dead += dc[at + i];
		if (nd_dead == 0 ? sift_scope_node_cube(m, nd, q->month, &nc) : node_live(m, r, nd, refs + at, dc + at, nr + at, nd_dead, q, e, &nc, st))
		{
			sift_scube_merge(sum, &nc, NULL);
			sift_scube_free(&nc);
		}
		if (nd_dead > 0)
			st->node_descents++;
		at += nd->nrefs;
	}
}

/*
 * The rows of the key that the object counts and the snapshot does not see, taken out of out in one subtraction: the entity read them once for all keys. false: it keeps too many of them to
 * share, or what the subtraction leaves cannot serve the query; out is the cube of the key again, for the key's granules to be asked for theirs.
 */
static bool
gone_taken_out(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *out, SiftScopeDeltaStats *st)
{
	SiftScopeCubeAcc gone;
	const SiftScopeGone what = e->gone_cube != NULL ? e->gone_cube(e->arg, r->scope_key, q->month, &gone) : SIFT_GONE_MANY;

	if (what == SIFT_GONE_NONE)
		return true;
	if (what == SIFT_GONE_ROWS)
	{
		const bool	ok = sift_scope_cube_take_out(out, &gone, q);

		st->dead_total = gone.row_count;
		sift_scube_free(&gone);
		if (ok)
			return true;
		sift_scube_free(out);
		(void) sift_scope_key_cube(m, r, q->month, out);
	}
	return false;
}

static bool
object_cube_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *out, SiftScopeDeltaStats *st)
{
	bool	   *dirty = palloc(sizeof(bool) * Max(m->hdr.nparts, 1));
	SiftScopeDeltaStats local = {0};
	uint32		nrefs;
	SiftScopeGranRef *refs;
	uint32	   *dc,
			   *nr;
	uint64		total = 0;
	bool		ok = true;

	st = st != NULL ? st : &local;
	memset(st, 0, sizeof(*st));
	if (!sift_scope_key_cube(m, r, q->month, out))
	{
		pfree(dirty);
		return false;
	}
	st->clean = !dirty_parts(m, e, dirty);
	if (st->clean || gone_taken_out(m, r, q, e, out, st))
	{
		pfree(dirty);
		return out->row_count > 0;
	}
	nrefs = sift_scope_key_refs(m, r, NULL);
	refs = palloc(sizeof(*refs) * Max(nrefs, 1));
	dc = palloc0(sizeof(uint32) * Max(nrefs, 1));
	nr = palloc0(sizeof(uint32) * Max(nrefs, 1));
	sift_scope_key_refs(m, r, refs);
	for (uint32 i = 0; i < nrefs; i++)
		if (dirty[part_of(m, refs[i].global_granule_idx)])
			total += dc[i] = e->dead_count(e->arg, refs[i].global_granule_idx, &nr[i]);
	st->dead_total = total;
	if (total > 0 && total <= SIFT_SCOPE_DELTA_ROOT_MAX)
		for (uint32 i = 0; ok && i < nrefs; i++)
			if (dc[i] > 0)
				ok = apply_dead(out, m, r, &refs[i], dc[i], nr[i], q, e, st);
	if (total > SIFT_SCOPE_DELTA_ROOT_MAX || !ok)
	{
		SiftScopeCubeAcc sum;

		sift_scube_init(&sum, out->shape);
		sift_scube_free(out);
		memset(st, 0, sizeof(*st));
		st->dead_total = total;
		sum_nodes(m, r, refs, dc, nr, q, e, &sum, st);
		*out = sum;
	}
	pfree(refs);
	pfree(dc);
	pfree(nr);
	pfree(dirty);
	return out->row_count > 0;
}

/* The cube of the object less its dead rows, plus the rows the object lacks that the snapshot sees */
bool
sift_scope_cube_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *out, SiftScopeDeltaStats *st)
{
	SiftScopeCubeAcc added;
	bool		have = object_cube_live(m, r, q, e, out, st);

	if (e->added_cube == NULL || !e->added_cube(e->arg, r->scope_key, sift_scope_span_month(q->month), &added))
		return have;
	if (have)
	{
		sift_scube_merge(out, &added, NULL);
		sift_scube_free(&added);
	}
	else
		*out = added;
	return true;
}

uint32
sift_scope_months_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeEntity *e, int32 **months)
{
	const uint32 nm = sift_scope_monthmap_nmonths(m, r);
	const int32 *more = NULL;
	const uint32 nmore = e->added_months != NULL ? e->added_months(e->arg, r->scope_key, &more) : 0;
	SiftScopeMonthBucket *bk = palloc(Max(nm, 1) * sizeof(SiftScopeMonthBucket));
	int32	   *all = palloc(Max(nm + nmore, 1) * sizeof(int32));
	uint32		n = 0,
				u = 0;

	(void) sift_scope_monthmap_buckets(m, r, bk, nm);
	for (uint32 i = 0; i < nm; i++)
		all[n++] = bk[i].month_id;
	for (uint32 i = 0; i < nmore; i++)
		all[n++] = more[i];
	qsort(all, n, sizeof(int32), sift_cmp_i32);
	for (uint32 i = 0; i < n; i++)
		if (u == 0 || all[u - 1] != all[i])
			all[u++] = all[i];
	pfree(bk);
	*months = all;
	return u;
}

/* The cubes of members (ascending, distinct): one pass of the entity when it has one, else a call for each; has[i] says a member has rows (out[i] is freed when it has none) */
static void
members_cubes(const SiftScopeEntity *e, const int64 *members, uint32 n, SiftScopeSpan span, SiftScopeCubeAcc *out, bool *has)
{
	if (e->members_cubes != NULL)
	{
		e->members_cubes(e->arg, members, n, span, out, has);
		return;
	}
	for (uint32 i = 0; i < n; i++)
		has[i] = e->member_cube(e->arg, members[i], span, &out[i]);
}

/* The cubes of the members of two lists as one batch: the distinct members in ascending order, cubes[i] belongs to all[i] */
static uint32
two_lists_cubes(const SiftScopeEntity *e, SiftScopeSpan span, const int64 *a, uint32 na, const int64 *b, uint32 nb, int64 **all, SiftScopeCubeAcc **cubes, bool **has)
{
	uint32		n = 0;

	*all = palloc(sizeof(int64) * Max(na + nb, 1));
	memcpy(*all, a, sizeof(int64) * na);
	memcpy(*all + na, b, sizeof(int64) * nb);
	qsort(*all, na + nb, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < na + nb; i++)
		if (n == 0 || (*all)[n - 1] != (*all)[i])
			(*all)[n++] = (*all)[i];
	*cubes = palloc(sizeof(SiftScopeCubeAcc) * Max(n, 1));
	*has = palloc(Max(n, 1));
	members_cubes(e, *all, n, span, *cubes, *has);
	return n;
}

static uint32
batch_at(const int64 *all, uint32 n, int64 member)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (all[mid] < member)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

SiftScopePatch
sift_scope_cube_patch(SiftScopeCubeAcc *acc, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const int64 *added, uint32 nadded, const int64 *removed, uint32 nremoved)
{
	int64	   *all;
	SiftScopeCubeAcc *cubes;
	bool	   *has;
	uint32		n;
	SiftScopePatch rc = SIFT_PATCH_OK;

	SIFT_INVARIANT(nadded + nremoved <= SIFT_SCOPE_PATCH_MEMBERS, "scope patch: more changed members than the fast path takes");
	n = two_lists_cubes(e, sift_scope_span_month(q->month), added, nadded, removed, nremoved, &all, &cubes, &has);
	for (uint32 i = 0; i < nremoved && rc == SIFT_PATCH_OK; i++)
	{
		const uint32 at = batch_at(all, n, removed[i]);

		if (has[at] && !sift_scope_cube_take_out(acc, &cubes[at], q))
			rc = SIFT_PATCH_RESCAN;
	}
	for (uint32 i = 0; i < nadded && rc == SIFT_PATCH_OK; i++)
		if (has[batch_at(all, n, added[i])])
			sift_scube_merge(acc, &cubes[batch_at(all, n, added[i])], NULL);
	for (uint32 i = 0; i < n; i++)
		if (has[i])
			sift_scube_free(&cubes[i]);
	pfree(all);
	pfree(cubes);
	pfree(has);
	return rc;
}

typedef struct MemberMult
{
	int64		member;
	uint32		mult;
} MemberMult;

static int
member_mult_cmp(const void *a, const void *b)
{
	return sift_cmp_i64(&((const MemberMult *) a)->member, &((const MemberMult *) b)->member);
}

SiftScopePatch
sift_scope_cube_dedupe(SiftScopeCubeAcc *acc, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const int64 *members, const uint32 *mult, uint32 n)
{
	MemberMult *dup = palloc(sizeof(*dup) * Max(n, 1));
	int64	   *ids = palloc(sizeof(int64) * Max(n, 1));
	SiftScopeCubeAcc *cubes = palloc(sizeof(SiftScopeCubeAcc) * Max(n, 1));
	bool	   *has = palloc(Max(n, 1));
	uint32		nd = 0;
	SiftScopePatch rc = SIFT_PATCH_OK;

	for (uint32 i = 0; i < n; i++)
		if (mult[i] >= 2)
			dup[nd++] = (MemberMult) {members[i], mult[i]};
	qsort(dup, nd, sizeof(*dup), member_mult_cmp);
	for (uint32 i = 0; i < nd; i++)
		ids[i] = dup[i].member;
	members_cubes(e, ids, nd, sift_scope_span_month(q->month), cubes, has);
	for (uint32 i = 0; i < nd; i++)
	{
		for (uint32 k = 1; has[i] && rc == SIFT_PATCH_OK && k < dup[i].mult; k++)
			if (!sift_scope_cube_take_out(acc, &cubes[i], q))
				rc = SIFT_PATCH_RESCAN;
		if (has[i])
			sift_scube_free(&cubes[i]);
	}
	pfree(dup);
	pfree(ids);
	pfree(cubes);
	pfree(has);
	return rc;
}

void
sift_scope_cube_untouched(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, const int64 *changed, uint32 nchanged, SiftScopeCubeAcc *out, bool *touched)
{
	for (uint32 n = 0; n < r->l1_count; n++)
	{
		const SiftScopeL1Node *nd = &m->nodes[r->l1_first + n];
		SiftScopeCubeAcc c;

		touched[n] = false;
		for (uint32 i = 0; i < nchanged && !touched[n]; i++)
			touched[n] = sift_scope_node_has_member(m, nd, changed[i]);
		if (!touched[n] && sift_scope_node_cube(m, nd, month, &c))
		{
			sift_scube_merge(out, &c, NULL);
			sift_scube_free(&c);
		}
	}
}

uint32
sift_scope_excl_union(const uint16 *a, uint32 na, const uint16 *b, uint32 nb, uint16 *out, uint32 cap)
{
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		uint16		v = j == nb || (i < na && a[i] <= b[j]) ? a[i] : b[j];

		if (n == cap)
			return cap + 1;
		out[n++] = v;
		i += i < na && a[i] == v;
		j += j < nb && b[j] == v;
	}
	return n;
}
