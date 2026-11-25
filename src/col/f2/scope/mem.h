/*
 * col/f2/scope/mem.h - a scope whose member is a text, varchar or uuid column in format major 2 (docs/COL-SCOPE.md "Key types"). The summary names a member by an int8, as for an integer member: its code in
 * the scope key store of the member column of the pairs relation, which a writer (the build, the refresh) fills with every member of the pairs and a reader only looks in. A row whose member the store lacks
 * and no pair has gained since is owned by no key, and leaves the cubes as a row of no member does.
 */
#ifndef SIFT_F2_SCOPE_MEM_H
#define SIFT_F2_SCOPE_MEM_H

#include "col/f2/scope/int.h"

typedef struct SiftF2Mem SiftF2Mem;

/* Is the leaf a text, varchar or uuid column a member can be */
extern bool sift_f2s_mem_leaf(const SiftColDef *d);

/* The codes of the member column of the pairs relation of the scope, NULL for an integer member (which reads nothing of the pairs relation). write: the store is made when the index has none, and the pairs of the scope intern their members in it */
extern SiftF2Mem *sift_f2s_mem_open(Relation heap, Relation index, const SiftF2Store *store, const SiftF2ScopeCfg *sc, bool write);

/* The store open for a loop that asks for the codes of many rows (every hold is released): sift_f2s_mem_code and sift_f2s_mem_slab need it */
extern void sift_f2s_mem_hold(SiftF2Mem *mem);
extern void sift_f2s_mem_release(SiftF2Mem *mem);

/* The map the pairs are read through (NULL: an integer member) */
extern SiftKeyMap *sift_f2s_mem_map(const SiftF2Mem *mem);

/* Does the map hold a member of its own that no store has (a pair gained it since the summary): what it names is not for another statement to keep */
extern bool sift_f2s_mem_transient(const SiftF2Mem *mem);

/* The code of a member value; false: no pair has it, now or when the summary was made */
extern bool sift_f2s_mem_code(const SiftF2Mem *mem, const char *s, uint32 len, int64 *code);

/* The members of the rows of a granule as their codes, the rows at[0..n) or all of its n rows; a row of no member (NULL, or no pair has it) is flagged in *nulls */
extern const int64 *sift_f2s_mem_ints(SiftF2Mem *mem, const SiftGranuleView *v, int leaf, const uint16 *at, uint32 n, const uint8 **nulls);

/* The members the journal names by their words (ascending, as it gives them for a text, varchar or uuid member) as their codes (ascending): the members of the store and of the pairs now whose word is one of them,
   which are those and the members that share a word. false when they are more than a read is worth, with nothing changed. An integer member is its own word: true, as it is */
extern bool sift_f2s_mem_translate(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, int64 **changed, uint32 *nchanged);

/* The member of row r of a slab (the store held for the member codes of a text member): false for none */
extern bool sift_f2s_mem_slab(const SiftF2ScopeCfg *sc, const SiftRowBuf *rb, int r, int64 *member);

#endif							/* SIFT_F2_SCOPE_MEM_H */
