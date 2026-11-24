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

/* The slots of a window, and the leaves of a schema it reads of a granule, at most */
#define SIFT_PIN_WIN_SLOTS_MAX	64
#define SIFT_PIN_MAX_LEAVES		1024

struct F2Scan;
struct PartPreds;
struct SiftPinPart;

typedef struct SiftPinSlot
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
} SiftPinSlot;

typedef struct SiftPinWin SiftPinWin;

/* Whether the rows of the part are all visible to the snapshot, so that no row of it needs a look at the heap: the consumer's word */
typedef bool (*SiftPinPartOk) (void *arg, const SiftPartDesc *d);

/* A window of nslots slots for the scan of rel; leaves: the leaves (columns of the schema) the consumer decodes, which the leader reads with the columns of the predicates */
extern SiftPinWin *sift_pin_win_new(Relation rel, Snapshot snap, const int *leaves, int nleaves, uint32 nslots, SiftPinPartOk part_ok, void *part_arg);

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

/* What the window holds: granules and their rows */
extern uint32 sift_pin_win_items(const SiftPinWin *w);
extern uint64 sift_pin_win_rows(const SiftPinWin *w);
/* The granules the scan offered and the window did not take: their rows went through the scan */
extern uint32 sift_pin_win_refused(const SiftPinWin *w);

/* The scan reads the first n granules it offers itself, whatever the consumer says: what they hold is what a bound of the consumer is made of (the consumer's own sink sees their rows) */
extern void sift_pin_win_serial_head(SiftPinWin *w, uint32 n);

/*
 * Tracks the zone (least, greatest, nulls) of an integer leaf of every granule it takes: the consumer's bound on what a granule can hold. A granule whose leaf has no zone is a granule with none
 */
extern void sift_pin_win_track_zone(SiftPinWin *w, int leaf);
/* The zone of granule job; false when it has none */
extern bool sift_pin_win_job_zone(const SiftPinWin *w, uint32 job, int64 *min, int64 *max, bool *has_null);
/* The least and the greatest of the leaf over all the granules; false when a granule has no zone */
extern bool sift_pin_win_zone_range(const SiftPinWin *w, int64 *min, int64 *max);

/* The leader: the slot of item is filled from the granule job of the list / free again / the granule job is run through the scan. w is the window */
extern bool sift_pin_win_feed(SiftPinWin *w, uint32 item, uint32 job);
extern void sift_pin_win_release(SiftPinWin *w, uint32 item);
extern void sift_pin_win_replay(SiftPinWin *w, uint32 job);

/* The heap address of row `row` of granule job (the leader reads the chunk of the addresses of the granule) */
extern uint64 sift_pin_win_tid(SiftPinWin *w, uint32 job, uint32 row);

/* The slot of an item that was fed and not released: read only */
extern const SiftPinSlot *sift_pin_win_slot(const SiftPinWin *w, uint32 item);

/* The pins and the memory of the window: after the run, before the scan ends */
extern void sift_pin_win_end(SiftPinWin *w);

/*
 * A thread (pure C: the memory is the arena, the error is data). Filter: the rows of the granule that no deletion vector kills and every predicate the chunks decide passes, as a bitmap of
 * SIFT_CFILT_WORDS + 2 words, and their number; memo: a SiftStrMemo the thread zeroed for the granule. SIFT_ERR_SERIAL for a predicate the stored chunk cannot test.
 */
extern bool sift_pin_slot_filter_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, SiftStrMemo *memo, uint64 *active, uint32 *nactive);
/* The leaf of the granule as a ColView (col/f2/f2_view.h); rows: the nrows rows that will be read, NULL for every row */
extern bool sift_pin_slot_view_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const SiftStrMemo *memo, int leaf, bool codes, const uint16 *rows, int nrows, ColView *out);

#endif							/* SIFT_F2_PIN_WIN_H */
