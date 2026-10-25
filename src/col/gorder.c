/*
 * col/gorder.c - the global order of a leaf in a segment (col/gorder.h). Portable (core/host.h only).
 */

#include "core/host.h"

#include "col/codec.h"
#include "col/gorder.h"
#include "col/point.h"
#include "col/runs.h"
#include "core/cmp.h"

typedef struct OrdEnt
{
	int64		group;
	int64		key;
	uint64		code;			/* the address as sift_cpoint_tid_code: it sorts as the address does and takes 9 bits for the offset */
} OrdEnt;

static int
ord_cmp(const void *a, const void *b)
{
	const OrdEnt *x = a,
			   *y = b;

	if (x->group != y->group)
		return SIFT_CMP3(x->group, y->group);
	return x->key != y->key ? SIFT_CMP3(x->key, y->key) : SIFT_CMP3(x->code, y->code);
}

bool
sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	if (!sift_crun_leaf_ok(s, d))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return true;
		default:
			return false;
	}
}

static void
pad8(StringInfo out)
{
	while (out->len % 8)
		appendStringInfoChar(out, '\0');
}

/* the runs of equal group in ent[from, from + n): the groups, with the first entry and the number of the run, appended to out in the order of the entries */
static uint32
group_runs(const OrdEnt *ent, uint32 from, uint32 n, SiftCGOrdGrp *out, bool nulls)
{
	uint32		m = 0;

	for (uint32 i = 0; i < n;)
	{
		uint32		j = i + 1;

		while (j < n && ent[from + j].group == ent[from + i].group)
			j++;
		out[m].group = ent[from + i].group;
		if (nulls)
		{
			out[m].nstart = from + i;
			out[m].ncount = j - i;
		}
		else
		{
			out[m].vstart = from + i;
			out[m].vcount = j - i;
		}
		m++;
		i = j;
	}
	return m;
}

static void
build_block(OrdEnt *ent, uint32 nvals, uint32 nnull, int gcol, bool grouped, StringInfo out)
{
	uint64		n = (uint64) nvals + nnull;
	uint32		nchunks = (uint32) ((n + SIFT_CGORD_ROWS - 1) / SIFT_CGORD_ROWS);
	int64	   *kv = palloc(SIFT_CGORD_CAP * sizeof(int64)),
			   *av = palloc(SIFT_CGORD_CAP * sizeof(int64));
	StringInfoData body;
	SiftCGOrdDir *dir = palloc0((nchunks + 1) * sizeof(SiftCGOrdDir));
	SiftCGOrdGrp *gv = NULL,
			   *gn = NULL,
			   *gs = NULL;
	uint32		ngv = 0,
				ngn = 0,
				ng = 0;
	uint32		hdr[8] = {SIFT_CGORD_MAGIC, SIFT_CGORD_VERSION, (uint32) n, nvals, nchunks, SIFT_CGORD_ROWS, 0, 0};
	SiftCcPolicy pol;

	qsort(ent, nvals, sizeof(OrdEnt), ord_cmp);
	qsort(ent + nvals, nnull, sizeof(OrdEnt), ord_cmp);
	if (grouped)
	{
		uint32		i = 0,
					j = 0;

		gv = palloc((nvals + 1) * sizeof(SiftCGOrdGrp));
		gn = palloc((nnull + 1) * sizeof(SiftCGOrdGrp));
		gs = palloc0((nvals + nnull + 1) * sizeof(SiftCGOrdGrp));
		memset(gv, 0, (nvals + 1) * sizeof(SiftCGOrdGrp));
		memset(gn, 0, (nnull + 1) * sizeof(SiftCGOrdGrp));
		ngv = group_runs(ent, 0, nvals, gv, false);
		ngn = group_runs(ent, nvals, nnull, gn, true);
		/* the runs of the two regions merged by group */
		while (i < ngv || j < ngn)
		{
			if (j >= ngn || (i < ngv && gv[i].group < gn[j].group))
				gs[ng++] = gv[i++];
			else if (i >= ngv || gn[j].group < gv[i].group)
				gs[ng++] = gn[j++];
			else
			{
				gs[ng] = gv[i++];
				gs[ng].nstart = gn[j].nstart;
				gs[ng++].ncount = gn[j++].ncount;
			}
		}
		/* a group with no run in a region starts where that region's runs of the groups before it end */
		{
			uint32		vnext = 0,
						nnext = nvals;

			for (uint32 k = 0; k < ng; k++)
			{
				if (gs[k].vcount == 0)
					gs[k].vstart = vnext;
				if (gs[k].ncount == 0)
					gs[k].nstart = nnext;
				vnext = gs[k].vstart + gs[k].vcount;
				nnext = gs[k].nstart + gs[k].ncount;
			}
		}
		hdr[1] = SIFT_CGORD_VERSION_GROUP;
		hdr[6] = ng;
		hdr[7] = (uint32) gcol + 1;
	}
	sift_cc_policy_default(&pol);
	initStringInfo(&body);
	for (uint32 c = 0; c < nchunks; c++)
	{
		uint32		m = (uint32) Min((uint64) SIFT_CGORD_ROWS, n - (uint64) c * SIFT_CGORD_ROWS);
		uint64		at = (uint64) c * SIFT_CGORD_ROWS;
		uint32		k0;

		for (uint32 i = 0; i < m; i++)
		{
			kv[i] = ent[at + i].key;
			av[i] = (int64) ent[at + i].code;
		}
		dir[c].first = kv[0];
		dir[c].off = (uint32) body.len;		/* made absolute below */
		k0 = body.len;
		sift_cc_encode(&body, SIFT_CC_INT, kv, m, NULL, &pol, NULL);
		dir[c].klen = body.len - k0;
		pad8(&body);
		k0 = body.len;
		sift_cc_encode(&body, SIFT_CC_INT, av, m, NULL, &pol, NULL);
		dir[c].alen = body.len - k0;
		pad8(&body);
	}
	appendBinaryStringInfo(out, (const char *) hdr, sizeof(hdr));
	for (uint32 c = 0; c < nchunks; c++)
	{
		dir[c].off += SIFT_CGORD_HDR + nchunks * SIFT_CGORD_DIRENT;
		appendBinaryStringInfo(out, (const char *) &dir[c], SIFT_CGORD_DIRENT);
	}
	appendBinaryStringInfo(out, body.data, body.len);
	pad8(out);
	if (grouped)
	{
		appendBinaryStringInfo(out, (const char *) gs, ng * sizeof(SiftCGOrdGrp));
		pfree(gv);
		pfree(gn);
		pfree(gs);
	}
	pfree(kv);
	pfree(av);
	pfree(dir);
	pfree(body.data);
}

void
sift_cgord_build(int64 *key, uint64 *tid, uint32 nvals, uint64 *ntid, uint32 nnull, StringInfo out)
{
	OrdEnt	   *ent = palloc(((uint64) nvals + nnull + 1) * sizeof(OrdEnt));

	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {0, key[i], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {0, 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, 0, false, out);
	pfree(ent);
}

void
sift_cgord_build_grouped(int64 *key, uint64 *tid, int64 *group, uint32 nvals, uint64 *ntid, int64 *ngroup, uint32 nnull, int gcol, StringInfo out)
{
	OrdEnt	   *ent = palloc(((uint64) nvals + nnull + 1) * sizeof(OrdEnt));

	for (uint32 i = 0; i < nvals; i++)
		ent[i] = (OrdEnt) {group[i], key[i], sift_cpoint_tid_code(tid[i])};
	for (uint32 i = 0; i < nnull; i++)
		ent[nvals + i] = (OrdEnt) {ngroup[i], 0, sift_cpoint_tid_code(ntid[i])};
	build_block(ent, nvals, nnull, gcol, true, out);
	pfree(ent);
}

bool
sift_cgord_head(const uint8 *blk, uint64 len, SiftCGOrdHdr *h)
{
	uint32		w[8];
	bool		grouped;

	if (len < SIFT_CGORD_HDR)
		return false;
	memcpy(w, blk, sizeof(w));
	grouped = w[1] == SIFT_CGORD_VERSION_GROUP;
	if (w[0] != SIFT_CGORD_MAGIC || (w[1] != SIFT_CGORD_VERSION && !grouped) || w[3] > w[2] || w[5] != SIFT_CGORD_ROWS ||
		(grouped ? (w[6] == 0 || w[7] == 0 || w[7] > SIFT_COL_MAX_COLS) : (w[6] != 0 || w[7] != 0)) ||
		w[4] != (uint32) (((uint64) w[2] + SIFT_CGORD_ROWS - 1) / SIFT_CGORD_ROWS) ||
		SIFT_CGORD_HDR + (uint64) w[4] * SIFT_CGORD_DIRENT + (uint64) w[6] * SIFT_CGORD_GRPENT > len)
		return false;
	*h = (SiftCGOrdHdr) {w[2], w[3], w[4], w[5], w[6], grouped ? (int) w[7] - 1 : -1};
	return true;
}

bool
sift_cgord_groups(const uint8 *blk, const SiftCGOrdHdr *h, uint64 len, SiftCGOrdGrp *out)
{
	uint32		vnext = 0,
				nnext = h->values;

	memcpy(out, blk + len - (uint64) h->ngroups * SIFT_CGORD_GRPENT, (Size) h->ngroups * SIFT_CGORD_GRPENT);
	for (uint32 i = 0; i < h->ngroups; i++)
	{
		const SiftCGOrdGrp *g = &out[i];

		if ((i > 0 && g->group <= out[i - 1].group) || g->vstart != vnext || g->nstart != nnext || (g->vcount == 0 && g->ncount == 0) ||
			(uint64) g->vstart + g->vcount > h->values || (uint64) g->nstart + g->ncount > h->entries)
			return false;
		vnext += g->vcount;
		nnext += g->ncount;
	}
	return vnext == h->values && nnext == h->entries;
}

bool
sift_cgord_dirent(const uint8 *dir, const SiftCGOrdHdr *h, uint64 len, uint32 c, SiftCGOrdDir *d)
{
	if (c >= h->chunks)
		return false;
	memcpy(d, dir + (uint64) c * SIFT_CGORD_DIRENT, sizeof(*d));
	return d->pad == 0 && d->off % 8 == 0 && (uint64) d->off + (((uint64) d->klen + 7) & ~(uint64) 7) + (((uint64) d->alen + 7) & ~(uint64) 7) <= len &&
		d->off >= SIFT_CGORD_HDR + (uint64) h->chunks * SIFT_CGORD_DIRENT;
}

bool
sift_cgord_decode(MemoryContext mcxt, const uint8 *kb, uint32 klen, const uint8 *ab, uint32 alen, uint32 m, int64 *key, uint64 *addr)
{
	SiftCc	   *k = sift_cc_open_rows(mcxt, kb, klen, false, m),
			   *a = sift_cc_open_rows(mcxt, ab, alen, false, m);

	sift_cc_decode(k, key);
	sift_cc_decode(a, (int64 *) addr);
	for (uint32 i = 0; i < m; i++)
		addr[i] = sift_cpoint_tid_key(addr[i]);
	sift_cc_close(k);
	sift_cc_close(a);
	return true;
}
