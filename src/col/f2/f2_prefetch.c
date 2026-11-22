/*
 * col/f2/f2_prefetch.c - the plan of the pages a scan reads ahead (col/f2/f2_prefetch.h).
 */
#include "core/host.h"

#include "col/f2/f2_prefetch.h"

int
sift_pf_survivor_spans(const bool *skipped, uint32 g0, uint32 n, uint32 spans[][2])
{
	int			ns = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (skipped[i])
			continue;
		if (ns > 0 && spans[ns - 1][1] == g0 + i)
			spans[ns - 1][1]++;
		else
		{
			spans[ns][0] = g0 + i;
			spans[ns][1] = g0 + i + 1;
			ns++;
		}
	}
	return ns;
}

bool
sift_pf_window_wanted(uint32 window_index, bool dense)
{
	return window_index == 0 || dense;
}

SiftPfPlan *
sift_pf_plan(SiftPfRange range, void *ctx, uint64 section_off, uint32 page_payload, const uint32 spans[][2], int nspans)
{
	SiftPfPlan *plan = palloc0(sizeof(SiftPfPlan));
	uint32		cap = 0;

	for (int i = 0; i < nspans; i++)
		for (uint32 g = spans[i][0]; g < spans[i][1]; g++)
		{
			uint64		off,
						first,
						last;
			uint32		len;

			if (!range(ctx, g, &off, &len) || len == 0)
				continue;
			first = (section_off + off) / page_payload;
			last = (section_off + off + len - 1) / page_payload;
			if (plan->nruns > 0 && first <= plan->runs[plan->nruns - 1].first + plan->runs[plan->nruns - 1].npages + SIFT_PF_MAX_GAP_PAGES)
			{
				SiftPfRun  *r = &plan->runs[plan->nruns - 1];
				uint64		end = Max(r->first + r->npages, last + 1);

				plan->total_pages += end - (r->first + r->npages);
				r->npages = (uint32) (end - r->first);
			}
			else
			{
				if (plan->nruns == cap)
				{
					cap = cap ? cap * 2 : 16;
					plan->runs = plan->runs ? repalloc(plan->runs, cap * sizeof(SiftPfRun)) : palloc(cap * sizeof(SiftPfRun));
				}
				plan->runs[plan->nruns++] = (SiftPfRun) {.first = first,.npages = (uint32) (last - first + 1)};
				plan->total_pages += last - first + 1;
			}
			if (plan->total_pages >= SIFT_PF_BUDGET_PAGES)
				goto done;
		}
done:
	if (plan->total_pages < SIFT_PF_MIN_PAGES)
	{
		sift_pf_free(plan);
		return NULL;
	}
	return plan;
}

uint32
sift_pf_batch(SiftPfPlan *plan, uint32 max, uint64 *pages)
{
	uint32		n = 0;

	while (n < max && plan->cursor_run < plan->nruns)
	{
		const SiftPfRun *r = &plan->runs[plan->cursor_run];

		pages[n++] = r->first + plan->cursor_page++;
		if (plan->cursor_page == r->npages)
		{
			plan->cursor_run++;
			plan->cursor_page = 0;
		}
	}
	return n;
}

void
sift_pf_free(SiftPfPlan *plan)
{
	if (plan->runs != NULL)
		pfree(plan->runs);
	pfree(plan);
}
