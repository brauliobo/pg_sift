/*
 * dict_int.h - what dict.c (reader) and dict_write.c share: the dictionary structs, the block iterator and prefix8.
 */
#ifndef SIFT_BM25_DICT_INT_H
#define SIFT_BM25_DICT_INT_H

#include "core/host.h"

#include "core/cmp.h"
#include "bm25/dict.h"
#include "core/varint.h"


#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define BLOCK_HDR_MAX	(2 + 1 + 10)

#define corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift dictionary is corrupted"), \
			 errdetail(__VA_ARGS__)))

struct SiftDictWriter
{
	SiftStreamWriter *sw;
	bool		packed;			/* blocks of segment format 3 (bm25/dict.h) */
	BufFile    *spool;			/* DICT bytes */
	uint64		dict_len;
	StringInfoData blk;			/* entries of the open block */
	int			nblk;
	uint64		blk_post;		/* post_base of the open block */
	uint64		post_end;		/* end of the last term blob */
	char		prev[SIFT_MAX_TERM_BYTES];
	int			prevlen;
	uint64		nterms;
	uint32		nslots;			/* terms that are not inline */
	BufFile    *idx;			/* DICTIDX bytes: one entry per block */
	uint64		nidx;
	uint64	   *root;			/* the first prefix of every SIFT_DICT_ROOT_FANOUT entries */
	uint64		maxroot;
};

struct SiftDict
{
	SiftStreamReader *sr;
	uint64		dict_off;
	uint64		dict_len;
	uint64		idx_off;
	uint64		nblocks;
	uint64		nterms;
	uint64	   *root;
	uint32		nroot;
	uint8	   *blk;			/* copy of the current block */
	bool		slots;			/* lookups count the terms without inline postings */
	MemoryContext mcxt;
};

/* Decoding position inside a copied block */
typedef struct BlockIter
{
	const uint8 *p;
	const uint8 *end;
	int			left;
	bool		packed;			/* the block is of segment format 3 */
	uint64		post;
	uint32		slot;			/* of the next term without inline postings */
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;
} BlockIter;

static inline uint64
prefix8(const char *s, int len)
{
	uint64		p = 0;

	for (int i = 0; i < 8; i++)
		p = p << 8 | (i < len ? (uint8) s[i] : 0);
	return p;
}



#endif
