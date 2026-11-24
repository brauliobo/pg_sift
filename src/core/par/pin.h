/*
 * core/par/pin.h - the window of granules a scan hands to the compute threads of the backend (core/par/pool.h), as the aggregate engine that consumes them sees it (col/f2/pin_win.h makes it).
 *
 * The leader feeds the granules of the window to the threads one slot at a time (sift_pin_win_feed reads their chunks into the slab of a free slot, the only reads of the pages), the slot is
 * immutable until sift_pin_win_release, and a thread reads memory and calls nothing of the server (sift_pin_slot_filter_a, sift_pin_slot_view_a). What a thread cannot do with a granule is
 * SIFT_ERR_SERIAL, and the leader runs the granule through the scan (sift_pin_win_replay) as if it had not been handed over.
 */
#ifndef SIFT_CORE_PAR_PIN_H
#define SIFT_CORE_PAR_PIN_H

#include "postgres.h"

#include "utils/rel.h"
#include "utils/snapshot.h"

#include "core/par/arena.h"
#include "core/scan_types.h"

/* The rows of a granule, the words of the bitmap of the rows a filter leaves (and the two the filter keeps its count and its bound in), and the rows below which a granule is read by their places */
#define SIFT_PIN_GRANULE_ROWS	8192
#define SIFT_PIN_ACTIVE_WORDS	(SIFT_PIN_GRANULE_ROWS / 64 + 2)
#define SIFT_PIN_POINT_ROWS_MAX	2048

struct SiftPartDesc;
struct SiftStrMemo;

typedef struct SiftPinWin SiftPinWin;
typedef struct SiftPinSlot SiftPinSlot;

/* Whether the rows of the part are all visible to the snapshot, so that no row of it needs a look at the heap: the consumer's word */
typedef bool (*SiftPinPartOk) (void *arg, const struct SiftPartDesc *d);

/* A window of nslots slots for the scan of rel; leaves: the leaves (columns of the schema) the consumer decodes, which the leader reads with the columns of the predicates */
extern SiftPinWin *sift_pin_win_new(Relation rel, Snapshot snap, const int *leaves, int nleaves, uint32 nslots, SiftPinPartOk part_ok, void *part_arg);

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
extern uint32 sift_pin_slot_nrows(const SiftPinSlot *sl);
/* The place of the granule of the slot in the list of the window */
extern uint32 sift_pin_slot_job(const SiftPinSlot *sl);

/* The pins and the memory of the window: after the run, before the scan ends */
extern void sift_pin_win_end(SiftPinWin *w);

/* A thread (pure C: the memory is the arena, the error is data). The memo of the strings its filters kept, zeroed for the granule */
extern struct SiftStrMemo *sift_pin_memo_new(SiftArena *a);
/*
 * Filter: the rows of the granule that no deletion vector kills and every predicate the chunks decide passes, as a bitmap of SIFT_PIN_ACTIVE_WORDS words, and their number; memo: the memo of
 * sift_pin_memo_new. SIFT_ERR_SERIAL for a predicate the stored chunk cannot test.
 */
extern bool sift_pin_slot_filter_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, struct SiftStrMemo *memo, uint64 *active, uint32 *nactive);
/* The leaf of the granule as a ColView; rows: the nrows rows that will be read, NULL for every row */
extern bool sift_pin_slot_view_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const struct SiftStrMemo *memo, int leaf, bool codes, const uint16 *rows, int nrows, ColView *out);

#endif							/* SIFT_CORE_PAR_PIN_H */
