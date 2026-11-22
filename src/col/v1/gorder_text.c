/*
 * col/v1/gorder_text.c - the order of the strings of a text leaf: the collation, then the bytes (col/v1/gorder.h). Portable (core/host.h only).
 */

#include "col/codec.h"
#include "col/v1/gorder.h"
#include "core/cmp.h"

SiftCStrCmp sift_cgord_strcmp = NULL;

static int
str_cmp_bytes(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = memcmp(a, b, Min(la, lb));

	return c != 0 ? c : SIFT_CMP3(la, lb);
}

char *
sift_cgord_prefix_end(const char *s, uint32 len, uint32 *olen)
{
	char	   *e;

	while (len > 0 && (uint8) s[len - 1] == 0xFF)
		len--;
	if (len == 0)
		return NULL;
	e = palloc(len);
	memcpy(e, s, len);
	e[len - 1]++;
	*olen = len;
	return e;
}

int
sift_cgord_str_order(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	int			c = (sift_cgord_strcmp != NULL ? sift_cgord_strcmp : str_cmp_bytes) (a, la, b, lb, collid);

	return c != 0 ? c : str_cmp_bytes(a, la, b, lb, 0);
}

typedef struct RankEnt
{
	const char *s;
	uint32		len;
	uint32		code;
} RankEnt;

static uint32 rank_coll;

/* the codes in the order of their strings */
static int
rank_cmp(const void *pa, const void *pb)
{
	const RankEnt *a = pa,
			   *b = pb;

	return sift_cgord_str_order(a->s, a->len, b->s, b->len, rank_coll);
}

/* The strings of d ranked: ent[r] is the r-th, by the collation, then the bytes; the strings are decoded once, not at each comparison */
static RankEnt *
rank_strings(const SiftCDict *d, uint32 collid)
{
	uint32		nstr = d->n;
	/* check-memory: allow one entry for each string of a segment dictionary, which holds at most SIFT_COL_DICT_MAX_BYTES bytes */
	RankEnt    *ent = palloc(((Size) nstr + 1) * sizeof(RankEnt));

	for (uint32 i = 0; i < nstr; i++)
	{
		ent[i].code = i;
		ent[i].s = sift_cdict_str(d, i, &ent[i].len);
	}
	rank_coll = collid;
	qsort(ent, nstr, sizeof(RankEnt), rank_cmp);
	return ent;
}

uint32 *
sift_cgord_text_order(const SiftCDict *d, uint32 collid)
{
	RankEnt    *re = rank_strings(d, collid);
	uint32	   *order = palloc(((Size) d->n + 1) * sizeof(uint32));

	for (uint32 r = 0; r < d->n; r++)
		order[r] = re[r].code;
	pfree(re);
	return order;
}

int64 *
sift_cgord_text_ranks(const uint32 *order, uint32 nstr)
{
	/* check-memory: allow one rank for each string of a segment dictionary, which holds at most SIFT_COL_DICT_MAX_BYTES bytes */
	int64	   *rank = palloc(((Size) nstr + 1) * sizeof(int64));

	for (uint32 r = 0; r < nstr; r++)
		rank[order[r]] = r;
	return rank;
}
