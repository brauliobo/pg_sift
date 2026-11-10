# Scope index (EXPERIMENTAL)

A scope is a key a `sift_col` index cuts its rows by that the rows do not hold: they reach it through a table of `(member, scope key)` pairs. Events of entities, entities in tenants: the question
is "the events of tenant 7", and the tenant is in the pairs of the entity, not in the event. The scope index stores the rows replicated by scope key in segments of their own, cut by `(scope key,
time)`, so a read of one tenant reads the units of that tenant and no others, where the join reads the entity of each member. New in 4.4, column format minor 45, opt-in, and EXPERIMENTAL: the
declaration, the formats and the settings may change in a later release. `sift_table` has no scope.

## The declaration

    CREATE INDEX tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

`key_name: fk -> [schema.]relation(member, scope_key)`: the name of the derived key, the column of the index that holds the member key (`int2`, `int4`, `int8`), and the relation of pairs with its
two integer columns. Up to 4 scopes, separated by `;`, each with its own name. The relation is a heap table (not a view, not the indexed table), with a unique btree on exactly `(member,
scope_key)` in either order (a primary key or a unique constraint), both columns NOT NULL; an unqualified name is in the schema of the indexed table. The index depends on that btree: `DROP INDEX`
of it, or `DROP TABLE` of the relation, needs `CASCADE`, which drops the index. `ALTER INDEX .. SET (scope = 'off')` or `RESET (scope)` removes the declaration; `REINDEX` drops the scope segments.
The time cut of the scope units is the first `date`, `timestamp` or `timestamptz` column of the index, else none.

**The journal index.** The relation of pairs must have a `sift_col` index with `track_changes = 'ctid'` whose first column is the member and that holds the scope key. Without one `CREATE INDEX`
is refused with a hint. The first scope index declared on a relation starts its journal at the end of its build, so its segments carry no stamp and the reads say `unfilled` until a `REINDEX` of the
scope index (or the rewrite the maintenance makes) takes it; the indexes built after it are stamped by their build.

**`cols(..)`.** `scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(created_at, kind, value_num)'`: the scope segments of that scope keep only the listed columns of the index,
the member key, the column the units are cut by and the derived key; every other leaf has no chunk in them. A statement that reads a column outside the list reads the entity rows instead
(`Scope Fallback: column pad not in the scope units`). The subset is smaller: the scope segments of the generic test table are smaller with `cols(kind, value_num)` than with every column, and
smaller again with `cols(created_at)`; the entries are the same.

## What a read does

A scan of the index with a scope conjunct (the rows whose member has a pair with a key) reads the scope units of those keys, less the members that changed in the relation of pairs since the
stamp of the units (the journal says which), plus the entity rows of those of them that have a pair with a key now (the patch), and the rows of the write buffers' members the same way. A statement
that the scope cannot answer reads the entity rows of the members the relation lists for the keys; the answer is the join's in every case (the tests compare md5 against the plain join after every
state: edits of pairs, writes, deletes, VACUUM, promotion, merge, rewrite, a crash at each directory update). In this release the scan takes the scope conjunct from the marker
`sift.in_scope(index, key_name, fk, key or keys)` of the testing flavour (`sift.in_scope_pairs` for the meaning of a join); the planner's recognition of the join forms (`JOIN scope_rel USING
(member) WHERE scope_key = k`, `member IN (SELECT member FROM scope_rel WHERE scope_key = k)`, `EXISTS`, `GROUP BY` the key) is a separate piece of the 4.4 work, and until it lands a release
build has the declaration, the segments and the journal but no statement that reads them.

**Many to many.** A member may be in several keys: a fact has one entry for each. A read of one key counts each fact of a member that has the pair once. A read of several keys with the meaning of
`IN` or `EXISTS` counts a fact once whichever of the keys its member has (`Scope Semantics: once`): the units of all but the least key of a member leave the member's facts out, from lists the read
builds from the pairs, bounded by `work_mem` and by 64 keys that have a list (`dedupe over work_mem`, `dedupe keys`: the entity rows are read then). A join and `GROUP BY` the key count a fact once for each pair
(`Scope Semantics: per pair`). A read per pair of several keys that has changed members cannot be patched from the entity rows (one row each); it is not a fallback to the entity rows, it needs
the join, which the planner keeps (only the testing marker `sift.in_scope_pairs` reaches it, and raises an error that says so).

## EXPLAIN

`Scope Index`, `Scope Keys`, `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows`, `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled`, `several stamps`, `scope segments do not cover the rows`, `write buffer`, `top-n, changed members` (a top-N cannot take the patch),
`dedupe over work_mem`, `dedupe keys`, and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the next `REINDEX` of
the scope index fills it again), `trimmed`, `snapshot` (a serializable or non-MVCC snapshot), `snapshot older than the stamp`, `stamp aged`, `too many changed keys`. A scope read is decided once, by the
leader (the verdict, the changed members and the patch are made from the snapshot of the statement): it has no workers.

## Settings

| Setting | Default | Gain | Impact | Why the default | Off |
|---|---|---|---|---|---|
| `sift.agg_scope` | on | a read of one key from the units: 0.7 to 1.0 million instructions where the entity rows of the members cost 2.6 to 3.4 (100,000 events, 5,000 entities, 40 tenants, the largest tenant); two keys 1.9 million against 2.0 to 3.1 | nothing stored | a scope exists only when the user declares it | `SET sift.agg_scope = off`: the statement is the join as written; no `REINDEX` |
| `sift.scope_patch_keys` | 4096 | the changed members are read through the entity path for a few keys, not by a rewrite | each patched key is a probe of the entity path | a few hundred changed keys cost less than a rewrite of the units | `SET sift.scope_patch_keys = 1`: more than one changed key reads the entity rows; no `REINDEX` |
| `sift.scope_max_multiplicity` | 8 | `CREATE INDEX` refuses a scope whose members are in so many keys that the units would be a multiple of the table | none | the measured multiplicity of a tenant or a group is 1.0 to 1.1 | `SET sift.scope_max_multiplicity = 1000000` accepts any figure |

## What it costs

`CREATE INDEX` and `REINDEX` say it for each scope: `NOTICE: scope "tenant_id": 93,248 entries for 100,000 rows (0.93 per row); the index grows from 2.4 MB to 5.1 MB (+107%)` (the growth is said
from 1 MB of entities). The scope segments are written with the entity segments by the build, the parallel build, a promotion and a merge, in the same switch of the directory; a rewrite fills
them again when the journal says that many members changed (`sift.col_connect_refresh_pct`) or an entity segment lost its flag. The relation of pairs pays the journal index: a 32-byte record for each pair an UPDATE or DELETE ends and each pair inserted.

## Limits

EXPERIMENTAL; one constant key or a list of constants per conjunct, no scope of a partitioned table of pairs; text keys through a dictionary table; a top-N over changed members reads the entity
rows; several keys with a per-pair meaning need the join once a member changed; at most 4 scopes and 8 connections plus scopes on an index; a library before 4.4 refuses the index. The reserved
by-reference unit kind (bits 8 to 15 of the scope stamp, directory flag 0x0010) is never written by 4.4: a library that finds one refuses the segment ("Use a newer pg_sift"), and the entity
rows still answer.
