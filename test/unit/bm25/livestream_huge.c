/*
 * test/unit/bm25/livestream_huge.c - the position records of the live postings a segment streams (bm25/livestream.c): the records a term holds until its
 * df is known grow with the huge allocation past 1 GB (a plain one failed with "invalid memory alloc request size", and the merge failed for good), and
 * those of a term whose df went first go to the sink once they pass a bound, not once a group of postings ends.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/livestream.c"

#define NPOST		64
#define REC_LEN		4096

static SiftDocSBRef sbref;
static DocMap dm = {.dir = &sbref,.nsb = 1};
static uint8 rec[REC_LEN];
static int	nsent;

DocMap *
docmap(SiftSegment *seg)
{
	return &dm;
}

int64
sb_search(DocMap *m, uint32 sb)
{
	return 0;
}

uint64_t
sift_grp_count(const sift_grp *g)
{
	return NPOST;
}

uint32_t
sift_grp_tids(const sift_grp *g, const uint64_t *mask, sift_tid *out)
{
	for (int i = 0; i < NPOST; i++)
		out[i] = i + 1;
	return NPOST;
}

void
sift_post_group_tf(SiftPostingCursor *c, uint8 *tf)
{
	memset(tf, 1, NPOST);
}

const uint8 *
sift_post_pos_next(SiftPosReader *r, uint32 *len)
{
	*len = REC_LEN;
	return rec;
}

static void
postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen, int n)
{
	nsent += n;
}

static const SiftSegSink sink = {.postings = postings};
static Stream s;
static sift_grp g;

static void
group(void *arg)
{
	stream_group(&s, NULL, &g, (SiftPosReader *) rec);
}

/* A stream whose one SB has NPOST docs on its first page, each of dl 1 */
static void
stream_init(void)
{
	static SBDocs e = {.len = {NPOST}};
	static SBDocs *sb = &e;
	static uint16 dl[NPOST];

	for (int i = 0; i < NPOST; i++)
		dl[i] = 1;
	e.dl = dl;
	s = (Stream) {.sink = &sink,.dc = {.mcxt = CurrentMemoryContext,.sb = &sb,.newest = &e,.oldest = &e}};
	nsent = 0;
}

static void
check_grow(void)
{
	stream_init();
	s.poslen = s.poscap = MaxAllocSize / 2 + 1;
	s.pos = palloc(s.poscap);
	SIFT_CHECKF(pgshim_try(group, NULL), "the records of a term grown past %zu bytes failed: %s", s.poscap, pgshim_errmsg);
	SIFT_CHECKF(s.n == NPOST && s.poslen == MaxAllocSize / 2 + 1 + NPOST * REC_LEN, "%u postings in %zu bytes", s.n, s.poslen);
}

static void
check_flush(void)
{
	stream_init();
	s.streaming = true;
	SIFT_CHECKF(pgshim_try(group, NULL), "the group failed: %s", pgshim_errmsg);
	SIFT_CHECKF(nsent > 0 && nsent + s.n == NPOST, "%d postings of %d went before the group ended", nsent, NPOST);
	SIFT_CHECKF(s.poscap <= 2 * STREAM_POS_KEEP, "%zu bytes held for postings of %d bytes of records", s.poscap, REC_LEN);
}

int
main(void)
{
	check_grow();
	check_flush();
	return sift_check_summary();
}
