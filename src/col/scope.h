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

typedef struct SiftScopeSpec
{
	char	   *key_name;			/* the name of the derived leaf, "<key_name>#scope" */
	char	   *fk;					/* column of the fact index that holds the member key */
	char	   *rel_schema;			/* NULL: the schema of the fact table */
	char	   *rel_name;
	char	   *member;				/* columns of the scope relation */
	char	   *scope_key;
	List	   *cols;				/* cols(a, b, c): the columns of the index its scope segments keep (with the member key and the time column of the cut); NIL: all of them */
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
/*
 * The leaves a scope index adds after its own and the connection leaves: one "<key_name>#scope" per scope. Appended to defs from n; returns the new count, leaf[] the leaf of each scope,
 * kept[s][leaf]: the leaves the scope segments of scope s store values of (the others are empty in them: no chunk, no directory, no summary)
 */
extern int	sift_scope_leaves(Relation index, SiftColDef *defs, int n, int *leaf, int *nscope, bool (*kept)[SIFT_COL_MAX_COLS]);
/* The leaf the units of a scope are cut by after its key: the first date, timestamp or timestamptz column of the index, -1 when there is none */
extern int	sift_scope_time_leaf(const SiftColDef *defs, int n);
/* The scope relation and its unique btree on (member, scope_key) of a declared scope (ERROR with a hint when the relation does not qualify); the relation is left open in *rel, AccessShare */
extern Oid	sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* CREATE INDEX, REINDEX and ALTER INDEX: resolves the declaration, writes the dependency on the unique btree, or removes it */
extern void sift_scope_resolve(Relation heap, Relation index);
/* After ALTER INDEX of any option of a sift_col index: resolves again, so that scope = 'off' or RESET (scope) removes the dependency */
extern void sift_scope_alter(Relation index);
/* The scope of the index from its column fk to the relation's columns: its ordinal, the leaf of its derived key and the unique btree; -1 when the index declares no such scope */
extern int	sift_scope_find(Relation heap, Relation index, AttrNumber fk, Oid rel, AttrNumber member, AttrNumber scope_key, int *leaf, Oid *btree);

/* The cut of the units of the segments of a scope: cut[] (SIFT_COL_CUT_MAX leaves) gets its derived key and the time column of the index if it has one; returns their number */
struct SiftColRel;
extern int	sift_scope_cut(const struct SiftColRel *r, int scope, int *cut);

/*
 * The journal of the scope relation (col/scope_journal.c, INTERFACE.md section 5): a sift_col index of the relation with track_changes = 'ctid' whose first column is the member key and
 * that holds the scope key (the journal index; the lowest oid that qualifies). Its tombstone log keeps a KEYCHG record for the member of every pair an UPDATE, DELETE or key change ended
 * and a KEYADD record for the member of every pair inserted; the rule that reads them is the one of the connections (col/conn.h, docs/COL-CONNECT.md). The scope is the ordinal of the
 * scope in the declaration of the fact index; a scope whose relation has no journal index vouches for nothing (SIFT_CONN_NO_JOURNAL).
 */
/* The journal index of the relation for the pair (member, scope_key); InvalidOid when it has none */
extern Oid	sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key);
extern bool sift_scope_has_journal(Relation index, int scope);
/* The journals of the scopes of the index, made when its build has read the table (the writers of the scope relation are held out to the end of the statement) */
extern void sift_scope_start(Relation heap, Relation index);
/* The stamp of the scope relation's journal at snap: fill, promotion and merge take it before they read the relation (lineage 0 and the reason when the journal cannot vouch) */
extern SiftConnWhy sift_scope_take(Relation index, int scope, Snapshot snap, SiftConnStamp *stamp);
/* May a derivation stamped so be read by snap */
extern SiftConnWhy sift_scope_vouch(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap);
/* The members whose pairs changed in (X, S], ended or inserted: sorted, unique, at most cap (SIFT_CONN_OVERFLOW), the keys palloc'd in the current context (sift_conn_keys_free) */
extern SiftConnWhy sift_scope_changed(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out);

/*
 * Refresh: is a scope segment of the index worth a rewrite for what its stamps say (sift_conn_stale for the scopes)? NULL from begin: the index declares no scope. The reason in
 * sift.col_maintenance_due is "stale connection".
 */
typedef struct SiftScopeStale SiftScopeStale;
extern SiftScopeStale *sift_scope_stale_begin(Relation index, MemoryContext mcxt);
struct SiftColSegment;
extern bool sift_scope_stale(SiftScopeStale *st, const struct SiftColSegment *seg);
extern void sift_scope_stale_end(SiftScopeStale *st);

/*
 * The fill: the writer of a build, promotion or merge expands each batch of fact rows by the scope keys of their fk under one stamp of the scope relation (col/scope_fill.c).
 * expand: for the nrows of the batch, the number of scope entries of each row (out_n[row]); the pairs are then taken in order by scope_fill_keys (n entries, row[i], key[i]).
 */
struct SiftScopeFill;
extern struct SiftScopeFill *sift_scope_fill_begin(Relation index, MemoryContext mcxt, uint64 rows);
extern void sift_scope_fill_end(struct SiftScopeFill *f);
/* A fill that expands nothing and gives the n scopes the stamps it is told (the writers of a merge); sift_scope_stamp_older: the stamp of a merge of segments stamped a and b */
extern struct SiftScopeFill *sift_scope_fill_carry(MemoryContext mcxt, int n, const SiftConnStamp *stamp);
extern SiftConnStamp sift_scope_stamp_older(SiftConnStamp a, SiftConnStamp b);
extern SiftConnStamp sift_scope_fill_stamp(const struct SiftScopeFill *f, int scope);
/* The leaf of the fact index that holds the member key of the scope */
extern int	sift_scope_fill_fk_leaf(const struct SiftScopeFill *f, int scope);
/* The fill the writers of segments of the index are given: its stamps are those of the connections (conn, nconn of them: NULL and 0 for none, whose fill it also fills and ends) and then those of the scopes */
struct SiftColFill;
extern struct SiftColFill *sift_scope_fill_hook(struct SiftScopeFill *f, struct SiftColFill *conn, int nconn);
/* The scope entries of the first nrows fk values (nulls: bit set = NULL): fills row[] (index of the fact row in the batch) and key[] with the pairs, returns their number; *cap is grown through repalloc */
extern uint32 sift_scope_fill_expand(struct SiftScopeFill *f, int scope, const int64 *fk, const uint64 *nulls, uint32 nrows, uint32 **row, int64 **key, uint32 *cap);

/* Does every entity segment of the directory have its scope entries in the scope segments (SIFT_SEG_SCOPED; rows of a write buffer are the reader's: it reads them through the entity path) */
extern bool sift_scope_covered(Relation index, int scope);

/*
 * The reader (col/scan/scope.c): what a scan that has a scope conjunct of nkeys keys of the scope key_name reads. A row of the scope units of the keys counts when its member
 * is not in except (the members whose pairs changed since the stamp, and those that have a row in a write buffer, which no scope entry describes); the entity rows of the
 * members of patch (those of except that have a pair with a key now) are read besides, through the entity path, so that the aggregates see one stream. With once and several keys,
 * the row of a member that has a pair with several of the keys counts in the units of the least of them only: drop[i] lists, for the key drop[i].key, the members whose rows
 * of that key's units are not counted. why != NULL: the scan reads the entity rows of the members the scope relation lists for the keys (sift_scope_members) instead; the word
 * is what EXPLAIN prints after "Scope Fallback". A leaf the scan reads (leaves[0..nleaves)) that the scope segments keep no values of (cols(..) of the scope) is a fallback. topn: the scan is a top-N, which does not read the units of both families as one.
 */
typedef struct SiftScopeDrop
{
	int64		key;
	int64	   *members;
	uint32		n;
} SiftScopeDrop;

typedef struct SiftScopeRead
{
	const char *why;
	int64	   *except;
	uint32		nexcept;
	int64	   *patch;
	uint32		npatch;
	uint32		nchanged;		/* the members whose pairs changed since the stamp, of except */
	SiftScopeDrop *drop;
	int			ndrop;
} SiftScopeRead;

extern SiftScopeRead *sift_scope_read(Relation heap, Relation index, Snapshot snap, const char *key_name, const int64 *keys, int nkeys, bool once, bool topn,
								   const int *leaves, int nleaves);
/* The member keys that have a pair with one of the keys under the snapshot, ascending, each once */
/* The columns of the index the scope segments of key_name keep values of, of all its columns (EXPLAIN "Scope Columns") */
extern void sift_scope_columns(Relation index, const char *key_name, int *kept, int *all);
extern int64 *sift_scope_members(Relation heap, Relation index, const char *key_name, Snapshot snap, const int64 *keys, int nkeys, uint32 *n);

#ifdef SIFT_TESTING
/* A fill of one scope read from a relation and its unique btree on (member, key), without a declaration (sift._test_scope_expand) */
extern struct SiftScopeFill *sift_scope_fill_begin_rel(MemoryContext mcxt, Relation rel, Oid btree, AttrNumber member, AttrNumber key, uint64 rows);
extern PGDLLIMPORT int sift_test_scope_unit_kind;	/* the unit kind the stamps of the scope segments are read as having, ORed into the one they hold (0: none): what a segment of a later release says */
extern PGDLLIMPORT bool sift_test_scope_trust;	/* a scope without a journal is read as if it vouched (the proof of concept) */
extern PGDLLIMPORT bool sift_test_scope_need_journal;	/* a scope whose relation has no journal index is refused at CREATE INDEX (a release always does) */
extern PGDLLIMPORT bool sift_test_scope_pairs;	/* the marker sift.in_scope reads a row once for each pair, as a join does, not once */
#endif
extern PGDLLIMPORT bool sift_agg_scope;
extern PGDLLIMPORT int sift_scope_patch_keys;
extern PGDLLIMPORT double sift_scope_max_multiplicity;

#endif							/* SIFT_COL_SCOPE_H */
