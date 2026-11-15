/*
 * agg/combo/int.h - what the files of agg/combo/ share
 * The entry hash and compare every lookup of a combo inlines, the macros of the caches, and the
 * functions the ids, the children, the keys, the outputs, the entries and the resolve call.
 */
#ifndef SIFT_AGG_COMBO_DIR_INT_H
#define SIFT_AGG_COMBO_DIR_INT_H

#include "agg/agg_int.h"

#define COMBO_MAX_ENTRIES	16777216	/* entries of one combo cache before the caches start again (combo_trim) */

#define CMAP_UNSET	UINT32_MAX

#define WIDE_AHEAD	12

extern uint32 intern_id(AggExec *ex, Intern *t, const char *b, uint32 len);

/* the id of entry c of the dictionary: a dictionary has far more entries than a scan reads (every distinct free text of a column) */
static inline uint32
cmap_id(AggExec *ex, uint32 *map, const SiftSrcDict *d, int ci, int p, uint32 c)
{
	if (map[c] == CMAP_UNSET)
	{
		uint32		len;
		char		buf[SIFT_SRC_DICT_ENTRY];
		const char *str = sift_srcdict_str(d, c, &len, buf);

		map[c] = intern_id(ex, &ex->cs[ci].in[p], str, len);
	}
	return map[c];
}

/* a placeholder of the nullable side reads NULL for a row nothing matched: the missing rows of a join, and the tuples of a LEFT lateral's rows with no element */
static inline bool
combo_null_input(const AggExec *ex)
{
	return ex->null_input || ex->nullpass;
}

extern int combo_child_rows(AggExec *ex, int ci, const uint64 *v, uint32 **rows);

/* test builds run a SQL command at a stage of a probe (sift.test_probe_hook), between two page reads, when no page is locked */
#ifdef SIFT_TESTING
extern void combo_probe_hook(int stage);
#else
#define combo_probe_hook(stage) ((void) 0)
#endif

extern bool combo_probe_on(AggExec *ex, int ci);
extern void combo_probe_defer(AggExec *ex, int ci, uint32 e);
extern void combo_probe_flush(AggExec *ex, int ci);
extern bool combo_probe_take(AggExec *ex, int ci, uint32 *start, int *n);

extern uint64 wide_vid(ComboSt *cs, int p, const SiftSrcLeaf *lf, const SiftSrcVec *v, uint32 i);
extern bool combo_fstep_resolve(AggExec *ex, int ci, int n, const uint32 *idx, uint32 *eid, const uint64 *alive, const SiftSrcLeaf *lf, const SiftSrcVec *v);

extern int combo_matches(AggExec *ex, int ci, const uint64 *v, uint32 **rows);
extern int combo_filter_rows(AggExec *ex, int ci, uint32 *rows, int nm);

extern void combo_make(AggExec *ex, int ci, const uint64 *v, ComboEnt *ent);

extern void id64_init(Id64 *m);
extern uint32 combo_ent(AggExec *ex, int ci, const uint64 *v);

extern uint32 * dict_cmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ci, int p);
extern uint32 id64_id(Id64 *m, int64 k);
extern uint32 id64_nan(Id64 *m);

#endif							/* SIFT_AGG_COMBO_DIR_INT_H */
