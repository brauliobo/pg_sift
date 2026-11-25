/*
 * col/keystore/int.h - the view a backend keeps of a scope key store (keystore_view.c) and what keystore_run.c reads of it
 */
#ifndef SIFT_KEYSTORE_INT_H
#define SIFT_KEYSTORE_INT_H

#include "postgres.h"

#include "lib/ilist.h"

#include "col/v1/seg/int.h"

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

/* A view holds the runs of one epoch of a store: 6.0 writes epoch 0, a 5.x store has the epoch its compactions reached */
typedef struct ViewKey
{
	Oid			store;
	uint32		pad;			/* the hash key is these bytes: no byte of it is padding */
	uint64		epoch;
} ViewKey;

StaticAssertDecl(sizeof(ViewKey) == offsetof(ViewKey, epoch) + sizeof(uint64), "ViewKey has no padding");

typedef struct KeyStoreView
{
	ViewKey		key;			/* first: the hash key */
	MemoryContext cx;
	bool		loaded;
	uint64	   *slots;			/* the table of the codes below indexed (NULL until an appender asks), nslots of them */
	uint32		nslots;
	uint64		indexed;
	ViewPiece  *piece;
	int			npiece;
	int			cap;
	uint64		total;			/* the codes the pieces cover */
	int			pins;			/* handles open on the store: a view in use is not dropped for room */
	uint64 used;            /* sift_cache_tick() of the last open */
	dlist_node lru;         /* views_lru: the loaded views in the order of their last open, the oldest first */
	bool on_lru;
} KeyStoreView;

/* released by sift_keystore_close, or by the memory context of the open when an error skipped it */
typedef struct ViewPin
{
	MemoryContextCallback cb;
	ViewKey		key;
	bool		held;
} ViewPin;

KeyStoreView *kview_open(Oid store, uint64 epoch, ViewPin **pin);
void kview_release(ViewPin *pin);
void kview_reset(KeyStoreView *v);
void kview_add(KeyStoreView *v, uint64 lo, uint64 dbase, SiftCDict *d);

#endif
