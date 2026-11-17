/*
 * col/vstore/vstore_int.h - the view a backend keeps of a shared value store (vstore_view.c) and what vstore_run.c reads of it
 */
#ifndef SIFT_VSTORE_INT_H
#define SIFT_VSTORE_INT_H

#include "postgres.h"

#include "col/seg_int.h"

/*
 * What a backend keeps of a store, up to the last code it has read: the runs as they were parsed (a piece covers the codes from lo, its strings from index
 * lo - dbase of the dictionary of the run), which is all that a reader of strings by code needs, and, for an appender whose batch is small beside the
 * store, a table of the hash of the strings (slots: hash << 32 | code + 1) that finds the code of a string
 */
typedef struct ViewPiece
{
	uint64		lo;				/* the first code of the piece */
	uint64		dbase;			/* the code of string 0 of d */
	SiftCDict  *d;
} ViewPiece;

typedef struct VStoreView
{
	Oid			store;			/* first: the hash key */
	MemoryContext cx;
	bool		loaded;
	uint64	   *slots;			/* the table of the codes below indexed (NULL until an appender asks), nslots of them */
	uint32		nslots;
	uint64		indexed;
	ViewPiece  *piece;
	int			npiece;
	int			cap;
	uint64		total;			/* the codes the pieces cover */
	uint64		epoch;
	int			pins;			/* handles open on the store: a view in use is not dropped for room */
	uint64		used;			/* sift_cache_tick() of the last open */
} VStoreView;

/* released by sift_vstore_close, or by the memory context of the open when an error skipped it */
typedef struct ViewPin
{
	MemoryContextCallback cb;
	Oid			store;
	bool		held;
} ViewPin;

VStoreView *vview_open(Oid store, ViewPin **pin);
void vview_release(ViewPin *pin);
void vview_reset(VStoreView *v, uint64 epoch);
void vview_add(VStoreView *v, uint64 lo, uint64 dbase, SiftCDict *d);

#endif
