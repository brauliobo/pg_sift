/*
 * col/f2/f2_scope_delta.c - the cube of a scope key from a summary that the pairs have moved under (f2_scope.h): the members the journal says changed for the key are told added, removed or
 * unchanged against the summary (col/scope_v6/overlay.c), their rows read from the entity parts under the pairs as they are now, and the rows the summary does not hold (slabs, parts it does
 * not name) that the key owns now are added from the entity side of the summary (f2_scope_over.c). The rows of the named parts that the snapshot does not see are out of every read of them.
 */
#include "postgres.h"

#include "col/f2/f2_scope_int.h"
#include "col/f2/f2_scope_snap.h"

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
	if (sc->entity.added_cube != NULL && sc->entity.added_cube(sc->entity.arg, key, q->month, &added))
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

bool
sift_f2_scope_added_rank(const SiftF2Scope *sc, int64 key, int ring, int missing, const SiftScopeNewest *last)
{
	const SiftF2ScopeParts *p = sc->entity.arg;
	SiftScopeCubeAcc cube;
	bool		rank;

	if (p->added == NULL || !sift_f2s_added_cube(sc->entity.arg, key, SIFT_SCOPE_NO_MONTH, &cube))
		return false;
	rank = ring != 0 || missing > 0 || last == NULL || (cube.has_time && cube.time_max >= last->time);
	sift_scube_free(&cube);
	return rank;
}
