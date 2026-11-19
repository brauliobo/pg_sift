/*
 * col/scope.h - scope indexes: a sift_col index on a fact table whose rows are also read by a key the row reaches through a scope relation, a table of (member, scope) pairs ("the
 * entities a tenant sees"). WITH (scope = 'key_name: fk -> relation(member, scope_key)') declares one; opt-in.
 *
 * The rows of the facts are stored once. The entity segments of 16 units or more carry a scope sidecar (col/side.h): the runs of units that hold the rows of the members of each scope key,
 * with the cube of the key's rows. A read of one scope key scans the entity rows of the members the scope relation lists for it, the sidecar's runs standing for their units while the journal
 * of the relation says no pair of their members changed. The rule is the one of col/conn.h.
 */
#ifndef SIFT_COL_SCOPE_H
#define SIFT_COL_SCOPE_H

#include <math.h>

#include "utils/rel.h"
#include "utils/snapshot.h"

#include "col/col.h"
#include "col/conn.h"

/* A relation of a declaration and the two columns it names: (member, scope key) for the pairs, (via, scope key) for the joined path, (via, member) for the via relation */
typedef struct SiftScopeRel
{
	char	   *schema;				/* NULL: the schema of the fact table */
	char	   *name;
	char	   *a;
	char	   *b;
	char	   *index_name;			/* using name: the btree of the relation that defines its rows, NULL: chosen by the rule (sift_scope_resolve) */
} SiftScopeRel;

typedef struct SiftScopeSpec
{
	char	   *key_name;			/* the name of the derived leaf, "<key_name>#scope" */
	char	   *fk;					/* column of the fact index that holds the member key */
	SiftScopeRel rel;				/* the pairs (member, scope_key) */
	SiftScopeRel *joined;			/* two-hop: | rel2(via, scope_key) via vrel(via, member): the pairs are those of rel and, for each row of rel2, the members of vrel with its via */
	SiftScopeRel *via;				/* ... vrel; both NULL for a scope of one path */
	List	   *cols;				/* cols(a, b, c): a 5.0 option the declaration may still state and the parser accepts; ignored, with a NOTICE; NIL: not stated */
	double		max_multiplicity;	/* max_multiplicity(n): a 5.0 option the declaration may still state and the parser accepts; ignored, with a NOTICE; 0: not stated */
	char	   *text;				/* the declaration of this scope as written, without the separator */
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
extern int	sift_scope_leaves(Relation index, SiftColDef *defs, int n, int *leaf, int *nscope);
/* The leaf the units of a scope are cut by after its key: the first date, timestamp or timestamptz column of the index, -1 when there is none */
extern int	sift_scope_time_leaf(const SiftColDef *defs, int n);
/* The checks of a declared scope's relation (ERROR with a hint when it does not qualify); left open in *rel, AccessShare */
extern void sift_scope_relation(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* ... and the btree on (member, scope_key) that defines its pairs: the one the index records (a dependency written by CREATE INDEX and REINDEX), never chosen again by a read */
extern Oid	sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* ... and those of the joined path and of the members of its via column of a scope of two paths (hop 1, 2): their columns (via, scope key) and (via, member) and their recorded btrees */
extern Oid	sift_scope_hop_index(Relation heap, Relation index, const SiftScopeSpec *s, int hop, Relation *rel, AttrNumber *a, AttrNumber *b);
/* How a relation of pairs without a journal gets one: made by a plain CREATE INDEX (holds the writers of the pairs for its build), left for later (a concurrent build before its end), or made empty and valid only after its validation pass */
typedef enum SiftJournalMake
{
	SIFT_JOURNAL_PLAIN,
	SIFT_JOURNAL_DEFER,
	SIFT_JOURNAL_EMPTY
} SiftJournalMake;
/*
 * CREATE INDEX, REINDEX (sticky false: chooses the btree by the rule and records it) and ALTER INDEX (sticky true: leaves the recorded one): resolves the declaration, writes the dependency, or removes it.
 * A relation of pairs without a journal index gets one
 */
extern void sift_scope_resolve(Relation heap, Relation index, bool sticky, SiftJournalMake make);
/* Before the build of an index with a scope: a lowered sift.col_format_minor cannot write one (the units are cut by key, which an older library refuses) */
extern void sift_scope_check_build(Relation index);
/* Is the value store the one of the scope key of an index (the codes of a text or uuid key): no epoch of it begins */
extern bool sift_scope_key_store_pinned(Oid store);
/* After ALTER INDEX of any option of a sift_col index: resolves again, so that scope = 'off' or RESET (scope) of an index with no segment removes the dependency */
extern void sift_scope_alter(Relation index);
/* col/scope_guard.c: what the declaration needs of its relations and journals, said when the index is made or altered */
/* The sift_col indexes with a scope that depend on the index (they took it for the journal or the btree of a relation of their scopes) */
extern List *sift_scope_users(Oid index);
/* col/scope_make.c: the journal index of a relation of pairs that has none, made by CREATE INDEX of the scope index (named by the rule, owned by the table's owner, marked by its comment); dropped with the last scope index that uses it */
extern Oid	sift_scope_journal_make(const SiftScopeSpec *s, Relation rel, int hop, AttrNumber member, AttrNumber key, Oid btree, SiftJournalMake make);
/* The build of the journal that a concurrent build makes reads no row: the rows come in the validation pass (sift_scope_journals_validate) */
extern bool sift_col_journal_empty;
/* Set while validate_index fills such a journal: the pairs it adds were written before the journal began, so it keeps no record of them (the records are those of the writers since) */
extern bool sift_col_journal_validating;
/* The validation pass of a concurrent build: the journals the build made empty are filled from the heap under ShareUpdateExclusiveLock, then made valid, as validate_index does for a CREATE INDEX CONCURRENTLY */
extern void sift_scope_journals_validate(void);
extern bool sift_scope_journal_pending(Oid index);
extern void sift_scope_journal_drop_unused(void);
extern void sift_scope_notice_relation(const char *key_name, const char *name, Relation rel);
extern void sift_scope_journal_alter(Relation index);
/* The scope of the index from its column fk to the relation's columns: its ordinal, the leaf of its derived key and the btree; -1 when the index declares no such scope */
/* The ordinal of the scope key_name of the index (an error when it has none by that name) */
extern int	sift_scope_ordinal(Relation index, const char *key_name);

/*
 * What the planner needs to prove that a query asks for the pairs of a scope (docs/COL-SCOPE.md): the paths that give pairs. A leg is the rows of rel that satisfy pred, the btree
 * that defines them (recorded when the scope segments were written); pred is the predicate of that btree as an implicit-AND list over Vars of varno 1 (NIL: every row).
 * The pairs of a scope are the distinct (member, key) of its legs.
 */
typedef struct SiftScopeLeg
{
	Oid			rel;
	AttrNumber	member;
	AttrNumber	key;
	Oid			btree;
	List	   *pred;
	/* a joined leg (via_rel valid): member is the column that joins via_rel by via_join, via_member the member key of via_rel; the pairs are (via_member, key) of the rows that join */
	Oid			via_rel;
	AttrNumber	via_join;
	AttrNumber	via_member;
	Oid			via_btree;
	List	   *via_pred;
} SiftScopeLeg;
/* Are the pairs of the scope (of one path) a multiset: its recorded btree is not unique (or none is recorded), so a join counts a fact once for each row of a pair */
extern bool sift_scope_pairs_multiset(Relation index, const SiftScopeSpec *s);
/* The legs of scope ordinal scope of the index: 1 (the pairs relation) or 2 (the pairs relation and the joined path); returns their number */
extern int	sift_scope_legs(Relation index, int scope, SiftScopeLeg legs[2]);
/* The same, found in the catalogs (not kept) */
extern int	sift_scope_legs_find(Relation index, int scope, SiftScopeLeg legs[2]);

/*
 * The journal of the scope relation (col/scope_journal.c, INTERFACE.md section 5): a sift_col index of the relation with track_changes = 'ctid' whose first column is the member key and
 * that holds the scope key (the journal index; the lowest oid that qualifies). Its tombstone log keeps a KEYCHG record for the member of every pair an UPDATE, DELETE or key change ended
 * and a KEYADD record for the member of every pair inserted; the rule that reads them is the one of the connections (col/conn.h, docs/COL-CONNECT.md). The scope is the ordinal of the
 * scope in the declaration of the fact index; a scope whose relation has no journal index vouches for nothing (SIFT_CONN_NO_JOURNAL).
 *
 * A scope of two paths depends on three journals, each with a stamp of its own (column format minor 49): J1, the pairs relation keyed by its member; J2, the joined relation keyed by its via
 * column (the groups whose keys changed); J3, the via relation keyed by its member column (the members whose groups changed). The members whose pairs changed are those of J1 and J3 and the
 * members of every group of J2, read from the recorded btree of the via relation under the reader's snapshot. The stamps of a scope are an array of its journals (sift_scope_njournals).
 */
#define sift_scope_njournals(s) ((s)->joined != NULL ? SIFT_SCOPE_JOURNALS : 1)
/* The journal index of the relation for the pairs defined by pair_btree: a plain index, or one whose predicate the predicate of pair_btree implies (every pair is in it); InvalidOid when it has none */
extern List *sift_scope_index_predicate(Oid indexoid);
extern Oid	sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree);
/* ... and the one whose leaves list exactly the pairs, InvalidOid when there is none */
extern Oid	sift_scope_pairs_journal(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree);
/* ... of relation hop of the declaration (0 the pairs relation, 1 the joined one, 2 the via one) with the columns (a, b) it names and the btree that defines its rows: the journaled column is a, and b for the via relation (its member) */
extern Oid	sift_scope_hop_journal(Relation rel, int hop, AttrNumber a, AttrNumber b, Oid btree);
/* Does the index declare a scope of two paths (column format minor 49) */
extern bool sift_scope_two_paths(Relation index);
/* Does the index declare a scope whose key or member is a text, varchar or uuid column (column format minor 50) */
extern bool sift_scope_coded(Relation index);
extern bool sift_scope_member_text(Relation index, int scope);
/*
 * The journals of the scopes of the index, made with the writers of the scope relations held out to the end of the statement. read_after: the build reads the relations under snapshots it takes
 * after this call, so that it stamps the segments with the journal it makes here (else the first scope index of a relation says "unfilled" until a REINDEX)
 */
extern void sift_scope_start(Relation heap, Relation index, bool read_after);
/* The stamps of the journals of the scope at snap (the journals of the scope, SIFT_SCOPE_JOURNALS at most): fill, promotion and merge take them before they read the relations (lineage 0 and the reason when a journal cannot vouch) */
extern SiftConnWhy sift_scope_take(Relation index, int scope, Snapshot snap, SiftConnStamp *stamp);
/* May a derivation stamped so be read by snap: every journal vouches for its stamp */
extern SiftConnWhy sift_scope_vouch(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap);
/*
 * The members whose pairs changed in (X, S], ended or inserted: sorted, unique, at most cap (SIFT_CONN_OVERFLOW), the keys palloc'd in the current context (sift_conn_keys_free). Of two paths: those of
 * the journals of the pairs relation and of the via relation and the members of every group of the joined relation's journal that the via relation lists under snap. keys: only the changes of the
 * pairs of those scope keys (ascending, distinct; NULL: any key): a journal made for the scope key column tags its records with the key of their pair, and the pairs of the other keys change no unit of these.
 */
extern SiftConnWhy sift_scope_changed(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, SiftConnKeys *out);

/* A pair of the scope relation: the member key and the scope key */
typedef struct SiftScopePair
{
	int64		member;
	int64		key;
} SiftScopePair;
/* Every distinct pair of scope ord of a scope of one path under snap, ascending by (key, member): the pairs the scope sidecars (col/side.h) are made from. A text, varchar or uuid key is its code in the store of the key column, which the call appends the keys the store lacks to (a writer's); a text, varchar or uuid member is its code (negative) in *members, a map of the call's own that holds the member values of the pairs (NULL for an integer member) */
extern SiftScopePair *sift_scope_all_pairs(Relation heap, Relation index, int ord, Snapshot snap, uint64 *n, SiftKeyMap **members);

/* The pairs of the rows of rel (under snap) whose column key is one of the nkeys keys (ascending; NULL: every row), read from the leaves of its sift_col index ix (agg/lookcol.c; a text, varchar or uuid column as the codes of its map mm or km); false: the index cannot give them */
extern bool lookcol_pairs(Relation rel, Oid ix, Snapshot snap, int member, int key, SiftKeyMap *mm, SiftKeyMap *km, const int64 *keys, int nkeys, SiftScopePair **out, uint32 *n);

/*
 * What the planner and the executor take a read of a scope key to cost, in instructions of the backend (execution of a prepared statement, warm; the fits are by relative least squares): the read
 * before any unit (the directory and the statement: 450,000), a unit read row by row (36,700) or answered from its pre-aggregates, or a run of the sidecar (13,300), an entry read, tested and
 * aggregated (18 and what its operators cost), the entity row of a member tested against the members listed (70) and each pair of the relation listed (25, 300 where only its btree lists it).
 */
#define SIFT_SCOPE_SETUP_INSTR		450000
#define SIFT_SCOPE_UNIT_INSTR		36700
#define SIFT_SCOPE_PREAGG_UNIT_INSTR	13300
#define SIFT_SCOPE_ENTRY_INSTR		18
#define SIFT_SCOPE_MATCH_INSTR		70
#define SIFT_SCOPE_SCAN_PAIR_INSTR	300
#define SIFT_SCOPE_PAIR_INSTR		25

/* The counts the price of a read takes: the keys, the entries they stand for, the instructions one entry's aggregation costs, and the pairs of the relations a read lists */
typedef struct SiftScopeCounts
{
	uint32		nkeys;
	uint64		entries;		/* the rows of the keys */
	double		entry_instr;	/* the instructions the aggregation of an entry costs on top of reading it (the operators of the model a row has, which the join pays too: SiftAggScope.entry_instr) */
	uint64		pairs;			/* the pairs of the relations whose journal index lists them (the leaves hold only pairs), which the read lists the members of the keys from */
	uint64		scan_pairs;		/* ... and the rows of those whose pairs only their btree lists (a partial btree over a plain journal index) */
} SiftScopeCounts;

/*
 * The weights every price of maintenance is made in (instructions): a MB of WAL weighs 0.6 G (the price of a row of the rewrite, the fold of the marks, the VACUUM a read asks for), and a block a read takes from the file
 * instead of the buffers 20,000 (the part of the price of a unit of the rewrite that is its block: 46,000 less the 26,000 to open and decode it). A block found in the buffers is in the instructions of the read already.
 */
#define MAINT_WAL_BYTE		600.0
#define MAINT_BLOCK_READ	20000.0

/* The member keys that have a pair with one of the keys under the snapshot (keys NULL: with any), ascending, each once */
/* The groups (via values) the joined relation lists under snap for the keys (sorted, distinct), of a scope of two paths */
extern int64 *sift_scope_groups(Relation heap, Relation index, int ord, SiftKeyMap *km, Snapshot snap, const int64 *keys, uint32 nkeys, uint32 *n);
/* ... of a text, varchar or uuid member the codes (negative, ascending) of the map mm, one for the whole statement (sift_scope_member_map; NULL: an integer member) */
extern int64 *sift_scope_members(Relation heap, Relation index, const char *key_name, SiftKeyMap *km, SiftKeyMap *mm, Snapshot snap, const int64 *keys, int nkeys, uint32 *n);
extern SiftKeyMap *sift_scope_member_map(Relation heap, Relation index, const char *key_name);
/* The map of the codes of the scope key (NULL for an integer key): one for each statement that reads the scope, which every key, pair and group of the read goes through (col/scope_keys.c) */
extern SiftKeyMap *sift_scope_keymap(Relation heap, Relation index, const char *key_name);
/* The rows of the relations a read lists the pairs of the scope from (the estimate of each): the pairs relation, and for two paths the joined and the via relations; *scanned of them those whose pairs the leaves of no journal index list */
extern double sift_scope_relations_rows(Relation heap, Relation index, const char *key_name, double *scanned);
/* The relation of pairs of the scope (open, AccessShare) with its member and scope key columns, and the journal index of the relation: a sift_col index that holds both columns */
extern Oid	sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey);

extern PGDLLIMPORT bool sift_agg_scope;
extern PGDLLIMPORT int sift_col_scope_refresh_pct;

#ifdef SIFT_TESTING
/* The journals of a scope opened with an ERROR raised before the fail_at-th open and closed again (sift._test_scope_journals_open) */
extern int	sift_scope_journals_probe(Relation index, int scope, int fail_at, bool *failed);
#endif

#endif							/* SIFT_COL_SCOPE_H */
