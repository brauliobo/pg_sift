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
/* The checks of a declared scope's relation (ERROR with a hint when it does not qualify); left open in *rel, AccessShare */
extern void sift_scope_relation(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* ... and the btree on (member, scope_key) that defines its pairs: the one the index records (a dependency written by CREATE INDEX and REINDEX), never chosen again by a read */
extern Oid	sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key);
/* ... and those of the joined path and of the members of its via column of a scope of two paths (hop 1, 2): their columns (via, scope key) and (via, member) and their recorded btrees */
extern Oid	sift_scope_hop_index(Relation heap, Relation index, const SiftScopeSpec *s, int hop, Relation *rel, AttrNumber *a, AttrNumber *b);
/* CREATE INDEX, REINDEX (sticky false: chooses the btree by the rule and records it) and ALTER INDEX (sticky true: leaves the recorded one): resolves the declaration, writes the dependency, or removes it */
extern void sift_scope_resolve(Relation heap, Relation index, bool sticky);
/* After ALTER INDEX of any option of a sift_col index: resolves again, so that scope = 'off' or RESET (scope) removes the dependency */
extern void sift_scope_alter(Relation index);
/* The scope of the index from its column fk to the relation's columns: its ordinal, the leaf of its derived key and the btree; -1 when the index declares no such scope */
extern int	sift_scope_find(Relation heap, Relation index, AttrNumber fk, Oid rel, AttrNumber member, AttrNumber scope_key, int *leaf, Oid *btree);

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

/* The cut of the units of the segments of a scope: cut[] (SIFT_COL_CUT_MAX leaves) gets its derived key and the time column of the index if it has one; returns their number */
struct SiftColRel;
extern int	sift_scope_cut(const struct SiftColRel *r, int scope, int *cut);

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
extern Oid	sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree);
/* ... and the one whose leaves list exactly the pairs, InvalidOid when there is none */
extern Oid	sift_scope_pairs_journal(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree);
/* ... of relation hop of the declaration (0 the pairs relation, 1 the joined one, 2 the via one) with the columns (a, b) it names and the btree that defines its rows: the journaled column is a, and b for the via relation (its member) */
extern Oid	sift_scope_hop_journal(Relation rel, int hop, AttrNumber a, AttrNumber b, Oid btree);
extern bool sift_scope_has_journal(Relation index, int scope);
/* Does the index declare a scope of two paths (column format minor 49) */
extern bool sift_scope_two_paths(Relation index);
/* The journals of the scopes of the index, made when its build has read the table (the writers of the scope relation are held out to the end of the statement) */
extern void sift_scope_start(Relation heap, Relation index);
/* The stamps of the journals of the scope at snap (the journals of the scope, SIFT_SCOPE_JOURNALS at most): fill, promotion and merge take them before they read the relations (lineage 0 and the reason when a journal cannot vouch) */
extern SiftConnWhy sift_scope_take(Relation index, int scope, Snapshot snap, SiftConnStamp *stamp);
/* May a derivation stamped so be read by snap: every journal vouches for its stamp */
extern SiftConnWhy sift_scope_vouch(Relation index, int scope, const SiftConnStamp *stamp, Snapshot snap);
/*
 * The members whose pairs changed in (X, S], ended or inserted: sorted, unique, at most cap (SIFT_CONN_OVERFLOW), the keys palloc'd in the current context (sift_conn_keys_free). Of two paths: those of
 * the journals of the pairs relation and of the via relation and the members of every group of the joined relation's journal that the via relation lists under snap.
 */
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
/* A fill that expands nothing and gives the scopes of the index the stamps it is told (the writers of a merge); sift_scope_stamp_older: the stamp of a merge of segments stamped a and b */
extern struct SiftScopeFill *sift_scope_fill_carry(Relation index, MemoryContext mcxt, const SiftConnStamp (*stamp)[SIFT_SCOPE_JOURNALS]);
extern SiftConnStamp sift_scope_stamp_older(SiftConnStamp a, SiftConnStamp b);
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
 * is not in except (the members whose pairs changed since the stamp): the source leaves their stored entries out (sift_col_scope_except). The rows no scope entry describes are the delta:
 * those of the write buffers and the live rows of the entity segments of the members of patch (those of except that have a pair with a key now, read by member key), each read once
 * for each of its member's pairs with a key under the statement's snapshot (sift_col_scope_delta). With once and several keys,
 * the row of a member that has a pair with several of the keys counts in the units of the least of them only: drop[i] lists, for the key drop[i].key, the members whose rows
 * of that key's units are not counted, and the delta copies a row for the least key only. why != NULL: the scan reads the entity rows of the members the scope relation lists for the keys
 * (sift_scope_members) instead; the word is what EXPLAIN prints after "Scope Fallback". keys NULL (with !once): every key, a row for each of its member's pairs, which a scan groups by the derived leaf
 * "<key_name>#scope" (every row the scan reads has its key then). A leaf the scan reads (leaves[0..nleaves)) that the scope segments keep no values of (cols(..) of the scope) is a fallback.
 * topn: the scan is a top-N, which does not read the units of both families as one.
 */
typedef struct SiftScopeDrop
{
	int64		key;
	int64	   *members;
	uint32		n;
} SiftScopeDrop;

/* A pair of the scope relation: the member key and the scope key */
typedef struct SiftScopePair
{
	int64		member;
	int64		key;
} SiftScopePair;
/* Sorts the pairs by (member, key) and keeps each once: the one place that turns the rows of a scope relation, which may hold a pair in several rows, into pairs; returns how many there are */
extern uint64 sift_scope_pairs_distinct(SiftScopePair *p, uint64 n);

/* The write buffers of an index: the batches of the live rows of each, as the backend keeps them (a scan reads them the same way), and what the scan stamps their rows with (col/scan/scope_delta.c) */
struct SiftColBatch;
typedef struct SiftScopeBuffer
{
	struct SiftColBatch **b;
	int			n;
	TransactionId stamp;
	bool		roots;
	bool		lane;			/* the rows are checked against the heap whatever the map says: a write buffer's, or rows decoded from a segment */
	uint64	  **rows;			/* the rows of b[i] the buffer holds, as bits (NULL: all of them): the buffers made from the rows of a segment share their batches */
} SiftScopeBuffer;

struct SiftDirSnapshot;
struct SiftScopeDelta;
extern SiftScopeBuffer *sift_scope_buffers(Relation index, const struct SiftDirSnapshot *dir, int *n);
extern uint64 sift_scope_buffers_rows(const SiftScopeBuffer *bufs, int nbufs);
/* The member keys of the rows of the buffers (not distinct, not sorted); *n of them */
extern int64 *sift_scope_buffers_members(Relation index, int scope, const SiftScopeBuffer *bufs, int nbufs, uint32 *n);
/*
 * The live rows of the entity segments whose member key is one of the members (ascending), read by member key: first a plan, from the point directory of the member leaf of each segment (the rows it
 * lists) or, for a segment with none, from the zones of the leaf (the units it leaves open are read whole): *rows the rows the members are expected to have there, *scanned those read to find them (rows_per_member
 * is what the segments of the relation have of a member on average); then the rows themselves, decoded with the leaves[0..nleaves) the scan reads and added to bufs as buffers of their own, those on pages the
 * visibility map holds all-visible apart from those the heap must check: *rows of them, bufs grown and returned
 */
struct SiftScopeSegPlan;
typedef struct SiftScopeSegPlan SiftScopeSegPlan;
extern SiftScopeSegPlan *sift_scope_segment_plan(Relation index, int scope, const struct SiftDirSnapshot *dir, const int64 *members, uint32 n, double rows_per_member, uint64 *rows, uint64 *scanned,
												  uint64 *units);
extern SiftScopeBuffer *sift_scope_segment_rows(SiftScopeSegPlan *plan, Relation heap, Relation index, Snapshot snap, int scope, const struct SiftDirSnapshot *dir, SiftScopeBuffer *bufs, int *nbufs,
												 const int *leaves, int nleaves, uint64 *rows);
/* The sets of b after those of a */
extern struct SiftScopeDelta *sift_scope_delta_merge(const struct SiftScopeDelta *a, const struct SiftScopeDelta *b);
/* The rows of the buffers whose member has one of the pairs (ascending by member), a copy for each, with the values of the leaves[0..nleaves) the scan reads: *rows of them */
extern struct SiftScopeDelta *sift_scope_delta_select(Relation index, int scope, const SiftScopeBuffer *bufs, int nbufs, const SiftScopePair *pairs, uint32 npairs, const int *leaves, int nleaves, uint64 *rows);

/* The pairs of the rows of rel (under snap) whose column key is one of the nkeys keys (ascending; NULL: every row), read from the leaves of its sift_col index ix (agg/lookcol.c); false: the index cannot give them */
extern bool lookcol_pairs(Relation rel, Oid ix, Snapshot snap, int member, int key, const int64 *keys, int nkeys, SiftScopePair **out, uint32 *n);

typedef struct SiftScopeRead
{
	const char *why;
	int			scope;			/* the ordinal of the scope */
	int64	   *except;
	uint32		nexcept;
	int64	   *patch;			/* the members of except that have a pair with a key now: their rows are read by member key from the entity segments and are the delta of their pairs */
	uint32		npatch;
	uint64		patch_rows;		/* the rows of the entity segments read for them (before they are copied for each pair): expected until sift_scope_read_rows, then as read */
	uint64		patch_scan;		/* the rows of the entity segments read to find them: no point directory of the member leaf tells where they are */
	uint64		patch_units;	/* the units of the entity segments the rows are read from: each costs its chunks of the leaves the scan reads */
	struct ScopeFill *fill;		/* what sift_scope_read_rows needs to read them */
	uint32		nchanged;		/* the members whose pairs changed since the stamp, of except */
	SiftScopeDrop *drop;
	int			ndrop;
	int64	   *members;		/* the members the scope relation lists for the keys, when the read listed them to price the patch: the entity path takes them */
	uint32		nmembers;
	uint64		buffer_rows;	/* the live rows of the write buffers */
	struct SiftScopeDelta *delta;	/* the rows of the buffers and the segment rows of the patched members, copied for each of the pairs their member has with the keys: what the source reads of them */
	uint64		delta_rows;		/* ... and how many copies they are */
} SiftScopeRead;

/*
 * What a read of a scope key costs, in instructions of the backend (one cluster, 300,000 events, 5,000 members, 50 keys, units cut by time: every unit of the entity segments holds every member;
 * measured warm, the median of 5 runs): the scope read before any row (the journal's vouch and changed members, the directory, the scope units opened), a scope entry read and aggregated, a
 * unit of the entity segments the rows of the changed members are read from (its chunks of the leaves the scan reads and the pages they are on), an entity row looked at to find them when no point
 * directory of the member leaf names them, an entity row the units of the facts hold that the key filter of a list of members tests, a row it keeps (read and aggregated), a pair read from the leaves of the journal index of the scope relation, and a changed member (its journal record, its probe in the btree of pairs and
 * the heap row it finds).
 */
#define SIFT_SCOPE_SETUP_INSTR		1400000
#define SIFT_SCOPE_ENTRY_INSTR		20
#define SIFT_SCOPE_PATCH_UNIT_INSTR	196000
#define SIFT_SCOPE_SCAN_INSTR		10
#define SIFT_SCOPE_ENTITY_INSTR		10
#define SIFT_SCOPE_MATCH_INSTR		35
#define SIFT_SCOPE_PAIR_INSTR		25
#define SIFT_SCOPE_MEMBER_INSTR		5600
/* an entity row of an index read whole, against which the changed members one read lists are capped (the figures of a table of 1,000,000 rows and 20,000 members) */
#define SIFT_SCOPE_CAP_ROW_INSTR	21
/* a row of a write buffer, or of a changed member, whose member has a pair with the keys, copied for each pair, made a unit and read (a unit of one key of some thousand rows) */
#define SIFT_SCOPE_DELTA_INSTR		1700
/* pricing a read against the entity path (the members listed, the rows counted) costs about this: a patch and a delta that cost less are read without it */
#define SIFT_SCOPE_PRICING_INSTR	1000000
/* the changed members one read lists, at most (8 MB of keys) */
#define SIFT_SCOPE_CHANGED_MAX		(1 << 20)
/* a scope entry tested against the drop list of each key of a read of several keys counted once that has one (the members it leaves to a lesser key): 217 instructions an entry for 10 keys, 364 for 20, 512 for 30,
 * 808 for 50 over the 67 of an entry of one key (300,000 events, 5,000 members in several keys, 50 keys, the largest keys first; warm, instructions of the backend): about 15 for each key with a list */
#define SIFT_SCOPE_DEDUPE_INSTR		15

/*
 * The counts the price of a read of a scope key takes. The executor (scope_open) has them from the read it made: the entries and rows from the zones and set summaries of the units as the scan
 * prunes them, the entity rows of the members to patch (the changed members that have a pair with a key), the delta and the members the relation lists. The planner can know at plan time: nkeys and once (its estimate of nkeys, or the real count once the keys are
 * evaluated), the entries as nkeys times the entries a key holds (rows of the scope segments of the directory over their keys), pairs as the reltuples of the relation of pairs and path_rows
 * as the rows of the entity segments the members of the keys may hold; patch_units, patch_scan_rows and delta_rows are 0 until the journal and the write buffers are read (the upper bound of delta_rows is the rows
 * of the buffers in the directory times nkeys). A changed member costs its units and copies here and, in the reader, SIFT_SCOPE_MEMBER_INSTR a member against the cap of SIFT_SCOPE_CHANGED_MAX: the figures of the entries and the patch
 * were measured with it in.
 */
typedef struct SiftScopeCounts
{
	uint32		nkeys;
	bool		once;			/* a row counts once though its member has pairs with several keys */
	uint32		drops;			/* with once: the keys that have a drop list (SiftScopeRead.ndrop), 0 where the read has not been made: the least the units cost */
	uint64		entries;		/* the rows of the scope units of the keys */
	uint64		patch_units;	/* the units of the entity segments the rows of the changed members that have a pair now are read from */
	uint64		patch_scan_rows;	/* the entity rows looked at to find them (no point directory of the member leaf tells where they are) */
	uint64		delta_rows;		/* the copies of the rows of the buffers and of the members to patch the delta reads */
	uint64		pairs;			/* the pairs of the relation, which the entity path lists the members of the keys from */
	uint64		path_rows;		/* the entity rows that may hold a member the relation lists for the keys */
} SiftScopeCounts;

/* What the patch and the delta cost on top of the scope units: the units of the changed members' rows, the rows looked at to find them, and the copies made of them and of the buffers' rows (pure: instructions of the backend) */
static inline double
sift_scope_patch_price(const SiftScopeCounts *c)
{
	return SIFT_SCOPE_PATCH_UNIT_INSTR * (double) c->patch_units + SIFT_SCOPE_SCAN_INSTR * (double) c->patch_scan_rows + SIFT_SCOPE_DELTA_INSTR * (double) c->delta_rows;
}

/* What the scope units cost, with the patch and the delta (pure) */
static inline double
sift_scope_units_price(const SiftScopeCounts *c)
{
	double		entry = SIFT_SCOPE_ENTRY_INSTR + (c->once ? SIFT_SCOPE_DEDUPE_INSTR * (double) c->drops : 0);

	return SIFT_SCOPE_SETUP_INSTR + entry * c->entries + sift_scope_patch_price(c);
}

/* What the entity path (the join as written) costs: the pairs read, the entity rows the key filter tests and those it keeps (pure) */
static inline double
sift_scope_entity_price(const SiftScopeCounts *c)
{
	return SIFT_SCOPE_PAIR_INSTR * (double) c->pairs + SIFT_SCOPE_ENTITY_INSTR * (double) c->path_rows + SIFT_SCOPE_MATCH_INSTR * (double) c->entries;
}

/* Is the entity path the cheaper of the two (pure: the same counts give the same answer) */
static inline bool
sift_scope_entity_cheaper(const SiftScopeCounts *c)
{
	return sift_scope_units_price(c) > sift_scope_entity_price(c);
}

/* The rows of the patch (sift_scope_segment_rows): the delta takes them after the price of the read was taken (patch_rows and patch_scan are only expected until then); a read that is not to be made frees what the plan holds */
extern void sift_scope_read_rows(SiftScopeRead *rd);
extern SiftScopeRead *sift_scope_read(Relation heap, Relation index, Snapshot snap, const char *key_name, const int64 *keys, int nkeys, bool once, bool topn,
								   const int *leaves, int nleaves);
/* The member keys that have a pair with one of the keys under the snapshot (keys NULL: with any), ascending, each once */
/* The columns of the index the scope segments of key_name keep values of, of all its columns (EXPLAIN "Scope Columns") */
extern void sift_scope_columns(Relation index, const char *key_name, int *kept, int *all);
extern int64 *sift_scope_members(Relation heap, Relation index, const char *key_name, Snapshot snap, const int64 *keys, int nkeys, uint32 *n);
/* The relation of pairs of the scope (open, AccessShare) with its member and scope key columns, and the journal index of the relation: a sift_col index that holds both columns */
extern Oid	sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey);

#ifdef SIFT_TESTING
/* A fill of one scope read from a relation and its btree on (member, key), without a declaration (sift._test_scope_expand) */
extern struct SiftScopeFill *sift_scope_fill_begin_rel(MemoryContext mcxt, Relation rel, Oid btree, AttrNumber member, AttrNumber key, uint64 rows);
#endif
extern PGDLLIMPORT bool sift_agg_scope;
extern PGDLLIMPORT bool sift_test_scope_patch;	/* sift.test_scope_patch: a read patches the changed members whatever it costs */
extern PGDLLIMPORT double sift_scope_max_multiplicity;

/* Refuses a scope of more than sift.scope_max_multiplicity entries a row; estimated: the figures are those of a sample (scope_sample.c), before and added are the bytes of the index and of the scope segments, 0 before the build */
extern void sift_scope_refuse(const char *what, uint64 entries, uint64 rows, bool estimated, double before, double added);
/* Before the build of an index with a scope: refuses a table whose sample is clearly above the limit */
extern void sift_scope_precheck(Relation heap, Relation index);
#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_scope_sample_blocks;
#endif

#endif							/* SIFT_COL_SCOPE_H */
