/*
 * col/scope_v6/test/scope_v6_edit.h - the checks of a scope summary whose entity parts or pairs changed, shared by scope_v6_edit_test.c and scope_v6_shape_test.c.
 */
#ifndef SIFT_COL_TEST_SCOPE_V6_EDIT_H
#define SIFT_COL_TEST_SCOPE_V6_EDIT_H

#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/scope_v6/test/scope_v6_world.h"

typedef struct DelStats
{
	int64		keys;
	int64		clean;
	int64		node_path;
	int64		scans;
	int64		over_root;
	int64		over_root_descended;
} DelStats;

typedef struct EditStats
{
	int64		dirty;
	int64		patched;
	int64		rescans;
	int64		medium;
	int64		carried_nodes;
	int64		work_full;
	int64		work_refresh;
	int64		pool_before;
	int64		pool_after;
	int64		moved;
} EditStats;

/* The live cube of every key and month, from the object with its dead rows taken out, against the brute force */
extern void tw_check_live(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *tpm, const SiftScopeEntity *e, const char *label, DelStats *ds);
/* The newest 20 rows of every key, from the ring and the month map, unfiltered and filtered, against the brute force */
extern void tw_check_latest(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *tpm, const SiftScopeEntity *e, const char *label);
/*
 * One round: edits the pairs of w, patches and refreshes the summary in old, leaves the refreshed object in old. flags: the clustering of the
 * scope (SIFT_SMETA_CLUSTERED: members also move from key to key, which moves no row; clustered by primary key the route must still name where the rows are)
 */
extern void tw_edit_round(TCheck *t, TWorld *w, StringInfoData *old, SiftScopeMeta *mold, uint64 epoch, uint32 flags, EditStats *es);
/* a and b are the same cube, but for the bounds a subtraction cannot keep: the time bounds, and the extremes unless the query reads them */
extern bool tw_cube_same(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b, bool extremes);

#endif							/* SIFT_TESTING */
#endif							/* SIFT_COL_TEST_SCOPE_V6_EDIT_H */
