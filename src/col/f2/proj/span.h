/*
 * col/f2/proj/span.h - the granules of a part a cursor reads: ascending, disjoint spans [lo, hi) (the whole part, or what the key marks leave to a statement, col/f2/v6/proj/seek.h), walked one
 * granule at a time. The cursor of f2_proj_scan.c and the tests of the seek walk them the same way.
 */
#ifndef SIFT_F2_PROJ_SPAN_H
#define SIFT_F2_PROJ_SPAN_H

#include "core/host.h"

#define SIFT_PROJ_SPANS_MAX		16

typedef struct SiftProjSpans
{
	uint32		span[SIFT_PROJ_SPANS_MAX][2];
	int			n;
	int			at;				/* the span in hand */
	uint32		next;			/* the next granule of it */
	uint32		skipped;		/* granules of the part left out by the spans */
} SiftProjSpans;

static inline void
sift_proj_spans_set(SiftProjSpans *s, const uint32 (*span)[2], int n, uint32 ngranules)
{
	uint32		covered = 0;

	memcpy(s->span, span, n * sizeof(s->span[0]));
	s->n = n;
	s->at = 0;
	s->next = n > 0 ? span[0][0] : 0;
	for (int i = 0; i < n; i++)
		covered += span[i][1] - span[i][0];
	s->skipped = ngranules - covered;
}

static inline void
sift_proj_spans_whole(SiftProjSpans *s, uint32 ngranules)
{
	const uint32 whole[1][2] = {{0, ngranules}};

	sift_proj_spans_set(s, whole, 1, ngranules);
}

static inline bool
sift_proj_spans_next(SiftProjSpans *s, uint32 *g)
{
	while (s->at < s->n && s->next >= s->span[s->at][1])
		if (++s->at < s->n)
			s->next = s->span[s->at][0];
	if (s->at >= s->n)
		return false;
	*g = s->next++;
	return true;
}

#endif							/* SIFT_F2_PROJ_SPAN_H */
