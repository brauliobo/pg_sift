/*
 * core/xlog.h - the one way the extension changes a page through generic WAL, and the rule every such change keeps.
 *
 * A page of the index holds its bytes in [SIFT_PAGE_DATA_OFFSET, pd_lower); [pd_lower, pd_special) is the hole that generic WAL and
 * full-page images replay as zeros and the special space (SiftPageOpaqueData: type, flags, generation) is written only when a page is
 * built (sift_page_init, a full image). So a change of a page that is not a full image must leave pd_special, pd_upper and every
 * byte from pd_lower on as the buffer had them (a byte written there is damage the next reader finds, or a change the log does not
 * carry), and pd_lower inside the payload. sift_xlog_finish checks that for every page of the record before the record is made:
 * a write that computed its offset wrongly ends in an ERROR, with nothing logged and the buffers as they were, and not in a page
 * that every later read of the index refuses. Writers do not check their offsets one by one: this is the check.
 */
#ifndef SIFT_XLOG_H
#define SIFT_XLOG_H

#include "core/host.h"

#include "core/page.h"

#ifndef MAX_GENERIC_XLOG_PAGES
#define MAX_GENERIC_XLOG_PAGES	4
#endif

/* Why the page `after` is not a legal change of `before` (both BLCKSZ bytes, `before` the buffer, `after` the working copy of the record), NULL when it is */
static inline const char *
sift_page_write_violation(const char *before, const char *after)
{
	const PageHeaderData *b = (const PageHeaderData *) before,
			   *a = (const PageHeaderData *) after;

	if (a->pd_special != b->pd_special || a->pd_upper != b->pd_upper || a->pd_upper > BLCKSZ)
		return "the page header's pd_special or pd_upper changed";
	if (a->pd_lower < SIFT_PAGE_DATA_OFFSET || a->pd_lower > a->pd_upper)
		return "pd_lower lies outside the payload of the page";
	if (memcmp(before + a->pd_upper, after + a->pd_upper, BLCKSZ - a->pd_upper) != 0)
		return "a byte of the special space changed";
	if (memcmp(before + a->pd_lower, after + a->pd_lower, a->pd_upper - a->pd_lower) != 0)
		for (uint32 i = a->pd_lower; i < a->pd_upper; i++)
			if (after[i] != before[i] && after[i] != 0)
				return "a byte past pd_lower was written, which the log does not carry";
	return NULL;
}

/* A record of up to MAX_GENERIC_XLOG_PAGES pages: GenericXLogState with the buffers it holds, so that the change can be checked against them */
typedef struct SiftXlog
{
	Relation	index;
	GenericXLogState *state;
	int			n;
	Buffer		buf[MAX_GENERIC_XLOG_PAGES];
	Page		page[MAX_GENERIC_XLOG_PAGES];
	bool		full[MAX_GENERIC_XLOG_PAGES];	/* a full image: the page is built, not changed */
} SiftXlog;

static inline SiftXlog
sift_xlog_start(Relation index)
{
	return (SiftXlog) {.index = index,.state = GenericXLogStart(index)};
}

/* The working copy of the exclusively locked buffer; GENERIC_XLOG_FULL_IMAGE as for GenericXLogRegisterBuffer */
static inline Page
sift_xlog_register(SiftXlog *x, Buffer buf, int flags)
{
	Page		page;

	for (int i = 0; i < x->n; i++)
		if (x->buf[i] == buf)
			return x->page[i];
	page = GenericXLogRegisterBuffer(x->state, buf, flags);
	x->buf[x->n] = buf;
	x->page[x->n] = page;
	x->full[x->n++] = (flags & GENERIC_XLOG_FULL_IMAGE) != 0;
	return page;
}

/* Logs the record and copies the changed pages into their buffers; ERROR (the record is dropped) when a change breaks the rule above */
static inline void
sift_xlog_finish(SiftXlog *x)
{
	for (int i = 0; i < x->n; i++)
	{
		const char *why = x->full[i] ? (sift_page_layout_ok(x->page[i]) ? NULL : "the page built is not laid out as a page of the index")
			: sift_page_write_violation(BufferGetPage(x->buf[i]), x->page[i]);

		if (why != NULL)
		{
			GenericXLogAbort(x->state);
			ereport(ERROR,	/* check-errors: allow */
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("sift index \"%s\": a change of block %u was refused", RelationGetRelationName(x->index), BufferGetBlockNumber(x->buf[i])),
					 errdetail("%s.", why)));
		}
	}
	GenericXLogFinish(x->state);
}

static inline void
sift_xlog_abort(SiftXlog *x)
{
	GenericXLogAbort(x->state);
}

#endif							/* SIFT_XLOG_H */
