/*
 * col/scope.h - scope indexes: a sift_col index on a fact table whose rows are also cut by a key the row reaches through a scope relation, a table of (member, scope)
 * pairs ("the entities a tenant sees"). WITH (scope = 'key_name: fk -> relation(member, scope_key)') declares one; EXPERIMENTAL, opt-in, column format minor 45.
 *
 * Layout: the entity segments are as ever. For each scope the index also holds scope segments: key-cut segments (flag SIFT_SEG_SCOPE) whose rows are the fact
 * rows replicated once per scope key of their fk, cut by (scope key, time), each entry with the heap address of its fact row. The derived leaf "<key_name>#scope"
 * (SIFT_COLD_SCOPE) holds the scope key; it is NULL in an entity segment. A read of one scope key reads the scope units of that key (not the table), less the
 * rows of the members that changed in the scope relation since the stamp, which it reads through the entity path. The rule is the one of col/conn.h.
 */
#ifndef SIFT_COL_SCOPE_H
#define SIFT_COL_SCOPE_H

#include "utils/rel.h"
#include "utils/snapshot.h"

#include "col/col.h"
#include "col/conn.h"

#define SIFT_SCOPE_MAX		4		/* scopes of one index */

typedef struct SiftScopeSpec
{
	char	   *key_name;			/* the name of the derived leaf, "<key_name>#scope" */
	char	   *fk;					/* column of the fact index that holds the member key */
	char	   *rel_schema;			/* NULL: the schema of the fact table */
	char	   *rel_name;
	char	   *member;				/* columns of the scope relation */
	char	   *scope_key;
} SiftScopeSpec;

typedef struct SiftScopeSpecs
{
	int			n;
	SiftScopeSpec *s[SIFT_SCOPE_MAX];
} SiftScopeSpecs;

/* Parses the option text without the catalog; NULL with *err set to what is wrong */
extern SiftScopeSpecs *sift_scope_parse(const char *text, const char **err);
/* The scopes an index declares (ERROR for text that does not parse), NULL when it declares none */
extern SiftScopeSpecs *sift_scope_specs(Relation index);
/* The leaves a scope index adds after its own and the connection leaves: one "<key_name>#scope" per scope. Appended to defs from n; returns the new count, leaf[] the leaf of each scope */
extern int	sift_scope_leaves(Relation index, SiftColDef *defs, int n, int *leaf, int *nscope);
/* The scope relation and its unique btree on (member, scope_key) of a declared scope (ERROR with a hint when the relation does not qualify); the relation is left open in *rel, AccessShare */
extern Oid	sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* CREATE INDEX, REINDEX and ALTER INDEX: resolves the declaration, writes the dependency on the unique btree, or removes it */
extern void sift_scope_resolve(Relation heap, Relation index);
/* After ALTER INDEX of any option of a sift_col index: resolves again, so that scope = 'off' or RESET (scope) removes the dependency */
extern void sift_scope_alter(Relation index);
/* The scope of the index from its column fk to the relation's columns: its ordinal, the leaf of its derived key and the unique btree; -1 when the index declares no such scope */
extern int	sift_scope_find(Relation heap, Relation index, AttrNumber fk, Oid rel, AttrNumber member, AttrNumber scope_key, int *leaf, Oid *btree);

/*
 * The fill: the writer of a build, promotion or merge expands each batch of fact rows by the scope keys of their fk under one stamp of the scope relation (col/scope_fill.c).
 * expand: for the nrows of the batch, the number of scope entries of each row (out_n[row]); the pairs are then taken in order by scope_fill_keys (n entries, row[i], key[i]).
 */
struct SiftScopeFill;
extern struct SiftScopeFill *sift_scope_fill_begin(Relation index, MemoryContext mcxt, uint64 rows);
extern void sift_scope_fill_end(struct SiftScopeFill *f);
extern SiftConnStamp sift_scope_fill_stamp(const struct SiftScopeFill *f, int scope);
/* The leaf of the fact index that holds the member key of the scope */
extern int	sift_scope_fill_fk_leaf(const struct SiftScopeFill *f, int scope);
/* The fill the writers of segments of the index are given: its stamps are those of the connections (conn, nconn of them: NULL and 0 for none, whose fill it also fills and ends) and then those of the scopes */
struct SiftColFill;
extern struct SiftColFill *sift_scope_fill_hook(struct SiftScopeFill *f, struct SiftColFill *conn, int nconn);
/* The scope entries of the first nrows fk values (nulls: bit set = NULL): fills row[] (index of the fact row in the batch) and key[] with the pairs, returns their number; *cap is grown through repalloc */
extern uint32 sift_scope_fill_expand(struct SiftScopeFill *f, int scope, const int64 *fk, const uint64 *nulls, uint32 nrows, uint32 **row, int64 **key, uint32 *cap);

#ifdef SIFT_TESTING
/* A fill of one scope read from a relation and its unique btree on (member, key), without a declaration (sift._test_scope_expand) */
extern struct SiftScopeFill *sift_scope_fill_begin_rel(MemoryContext mcxt, Relation rel, Oid btree, AttrNumber member, AttrNumber key, uint64 rows);
extern PGDLLIMPORT bool sift_test_scope_trust;	/* a scope without a journal is read as if it vouched (the proof of concept) */
#endif
extern PGDLLIMPORT bool sift_agg_scope;
extern PGDLLIMPORT int sift_scope_patch_keys;

#endif							/* SIFT_COL_SCOPE_H */
