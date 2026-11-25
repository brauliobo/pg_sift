/*
 * col/granule/part/int.h - what the files that write and read a part of granules share (col/granule.h).
 */
#ifndef SIFT_GRANULE_PART_INT_H
#define SIFT_GRANULE_PART_INT_H

#include "core/host.h"

#include "col/granule/int.h"

#define SIFT_DICTS_CHUNK	16384

/* The kind of an entry (SiftDictKind) is in the top 4 bits of nchunks, the chunks of a STR dictionary in the low 12: a part written before the kinds has kind 0 */
#define SIFT_DICT_KIND_SHIFT		12
#define SIFT_DICT_NCHUNKS_MASK		0x0FFFu
#define SIFT_DICT_ENTRY_KIND(nc)	((SiftDictKind) ((nc) >> SIFT_DICT_KIND_SHIFT))
#define SIFT_DICT_ENTRY_NCHUNKS(nc)	((uint16) ((nc) & SIFT_DICT_NCHUNKS_MASK))
#define SIFT_DICT_PACK_NC(kind, nc)	((uint16) (((uint16) (kind) << SIFT_DICT_KIND_SHIFT) | ((nc) & SIFT_DICT_NCHUNKS_MASK)))

typedef struct SiftDictsEntry
{
	uint16		col;
	uint16		nchunks;			/* SIFT_DICT_PACK_NC */
	uint32		n;
	uint64		off;
	uint64		len;
} SiftDictsEntry;

StaticAssertDecl(sizeof(SiftDictsEntry) == 24, "SiftDictsEntry must be 24 bytes");

struct SiftFsst;

/* What the columns of a part are coded by besides their own dictionaries (part_codecs.c): per column a table of symbols or a zstd dictionary, with the bytes SEC_DICTS keeps; its memory context frees the zstd handles */
typedef struct SiftPartCodecs
{
	MemoryContext cx;
	int			ncols;
	int			nentries;		/* the columns with raw bytes: the entries these add to SEC_DICTS */
	struct SiftFsst **fsst;
	void	  **cdict;
	uint8	  **raw;
	uint32	   *rawlen;
} SiftPartCodecs;

/* adopt: the dictionaries of a part the rows come mostly from, whose tables are kept for the columns where they still pay */
extern SiftPartCodecs *sift_part_codecs_train(const SiftRows *rows, SiftCompressTier tier, SiftDictEnc *const *dicts, const SiftColPromo *promos, const SiftPartDicts *adopt);
extern void sift_part_codecs_free(SiftPartCodecs *pc);

extern void sift_part_add_section(SiftSecInput *secs, int *n, uint16 tag, uint16 flags, const void *data, uint64 len, bool inl);

#endif							/* SIFT_GRANULE_PART_INT_H */
