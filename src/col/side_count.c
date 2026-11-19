/*
 * col/side_count.c - what the scope sidecars of an index hold of one key (col/side.h): the counts a read of the key is priced by
 */
#include "postgres.h"

#include "col/side.h"

void
sift_side_key_counts(Relation index, int scope, int64 key, SideKeyCounts *c)
{
	SiftDirSnapshot *snap;

	*c = (SideKeyCounts) {0};
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		SiftColSegment *seg;
		const SiftSide *sd;
		const SideScope *sc;
		const SideRun *runs;
		uint32		n = 0;

		if (e->kind != SIFT_SEG_IMMUTABLE || (e->flags & (SIFT_SEG_SCOPE | SIFT_SEG_NET)))
			continue;
		seg = sift_cseg_open(index, e, CurrentMemoryContext);
		sd = sift_cseg_side(seg);
		sc = sd != NULL ? sift_side_scope(sd, scope) : NULL;
		if (sc == NULL)
		{
			c->other_units += e->sb_hi - e->sb_lo + 1;
			c->other_rows += e->ndocs;
		}
		else
		{
			c->side_rows += e->ndocs;
			runs = sift_side_runs(seg, sd, sc, key, &n);
			for (uint32 r = 0; r < n; r++)
			{
				c->runs++;
				c->units += runs[r].nunits;
				c->rows += runs[r].rows;
			}
			if (n > 0)
				pfree((void *) runs);
		}
		sift_cseg_close(seg);
	}
	pfree(snap);
}
