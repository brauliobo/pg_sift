/*
 * col/f2/scope/delta.c - the cube of a scope key from a summary that the pairs have moved under (f2_scope.h): the members the journal says changed for the key are told added, removed or
 * unchanged against the summary (col/f2/cube/overlay.c), their rows read from the entity parts under the pairs as they are now, and the rows the summary does not hold (slabs, parts it does
 * not name) that the key owns now are added from the entity side of the summary (f2_scope_over.c). The rows of the named parts that the snapshot does not see are out of every read of them.
 */
#include "postgres.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/snap.h"

SiftScopeServeMode
sift_f2_scope_mode(const SiftF2Scope *sc)
{
	const SiftF2ScopeParts *p = sc->entity.arg;
	bool		dirty = sc->nchanged > 0 || p->added != NULL;

	for (int i = 0; !dirty && i < p->n; i++)
		dirty = p->s[i].part_gen != 0 && sift_f2s_slot_dirty(p, &p->s[i]);
	return dirty ? SIFT_SERVE_DELTA_CUBE : SIFT_SERVE_CLEAN_CUBE;
}

/* The cube of the key under the pairs as they are now: the summary's with the rows the snapshot does not see out and those it lacks in */
static bool
key_cube_compute(SiftF2Scope *sc, const SiftScopeL2Root *r, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed, SiftScopeCubeAcc *out, bool *exact)
{
	const SiftScopeShape shape = ((const SiftF2ScopeParts *) sc->entity.arg)->sc->shape;
	SiftScopeCubeAcc added;
	bool		has = sift_scope_overlay_key_cube(&sc->meta, r, shape, q, &sc->entity, ed, out, exact);

	if (!*exact)
	{
		if (has)
			sift_scube_free(out);
		return false;
	}
	if (!has)
		sift_scube_init(out, shape);
	if (sc->entity.added_cube != NULL && sc->entity.added_cube(sc->entity.arg, key, sift_scope_span_month(q->month), &added))
	{
		sift_scube_merge(out, &added, NULL);
		sift_scube_free(&added);
	}
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

bool
sift_f2_scope_key_cube(SiftF2Scope *sc, int64 key, const SiftScopeQueryNeeds *q, SiftScopeCubeAcc *out, bool *exact)
{
	const SiftScopeL2Root *r = sift_scope_l2_find(&sc->meta, key);
	SiftF2ScopeSnap *settled = ((const SiftF2ScopeParts *) sc->entity.arg)->settled;
	SiftScopeEdit ed;
	bool		has;

	*exact = true;
	if (sc->nchanged == 0)
		return r != NULL && sift_scope_cube_live(&sc->meta, r, q, &sc->entity, out, NULL);
	sift_scope_overlay_classify(&sc->meta, r, key, &sc->entity, sc->changed, NULL, sc->nchanged, &ed);
	if (settled == NULL || !sift_f2_scope_snap_recall(settled, key, q, &ed, out, &has))
	{
		has = key_cube_compute(sc, r, key, q, &ed, out, exact);
		if (settled != NULL && *exact)
			sift_f2_scope_snap_remember(settled, key, q, &ed, has ? out : NULL);
	}
	sift_scope_edit_free(&ed);
	return has;
}

/* The rows of the key in [lo, hi), read from the rows of the key */
static bool
slice_cube(SiftF2Scope *sc, int64 key, int64 lo, int64 hi, SiftScopeCubeAcc *out)
{
	sift_scube_init(out, ((const SiftF2ScopeParts *) sc->entity.arg)->sc->shape);
	(void) sift_scope_overlay_slice(&sc->meta, sift_scope_l2_find(&sc->meta, key), key, &sc->entity, sc->changed, sc->nchanged, (SiftScopeSpan) {.lo = lo,.hi = hi}, out);
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

/* The window is a part of its month and the rest of the month is a small part of it: the month's cube less the rest. Not when what is left may be wrong for the query (*served false) */
static bool
month_less_rest(SiftF2Scope *sc, int64 key, SiftScopeQueryNeeds q, int64 lo, int64 hi, SiftScopeCubeAcc *out, bool *served)
{
	const int32 month = sift_scope_month_of(lo);
	const int64 rest[2][2] = {{sift_scope_month_start(month), lo}, {hi, sift_scope_month_end(month)}};
	bool		exact;
	bool		has;

	q.month = month;
	has = sift_f2_scope_key_cube(sc, key, &q, out, &exact);
	*served = exact;
	for (int i = 0; has && i < 2; i++)
	{
		SiftScopeCubeAcc c;

		if (rest[i][0] >= rest[i][1] || !slice_cube(sc, key, rest[i][0], rest[i][1], &c))
			continue;
		*served = sift_scope_cube_take_out(out, &c, &q);
		sift_scube_free(&c);
		if (!*served)
		{
			sift_scube_free(out);
			return false;
		}
	}
	if (has && out->row_count == 0)
	{
		sift_scube_free(out);
		has = false;
	}
	return has;
}

bool
sift_f2_scope_window_cube(SiftF2Scope *sc, int64 key, const SiftScopeQueryNeeds *q, int64 lo, int64 hi, SiftScopeCubeAcc *out)
{
	const int32 month = sift_scope_month_of(lo);
	const int64 rest = sift_scope_month_end(month) - sift_scope_month_start(month) - (hi - lo);
	bool		served = false;
	bool		has = false;

	if (rest * SIFT_SCOPE_REST_RATIO <= hi - lo)
		has = month_less_rest(sc, key, *q, lo, hi, out, &served);
	return served ? has : slice_cube(sc, key, lo, hi, out);
}

bool
sift_f2_scope_added_rank(const SiftF2Scope *sc, int64 key, int ring, int missing, const SiftScopeNewest *last)
{
	const SiftF2ScopeParts *p = sc->entity.arg;
	SiftScopeCubeAcc cube;
	bool		rank;

	if (p->added == NULL || !sift_f2s_added_cube(sc->entity.arg, key, SIFT_SPAN_ALL, &cube))
		return false;
	rank = ring != 0 || missing > 0 || last == NULL || (cube.has_time && cube.time_max >= last->time);
	sift_scube_free(&cube);
	return rank;
}
