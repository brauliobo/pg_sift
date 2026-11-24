/*
 * col/f2/pin_win.h - the granules of a scan that the compute threads of the backend (core/par/pool.h) filter and decode, handed over by the scan and fed to the threads one at a time.
 *
 * The leader runs the scan as it always does, for every part and granule it reads; a granule that a thread can take (sift_pin_win_take: every predicate is decided on the stored chunks or can be, no
 * column delta, exception, pending delete, bridge or key lookup touches it, and the consumer says the part is all-visible) is listed and not read. When the scan is done, the leader feeds the list to
 * the threads (core/par/morsel.h in RING mode): sift_pin_win_feed reads the chunks the predicates and the consumer's leaves need of the next granule into the slab of a free slot (the only reads of
 * the pages, and they are the leader's), resolves its deletion vector, and the slot is then immutable until sift_pin_win_release: a thread reads memory and calls nothing of the server
 * (sift_pin_slot_filter_a, sift_pin_slot_view_a). What a thread cannot do with a granule (a NULL it does not fold, a predicate the stored chunk cannot decide) is SIFT_ERR_SERIAL, and the leader
 * runs the granule through the scan (sift_pin_win_replay) as if it had not been handed over.
 *
 * The slot is the same for the index of a sift_col and the store of a sift_table.
 */
#ifndef SIFT_F2_PIN_WIN_H
#define SIFT_F2_PIN_WIN_H

#include "postgres.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_sink.h"
#include "col/f2/pend_del.h"
#include "col/granule/granule.h"
#include "col/granule/granule_filter.h"
#include "core/par/arena.h"
#include "core/par/pin.h"

/* The slots of a window, and the leaves of a schema it reads of a granule, at most */
#define SIFT_PIN_WIN_SLOTS_MAX	64
#define SIFT_PIN_MAX_LEAVES		1024

struct F2Scan;
struct PartPreds;
struct SiftPinPart;

struct SiftPinSlot
{
	SiftGranuleView view;		/* over slab: the chunks the leader read, and the dictionaries of the part */
	const struct SiftPinPart *part;
	uint32		granule;
	uint32		job;			/* the place of the granule in the list of the window */
	uint16		nrows;
	uint32		decided;		/* bit k: predicate k is decided on the granule, no row of it needs the test */
	bool		any_dead;
	uint64		dead[SIFT_DV_MASK_WORDS];
	char	   *slab;
	Size		slab_cap;
};

/* Called by the scan for every granule it would read: true when the window took it (the scan does not read it) */
extern bool sift_pin_win_take(SiftPinWin *w, Relation rel, struct F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const struct PartPreds *pp, uint32 g, const bool *decided,
							  const SiftPendDelIndex *px, Snapshot snap);

/*
 * The predicates of the scan of a sift_table: every row the threads take of a granule passes them, as the stored chunks say. The window takes no granule when one of them cannot run on the chunks
 * as they are stored (it needs a decoded value, a function, or the heap), so that the scan reads those as it always did. Called once, before the scan offers a granule
 */
extern void sift_pin_win_table_preds(SiftPinWin *w, const SiftColSchema *schema, const SiftColPred *preds, int npreds);

/*
 * Called by the scan of a sift_table for every granule it would read, with whether the granule is for the threads (eligible: every row of it is there for the snapshot as the stored values show
 * it, no bridge, pending delete or column delta touches it): true when the window took it, which it does not for a column map, or a value of a leaf of a predicate that the chunks cannot judge.
 * host is the partition entry of the part, which replay gives back to the scan, with the pin of the window, to read the granule as it would have
 */
typedef void (*SiftPinReplay) (void *arg, const void *host, SiftF2PartPin *pin, uint32 granule);
extern bool sift_pin_win_take_table(SiftPinWin *w, Relation rel, const SiftColSchema *schema, const void *host, SiftF2PartPin *pin, uint32 g, bool eligible, SiftPinReplay replay,
									void *replay_arg);

#endif							/* SIFT_F2_PIN_WIN_H */
