/*
 * col/scope_v6/overlay.c - the live read of a summary under pairs that changed since it was written: the cube of a key, the rows no cube holds folded in, and the rows at the edge of a time
 * window.
 *
 * A pair edit moves a member into or out of a key. The summary cannot say who was in the key before (a level-1 node keeps its members as a Bloom filter or, when few, as a list), so a member is
 * told added, removed or unchanged only when no node, or a node with a list, answers for it (classify); a Bloom filter that says "maybe" makes the edit ambiguous and the read goes to the join.
 */
/* check-memory: allow-allocs the arrays follow the members of one key's edit and the granules of one key */
#include "core/host.h"

#include "core/cmp.h"
#include "col/scope_v6/overlay.h"

typedef enum OldMember
{
	OLD_NO,
	OLD_YES,
	OLD_MAYBE
} OldMember;

/* A node with a list of its members (no Bloom filter) answers exactly; a Bloom filter or a bare range only for "no" */
static OldMember
old_member(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 member)
{
	OldMember	seen = OLD_NO;

	for (uint32 i = 0; r != NULL && i < r->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &m->nodes[r->l1_first + i];

		if (!sift_scope_node_has_member(m, nd, member))
			continue;
		if (nd->bloom_len > 0 && !(nd->flags & SIFT_L1_HAS_BLOOM))
			return OLD_YES;
		seen = OLD_MAYBE;
	}
	return seen;
}

void
sift_scope_overlay_classify(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 key, const SiftScopeEntity *e, const int64 *changed, const bool *kept, uint32 n, SiftScopeEdit *out)
{
	*out = (SiftScopeEdit) {.exact = true};
	out->changed = palloc(sizeof(int64) * Max(n, 1));
	out->own = palloc(sizeof(int64) * Max(n, 1));
	out->added = palloc(sizeof(int64) * Max(n, 1));
	out->removed = palloc(sizeof(int64) * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		bool		own;
		OldMember	was;

		if (kept != NULL && kept[i])
			continue;
		own = e->owns(e->arg, changed[i], key);
		was = old_member(m, r, changed[i]);
		out->changed[out->nchanged++] = changed[i];
		if (own)
			out->own[out->nown++] = changed[i];
		if (was == OLD_MAYBE)
			out->exact = false;
		else if (was == OLD_NO && own)
			out->added[out->nadded++] = changed[i];
		else if (was == OLD_YES && !own)
			out->removed[out->nremoved++] = changed[i];
	}
}

void
sift_scope_edit_free(SiftScopeEdit *ed)
{
	pfree(ed->changed);
	pfree(ed->own);
	pfree(ed->added);
	pfree(ed->removed);
	*ed = (SiftScopeEdit) {0};
}

static const int64 *
find_member(const int64 *v, uint32 n, int64 x)
{
	return n > 0 ? bsearch(&x, v, n, sizeof(int64), sift_cmp_i64) : NULL;
}

static bool
node_touched(const SiftScopeMeta *m, const SiftScopeL1Node *nd, const SiftScopeEdit *ed)
{
	return sift_scope_node_hits_any(m, nd, ed->changed, ed->nchanged);
}

static bool
node_has_dead(const SiftScopeMeta *m, const SiftScopeL1Node *nd, const SiftScopeEntity *e)
{
	SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
	const uint32 n = sift_scope_node_refs(m, nd, refs);

	for (uint32 i = 0; i < n; i++)
	{
		uint64		gen;
		uint32		id,
					nrows;

		if (sift_scope_gidx_part(m, refs[i].global_granule_idx, &gen, &id) && e->part_dirty(e->arg, gen) && e->dead_count(e->arg, refs[i].global_granule_idx, &nrows) > 0)
			return true;
	}
	return false;
}

static bool
row_wanted(const SiftScopeRows *rows, uint32 i, const uint8 *dead, int32 month)
{
	if ((dead != NULL && dead[i]) || (rows->excluded != NULL && rows->excluded[i]) || (rows->member_null != NULL && rows->member_null[i]))
		return false;
	return month == SIFT_SCOPE_NO_MONTH || (rows->time != NULL && !(rows->time_null != NULL && rows->time_null[i]) && sift_scope_month_of(rows->time[i]) == month);
}

/* The live rows of the granules of a node that the key owns under the pairs as they are; those of the members in own (ascending) are also counted apart, for each member */
static void
scan_node(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeL1Node *nd, int32 month, const SiftScopeEntity *e, const SiftScopeEdit *ed, SiftScopeCubeAcc *own_rows, SiftScopeCubeAcc *sum)
{
	SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
	const uint32 n = sift_scope_node_refs(m, nd, refs);

	for (uint32 g = 0; g < n; g++)
	{
		SiftScopeRows rows;
		const uint8 *dead = NULL;
		const SiftScopeDictMap *map = NULL;

		memset(&rows, 0, sizeof(rows));
		rows.want_filter = (m->hdr.flags & SIFT_SMETA_NULL_FILTERED) != 0;
		e->granule_rows(e->arg, refs[g].global_granule_idx, &rows, &dead, &map);
		for (uint32 i = 0; i < rows.n; i++)
		{
			const int64 *at;

			if (!row_wanted(&rows, i, dead, month) || !e->owns(e->arg, rows.member[i], r->scope_key))
				continue;
			sift_scube_add_row(sum, &rows, i, map);
			at = find_member(ed->own, ed->nown, rows.member[i]);
			if (at != NULL)
				sift_scube_add_row(&own_rows[at - ed->own], &rows, i, map);
		}
	}
}

/* The rows of a member the key holds now that no scanned node counted: all its rows less the ones the scan counted (equal counts: none, whatever the bounds say) */
static bool
outside_rows(const SiftScopeEntity *e, int64 member, int32 month, const SiftScopeCubeAcc *counted, bool minmax, SiftScopeCubeAcc *sum)
{
	SiftScopeCubeAcc all;
	bool		ok = true;

	if (!e->member_cube(e->arg, member, sift_scope_span_month(month), &all))
		return true;
	if (all.row_count > counted->row_count)
	{
		const uint32 lost = sift_scube_sub(&all, counted);

		ok = !((minmax && (lost & SIFT_SUB_BOUNDS)) || (lost & SIFT_SUB_FLOAT));
		sift_scube_merge(sum, &all, NULL);
	}
	sift_scube_free(&all);
	return ok;
}

static bool
nodes_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeShape shape, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const SiftScopeEdit *ed, SiftScopeCubeAcc *out)
{
	SiftScopeCubeAcc *own_rows = palloc(sizeof(SiftScopeCubeAcc) * Max(ed->nown, 1));
	bool		ok = true;

	sift_scube_init(out, shape);
	for (uint32 j = 0; j < ed->nown; j++)
		sift_scube_init(&own_rows[j], shape);
	for (uint32 i = 0; r != NULL && i < r->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &m->nodes[r->l1_first + i];
		SiftScopeCubeAcc c;

		if (node_touched(m, nd, ed) || node_has_dead(m, nd, e))
			scan_node(m, r, nd, q->month, e, ed, own_rows, out);
		else if (sift_scope_node_cube(m, nd, q->month, &c))
		{
			sift_scube_merge(out, &c, NULL);
			sift_scube_free(&c);
		}
	}
	for (uint32 j = 0; j < ed->nown; j++)
	{
		ok = outside_rows(e, ed->own[j], q->month, &own_rows[j], q->minmax, out) && ok;
		sift_scube_free(&own_rows[j]);
	}
	pfree(own_rows);
	return ok;
}

/* No part has a deletion vector or a pending delete: the cubes of the summary count every row they say */
static bool
parts_clean(const SiftScopeMeta *m, const SiftScopeEntity *e)
{
	for (uint32 p = 0; p < m->hdr.nparts; p++)
		if (m->parts[p].part_gen != 0 && e->part_dirty(e->arg, m->parts[p].part_gen))
			return false;
	return true;
}

/*
 * Dead rows are taken out of a key's cube by the pairs the key had when it was written (a granule that mixes keys gives the rows of the key); after an edit the pairs are not those, so a key with dead
 * rows and edited pairs is never read as the cube less its dead rows plus a patch: its nodes are read from their live rows under the pairs as they are.
 */
bool
sift_scope_overlay_key_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeShape shape, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const SiftScopeEdit *ed, SiftScopeCubeAcc *out, bool *exact)
{
	const uint32 nedit = ed->nadded + ed->nremoved;
	bool		have;

	*exact = true;
	if (ed->nchanged == 0)
	{
		if (r != NULL)
			return sift_scope_cube_live(m, r, q, e, out, NULL);
		sift_scube_init(out, shape);
		sift_scube_free(out);
		return false;
	}
	if (ed->exact && nedit <= SIFT_SCOPE_PATCH_MEMBERS && (r == NULL || parts_clean(m, e)))
	{
		have = r != NULL && sift_scope_key_cube(m, r, q->month, out);
		if (!have)
			sift_scube_init(out, shape);
		if (sift_scope_cube_patch(out, q, e, ed->added, ed->nadded, ed->removed, ed->nremoved) == SIFT_PATCH_OK)
		{
			have = out->row_count > 0;
			if (!have)
				sift_scube_free(out);
			return have;
		}
		sift_scube_free(out);
	}
	*exact = nodes_cube(m, r, shape, q, e, ed, out);
	have = out->row_count > 0;
	if (!have)
		sift_scube_free(out);
	return have;
}

uint32
sift_scope_overlay_fold(SiftScopeCubeAcc *acc, const SiftScopeRows *r, const uint8 *skip, const SiftScopeEntity *e, const SiftScopeFold *f)
{
	const bool	windowed = f->lo != PG_INT64_MIN || f->hi != PG_INT64_MAX || f->month != SIFT_SCOPE_NO_MONTH;
	uint32		taken = 0;

	for (uint32 i = 0; i < r->n; i++)
	{
		uint32		owners = 0;

		if ((skip != NULL && skip[i]) || (r->member_null != NULL && r->member_null[i]))
			continue;
		if (windowed)
		{
			if (r->time == NULL || (r->time_null != NULL && r->time_null[i]) || r->time[i] < f->lo || (f->hi != PG_INT64_MAX && r->time[i] >= f->hi) ||
				(f->month != SIFT_SCOPE_NO_MONTH && sift_scope_month_of(r->time[i]) != f->month))
				continue;
		}
		if (e->owns_count != NULL)
			owners = e->owns_count(e->arg, r->member[i], f->keys, f->nkeys, f->semi);
		else
			for (uint32 k = 0; k < f->nkeys; k++)
				owners += e->owns(e->arg, r->member[i], f->keys[k]);
		for (uint32 k = 0; k < (f->semi ? Min(owners, 1) : owners); k++)
			sift_scube_add_row(acc, r, i, NULL);
		taken += owners > 0;
	}
	return taken;
}

static bool
strides_overlap(const SiftScopeEntity *e, uint32 gidx, const SiftScopeSpan *span)
{
	const SiftTimeStride *st;
	const uint32 n = e->strides(e->arg, gidx, &st);

	for (uint32 i = 0; i < n; i++)
		if (st[i].time_max >= span->lo && st[i].time_min < span->hi)
			return true;
	return n == 0;
}

/* The ordinals among the nrefs granules of the key (ascending) that hold rows of the months the span touches */
static uint32
span_granules(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeSpan *span, uint32 nrefs, uint32 **out)
{
	const uint32 cap = Max(sift_scope_monthmap_ngranules(m, r), 1);
	uint32	   *ords = palloc(sizeof(uint32) * cap);
	uint8	   *held = palloc0(nrefs);
	uint32	   *want = palloc(sizeof(uint32) * nrefs);
	uint32		n = 0;

	for (int32 mo = sift_scope_month_of(span->lo); mo <= sift_scope_month_of(span->hi - 1); mo++)
	{
		const uint32 k = sift_scope_monthmap_granules(m, r, mo, ords, cap);

		for (uint32 i = 0; i < k; i++)
			held[ords[i]] = 1;
	}
	for (uint32 o = 0; o < nrefs; o++)
		if (held[o])
			want[n++] = o;
	pfree(ords);
	pfree(held);
	*out = want;
	return n;
}

/* The rows of the granules of key r whose time is in the span, that the key owns under the pairs as they are, but those of the members in skip (ascending), added to acc; the granules read */
static uint32
key_granules_slice(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeEntity *e, const SiftScopeSpan *span, const int64 *skip, uint32 nskip, SiftScopeCubeAcc *acc)
{
	const uint32 nrefs = sift_scope_key_refs(m, r, NULL);
	SiftScopeGranRef *refs;
	uint32	   *want;
	uint32		nwant,
				read = 0;

	if (nrefs == 0)
		return 0;
	refs = palloc(sizeof(*refs) * nrefs);
	sift_scope_key_refs(m, r, refs);
	nwant = span_granules(m, r, span, nrefs, &want);
	for (uint32 w = 0; w < nwant; w++)
	{
		SiftScopeRows rows;
		const uint8 *dead = NULL;
		const SiftScopeDictMap *map = NULL;
		const uint32 gidx = refs[want[w]].global_granule_idx;

		if (!strides_overlap(e, gidx, span))
			continue;
		memset(&rows, 0, sizeof(rows));
		rows.want_filter = (m->hdr.flags & SIFT_SMETA_NULL_FILTERED) != 0;
		e->granule_rows(e->arg, gidx, &rows, &dead, &map);
		read++;
		for (uint32 i = 0; i < rows.n; i++)
			if (row_wanted(&rows, i, dead, SIFT_SCOPE_NO_MONTH) && rows.time != NULL && sift_scope_span_has(span, rows.time[i], rows.time_null != NULL && rows.time_null[i]) &&
				find_member(skip, nskip, rows.member[i]) == NULL && e->owns(e->arg, rows.member[i], r->scope_key))
				sift_scube_add_row(acc, &rows, i, map);
	}
	pfree(refs);
	pfree(want);
	return read;
}

static void
fold_cube(SiftScopeCubeAcc *acc, SiftScopeCubeAcc *c)
{
	sift_scube_merge(acc, c, NULL);
	sift_scube_free(c);
}

/* The rows in the span of the members the key owns now whose pairs changed: they may be in granules the key did not have, so all the granules that can hold them are read */
static void
changed_members_slice(const SiftScopeEntity *e, const int64 *own, uint32 nown, const SiftScopeSpan *span, SiftScopeCubeAcc *acc)
{
	SiftScopeCubeAcc *cubes = palloc(sizeof(SiftScopeCubeAcc) * nown);
	bool	   *has = palloc(nown);

	if (e->members_cubes != NULL)
		e->members_cubes(e->arg, own, nown, *span, cubes, has);
	else
		for (uint32 i = 0; i < nown; i++)
			has[i] = e->member_cube(e->arg, own[i], *span, &cubes[i]);
	for (uint32 i = 0; i < nown; i++)
		if (has[i])
			fold_cube(acc, &cubes[i]);
	pfree(cubes);
	pfree(has);
}

uint32
sift_scope_overlay_slice(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 key, const SiftScopeEntity *e, const int64 *changed, uint32 nchanged, SiftScopeSpan span, SiftScopeCubeAcc *acc)
{
	int64	   *own;
	uint32		nown = 0,
				read = 0;
	SiftScopeCubeAcc added;

	SIFT_INVARIANT(!span.all, "scope slice: a span of the time leaf");
	if (span.lo >= span.hi)
		return 0;
	own = palloc(sizeof(int64) * Max(nchanged, 1));
	for (uint32 i = 0; i < nchanged; i++)
		if (e->owns(e->arg, changed[i], key))
			own[nown++] = changed[i];
	if (r != NULL)
		read = key_granules_slice(m, r, e, &span, own, nown, acc);
	if (nown > 0)
		changed_members_slice(e, own, nown, &span, acc);
	if (e->added_cube != NULL && e->added_cube(e->arg, key, span, &added))
		fold_cube(acc, &added);
	pfree(own);
	return read;
}
