/*
 * col/skip/join.c - the skipping indexes of parts joined as they are: the payloads of the granules do not depend on the place of the granule in the part, so each granule keeps its own
 * and an entry that a part lacks is flagged for its granules (col/skip_index.h).
 */
/* check-memory: allow-allocs every array here follows the columns, the granules or the predicates of one part, whose rows are bounded by the build buffer, not the rows of the table */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/skip/skip_int.h"
#include "core/page_io.h"

typedef struct PartSkip
{
	const SiftPart *part;
	const char *sec;
	uint64		len;
	const SiftSkipColDesc *desc;
	uint32		nentries;
} PartSkip;

typedef struct JoinKey
{
	uint16		col;
	uint8		kind;
	uint16		param;
} JoinKey;

static const PartSkip *
open_part(Relation rel, const SiftPart *part, PartSkip *ps)
{
	const SiftSkipIndexHeader *h;

	*ps = (PartSkip) {.part = part};
	ps->sec = sift_part_read_section(rel, part, SEC_SKIP_INDEX, &ps->len);
	if (ps->sec == NULL)
		return ps;
	h = (const SiftSkipIndexHeader *) ps->sec;
	if (ps->len < sizeof(*h) || h->magic != SIFT_SKIP_MAGIC || h->version != SIFT_SKIP_VERSION || h->ngranules != part->hdr.ngranules ||
		ps->len < sizeof(*h) + (uint64) h->nentries * sizeof(SiftSkipColDesc))
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The skipping indexes of the part do not start with their header.");
	ps->desc = (const SiftSkipColDesc *) (ps->sec + sizeof(*h));
	ps->nentries = h->nentries;
	return ps;
}

static int
cmp_key(const void *a, const void *b)
{
	const JoinKey *x = a,
			   *y = b;

	return x->col != y->col ? (x->col > y->col) - (x->col < y->col) : (x->kind > y->kind) - (x->kind < y->kind);
}

static const SiftSkipColDesc *
find_desc(const PartSkip *ps, const JoinKey *k)
{
	for (uint32 i = 0; i < ps->nentries; i++)
		if (ps->desc[i].col == k->col && ps->desc[i].kind == k->kind && ps->desc[i].param == k->param)
			return &ps->desc[i];
	return NULL;
}

/* The payloads of the granules of one part for one entry, appended to data: the running total of their bytes into *total, the end of each into ends and the flags */
static void
append_entry(Relation rel, const PartSkip *ps, const SiftSkipColDesc *d, StringInfo data, uint32 *total, uint32 *ends, uint8 *flags)
{
	uint32		ng = ps->part->hdr.ngranules;
	const uint32 *goff = NULL;
	const uint8 *gflags = NULL;

	if (d != NULL)
	{
		if ((uint64) d->meta_off + MAXALIGN((uint64) (ng + 1) * 4 + ng) > ps->len || d->data_off > ps->len)
			SIFT_SEG_CORRUPT(rel, ps->part->hdr.part_gen, "An entry of the skipping indexes lies outside its section.");
		goff = (const uint32 *) (ps->sec + d->meta_off);
		gflags = (const uint8 *) (goff + ng + 1);
	}
	for (uint32 g = 0; g < ng; g++)
	{
		flags[g] = d != NULL ? gflags[g] : SIFT_SKIP_GRAN_NONE;
		if (flags[g] == SIFT_SKIP_GRAN_PRESENT)
		{
			if (goff[g + 1] < goff[g] || (uint64) d->data_off + goff[g + 1] > ps->len)
				SIFT_SEG_CORRUPT(rel, ps->part->hdr.part_gen, "A payload of the skipping indexes lies outside its section.");
			appendBinaryStringInfo(data, ps->sec + d->data_off + goff[g], goff[g + 1] - goff[g]);
			*total += goff[g + 1] - goff[g];
		}
		ends[g] = *total;
	}
}

bool
sift_skip_join(Relation rel, SiftPart *const *src, int n, StringInfo out)
{
	PartSkip   *ps = palloc(n * sizeof(PartSkip));
	JoinKey    *keys = palloc(1 * sizeof(JoinKey));
	int			nkeys = 0,
				capk = 1;
	uint64		ng = 0;
	bool		any = false;

	for (int i = 0; i < n; i++)
	{
		open_part(rel, src[i], &ps[i]);
		ng += src[i]->hdr.ngranules;
		any |= ps[i].sec != NULL;
		for (uint32 e = 0; e < ps[i].nentries; e++)
		{
			JoinKey		k = {ps[i].desc[e].col, ps[i].desc[e].kind, ps[i].desc[e].param};
			bool		known = false;

			for (int j = 0; j < nkeys && !known; j++)
				known = keys[j].col == k.col && keys[j].kind == k.kind;
			if (known)
				continue;
			if (nkeys == capk)
				keys = repalloc(keys, (capk *= 2) * sizeof(JoinKey));
			keys[nkeys++] = k;
		}
	}
	if (!any)
		return false;
	qsort(keys, nkeys, sizeof(JoinKey), cmp_key);
	{
		SiftSkipIndexHeader h = {.magic = SIFT_SKIP_MAGIC,.version = SIFT_SKIP_VERSION,.nentries = nkeys,.ngranules = (uint32) ng};
		uint32		at = sizeof(h) + nkeys * sizeof(SiftSkipColDesc),
					meta = MAXALIGN((ng + 1) * 4 + ng);
		SiftSkipColDesc *desc = palloc0(nkeys * sizeof(SiftSkipColDesc));
		StringInfoData body;
		uint32	   *off = palloc((ng + 1) * sizeof(uint32));
		uint8	   *flags = palloc(ng);

		initStringInfo(&body);
		for (int k = 0; k < nkeys; k++)
		{
			StringInfoData data;
			uint32		total = 0;
			uint64		g0 = 0;

			initStringInfo(&data);
			off[0] = 0;
			for (int i = 0; i < n; i++)
			{
				const SiftSkipColDesc *d = ps[i].sec != NULL ? find_desc(&ps[i], &keys[k]) : NULL;

				append_entry(rel, &ps[i], d, &data, &total, off + g0 + 1, flags + g0);
				g0 += src[i]->hdr.ngranules;
			}
			SIFT_INVARIANT((uint64) at + body.len + meta + data.len <= PG_UINT32_MAX, "the skipping indexes of a part fit 32 bit offsets");
			desc[k] = (SiftSkipColDesc) {.col = keys[k].col,.kind = keys[k].kind,.param = keys[k].param,.meta_off = at + body.len,.data_off = at + body.len + meta};
			appendBinaryStringInfo(&body, (char *) off, (ng + 1) * 4);
			appendBinaryStringInfo(&body, (char *) flags, ng);
			while ((body.len & 7) != 0)
				appendStringInfoChar(&body, '\0');
			appendBinaryStringInfo(&body, data.data, data.len);
			pfree(data.data);
		}
		appendBinaryStringInfo(out, (char *) &h, sizeof(h));
		appendBinaryStringInfo(out, (char *) desc, nkeys * sizeof(SiftSkipColDesc));
		appendBinaryStringInfo(out, body.data, body.len);
	}
	return true;
}
