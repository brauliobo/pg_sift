/*
 * core/f2/manifest/many.c - the edits of several partitions in one commit of the manifest (sift_manifest_commit_many): the leaves from the first edit's to the last one's are read once,
 * each edit is applied to its entry, and one record publishes them all, so that a step that changes many partitions at once (the packing of tiny parts) writes the directory once.
 */
#include "core/host.h"

#include <limits.h>

#include "core/f2/gate.h"
#include "core/f2/manifest/int.h"

typedef struct EditsArg
{
	const SiftManifestEdit *edits;
	int			n;
	SiftExtent *release;
	int			nrelease;
} EditsArg;

static void
build_commit_many(Relation rel, Tree *t, Change *ch, BlkVec *created, void *arg)
{
	const EditsArg *a = arg;
	int			lo = INT_MAX,
				hi = 0;

	for (int i = 0; i < a->n; i++)
	{
		int			leaf = sift_mf_find_leaf(t, a->edits[i].key_lo);

		lo = Min(lo, leaf);
		hi = Max(hi, leaf);
	}
	sift_mf_read_leaves(rel, t, lo, hi - lo + 1, ch, 2);
	for (int i = 0; i < a->n; i++)
		sift_mf_apply_edit(rel, t, ch, created, &a->edits[i]);
	ch->release = a->release;
	ch->nrelease = a->nrelease;
}

uint64
sift_manifest_commit_many(Relation rel, const SiftManifestEdit *edits, int n)
{
	EditsArg	a = {.edits = edits,.n = n};
	int			total = 0;

	SIFT_F2_GATE(SIFT_F2_GATE_MANIFEST);
	for (int i = 0; i < n; i++)
		total += edits[i].nrelease;
	/* check-memory: allow the extents the edits release, each edit's own parts' */
	a.release = palloc(Max(total, 1) * sizeof(SiftExtent));
	for (int i = 0; i < n; i++)
	{
		memcpy(a.release + a.nrelease, edits[i].release, edits[i].nrelease * sizeof(SiftExtent));
		a.nrelease += edits[i].nrelease;
	}
	return sift_mf_run_change(rel, NULL, 0, build_commit_many, &a, NULL, false);
}
