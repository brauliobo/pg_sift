/*
 * agg/gord/int.h - the cursors and the state of the merge of the global order (agg/gord.c), shared with the cursors that read a second order group after group (agg/gord/group.c).
 */
#ifndef SIFT_AGG_GORD_INT_H
#define SIFT_AGG_GORD_INT_H

#include "postgres.h"

#include "col/gorder.h"
#include "agg/gord.h"

#define GORD_PEND_MAX	(1 << 20)	/* the entries a skip leaves to the heap: the rest of a longer one is walked */

/* A cursor over one source: the entries in the order of the scan, one chunk decoded at a time */
typedef struct GCur
{
	int			src;
	int			slot;			/* its place in the cursors of the scan (the leaf of the tournament of a text key) */
	uint32		entries;
	uint32		values;
	uint32		vs;				/* the values in the range of the scan: [vs, ve) */
	uint32		ve;
	uint32		ns;				/* the NULL rows of the group of the scan: [ns, ne) */
	uint32		ne;
	uint32		sk;				/* the entries sift_gord_skip took off the front of [vs, ve) (the back, descending) */
	int			region;			/* 0, 1: the regions in the order of the scan; 2: done */
	uint32		pos;			/* the next entry of the region (a group of a descending scan: pos in [gs, ge)) */
	uint32		end;
	uint32		gs;
	uint32		ge;
	bool		ends;			/* the source is the ends of a segment's order (cuts says where they stop being its own) */
	bool		gm;				/* the source lists its rows group after group, from the second order of another leaf: the key of an entry is its group (cur_next_g) */
	bool		gnull;			/* ... the NULL-key entries [ns, ne) of the group have been (or are being) read */
	uint32		ngrp;
	uint32		gnext;			/* ... the groups taken so far */
	uint32		gskip;			/* ... the groups sift_gord_skip took off the front */
	int64		gval;
	SiftCGOrdCuts cuts;
	bool		have;			/* cls, k and tid are the cursor's entry */
	int			cls;			/* 0: NULL rows first, 1: values, 2: NULL rows last */
	int64		k;
	uint64		tid;
	uint32		slen;			/* a text key: the string of k (str, slen bytes) */
	int64		skey;			/* ... and the key str is the string of (-1: none yet) */
	int8		clean;			/* every entry of the source is exact (sift_colsrc_gorder_clean): 1 yes, 2 no, 0 not asked */
	SiftCGOrdBuf *buf;			/* from the pool: the chunk of the entry in buf->key / buf->addr (a seek may have left it): the first buf->cm entries, the scan asks for more when it goes past them */
	char		str[SIFT_CGORD_STR_MAX];	/* last: a cursor clears the fields before it */
} GCur;

struct SiftGOrd
{
	SiftSrcSource *src;
	int			leaf;
	bool		desc;
	bool		nulls_first;
	int			grouped;		/* the leaf the arrays are grouped by (the scan reads the group of its condition), -1 */
	bool		bounded;		/* a range of the key: the rows with no value are not in it */
	uint32		head;			/* the entries of a chunk decoded when the scan enters it (the rows asked for, shared by the sources, at least GORD_HEAD_MIN) */
	int			ncur;
	GCur	  **cur;
	bool		started;
	GCur	   *prev;			/* the source of the last entry given, and the best of the others at that time (NULL: the sources are scanned) */
	GCur	   *runner;
	int		   *tree;			/* a text key: the tournament of the cursors, tree[0] the winner and tree[1..] the loser of each match (log2 of the sources compares a step, where the scan of them compares each) */
	bool		ends;			/* some source is the ends of a segment's order */
	bool		gm;				/* some source lists its rows group after group (no skip of an OFFSET) */
	bool		short_;			/* a source of the ends is spent: the order is not known past the last entry given */
	MemoryContext cxt;
	SiftGPend  *pend;			/* the entries sift_gord_skip took off the front that need the heap's word (sift_gord_pending) */
	uint32		npend;
	uint32		cpend;
	bool		last_set;
	bool		repeat;			/* the last sift_gord_next dropped an entry that repeated the one before it */
	int			last_cls;
	int64		last_key;
	uint64		last_tid;
	uint32		collid;			/* the keys are ranks of strings (a text key under this collation): entries compare by their strings, across sources */
	uint32		last_len;
	char		last_str[SIFT_CGORD_STR_MAX];
	MemoryContextCallback release;
};

extern void gord_cur_fill(SiftGOrd *g, GCur *c, uint32 p);
extern bool gord_group_load(SiftGOrd *g, GCur *c);
extern void gord_group_next(SiftGOrd *g, GCur *c);
extern uint32 gord_group_skip(SiftGOrd *g, uint32 k);

/* the address of the entry p is in the buffer: the window [afrom, afrom + an) of the chunk */
static inline bool
cur_addr_ok(const SiftCGOrdBuf *b, uint32 p)
{
	return p - b->at - b->afrom < b->an;
}

/* the entry p of the cursor's source (a cursor of addresses only, gm, has them for the whole chunk, whichever layout it is) */
static inline void
cur_at(SiftGOrd *g, GCur *c, uint32 p, int64 *key, uint64 *addr)
{
	if (c->gm ? !cur_addr_ok(c->buf, p) : p - c->buf->at - c->buf->kfrom >= c->buf->kn || !cur_addr_ok(c->buf, p))
		gord_cur_fill(g, c, p);
	*key = c->buf->key[p - c->buf->at];
	*addr = c->buf->addr[p - c->buf->at];
}

#endif
