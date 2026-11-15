/*
 * col/track_hear.h - the triggers of an index with track_changes as a set: one AFTER DELETE and one AFTER UPDATE on the table of the index and, for a statement trigger, on every table above it
 * (col/track_hear.c). col/track.c decides what the log may be taken for from them.
 */
#ifndef SIFT_COL_TRACK_HEAR_H
#define SIFT_COL_TRACK_HEAR_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "storage/lockdefs.h"
#include "utils/rel.h"

#define HEAR_FUNCTION	"col_track_old_rows"

/*
 * The triggers of an index are the rows of pg_trigger that it depends on (NORMAL): for each table whose statements it must hear, an AFTER DELETE and an AFTER UPDATE (an internal trigger gets
 * its oid appended to its name, so they are found by what the index depends on). Those tables are the table of the index and, for a statement trigger, every table above it: a statement on
 * a partitioned or an inheritance parent fires the triggers of the parent alone, and the transition table of the parent holds the old rows of all its children (a row trigger fires on the child
 * itself, so track_changes = ctid has the table of the index alone). The index depends on them, and not they on it, because DROP INDEX CONCURRENTLY refuses to start once a transaction id is
 * assigned and deletes what depends on the index first: an index nothing depends on drops, and sift_col_track_hook removes the triggers in the transaction that deletes the index (or a worker a
 * moment later, when a table is in use: track_drop.c). REINDEX CONCURRENTLY swaps the dependencies of the old and the new index, so the triggers follow the index that stays. Each is checked to be
 * what the option created: calling the function, a statement trigger, always enabled. The identity of an event is the xmin of the catalog rows of its triggers, which any change to a row (disabling
 * and enabling it again, a rename) makes another.
 */
typedef struct HearTrig
{
	Oid			oid;
	Oid			relid;
	NameData	name;
	int			event;			/* 0: AFTER DELETE, 1: AFTER UPDATE */
	bool		ok;
	bool		origin;			/* what ok says, but enabled for the origin only (ENABLE TRIGGER) */
	uint32		witness;
} HearTrig;

/* What the triggers of an index are for one event: [0] AFTER DELETE, [1] AFTER UPDATE */
typedef struct Hear
{
	int			n;
	bool		whole;			/* one on each table that must be heard, none on another */
	bool		ok;				/* whole, each as the option made it */
	bool		origin;			/* whole, each as the option made it or enabled for the origin only, and one of them so */
	uint32		witness;
} Hear;

/*
 * What the triggers of an index lack or have too much of for the tables that must be heard: a trigger to make (a table that has none for an event), one that ENABLE TRIGGER left enabled for the origin
 * only, one on a table that is not heard any more (it was detached, or no longer inherits), and whether one is not ours to change (disabled by the user, or two for one table and event).
 */
typedef struct HearWant
{
	Oid			relid;
	int			event;
} HearWant;

typedef struct HearPlan
{
	List	   *make;
	List	   *enable;
	List	   *drop;
	bool		bad;
} HearPlan;

extern List *sift_col_hear_tables(Oid heapoid, bool row);
extern List *sift_col_hear_find(Relation index, bool row);
extern uint32 sift_col_hear_witness(List *trigs, int event);
extern bool sift_col_hear_all_ok(List *trigs, int event, uint32 *witness);
extern void sift_col_hear_read(Oid heapoid, Relation index, List *trigs, Hear h[2]);
extern bool sift_col_hear_lock_briefly(Oid relid, LOCKMODE mode);
extern bool sift_col_hear_lock_all(Relation heap, Relation index);
extern void sift_col_hear_create(Relation heap, Relation index);
extern void sift_col_hear_drop_all(Relation index);
extern HearPlan sift_col_hear_plan(Relation heap, Relation index, List *trigs);
extern bool sift_col_hear_plan_empty(const HearPlan *plan);
extern bool sift_col_hear_apply(Relation heap, Relation index, const HearPlan *plan);

#endif
