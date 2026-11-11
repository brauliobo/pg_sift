# Scope index (EXPERIMENTAL)

A scope is a key a `sift_col` index cuts its rows by that the rows do not hold: they reach it through a table of `(member, scope key)` pairs. Events of entities, entities in tenants: the question
is "the events of tenant 7", and the tenant is in the pairs of the entity, not in the event. The scope index stores the rows replicated by scope key in segments of their own, cut by `(scope key,
time)`, so a read of one tenant reads the units of that tenant and no others, where the join reads the entity of each member. New in 5.0, column format minor 45, opt-in, and EXPERIMENTAL: the
declaration, the formats and the settings may change in a later release. `sift_table` has no scope.

## The declaration

    CREATE INDEX tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

`key_name: fk -> [schema.]relation(member, scope_key)`: the name of the derived key, the column of the index that holds the member key (`int2`, `int4`, `int8`), and the relation of pairs with its
two integer columns. Up to 4 scopes, separated by `;`, each with its own name. The relation is a heap table (not a view, not the indexed table), with a unique btree on exactly `(member,
scope_key)` in either order (a primary key or a unique constraint), both columns NOT NULL; an unqualified name is in the schema of the indexed table. The index depends on that btree: `DROP INDEX`
of it, or `DROP TABLE` of the relation, needs `CASCADE`, which drops the index. The dependency is also the record of which btree defines the pairs: `CREATE INDEX` and `REINDEX`
choose it (the lowest oid of those that qualify) and say which in a NOTICE, every read uses that one, and a btree made or rebuilt (`REINDEX .. CONCURRENTLY`) afterwards changes nothing until the next
`REINDEX` of the scope index chooses again; `ALTER INDEX` keeps it. `ALTER INDEX .. SET (scope = 'off')` or `RESET (scope)` removes the declaration; `REINDEX` drops the scope segments.
The time cut of the scope units is the first `date`, `timestamp` or `timestamptz` column of the index, else none.

**The journal index.** The relation of pairs must have a `sift_col` index with `track_changes = 'ctid'` whose first column is the member, that holds the scope key and that has no `WHERE` (a partial index never hears of a pair inserted
outside its predicate, so it does not qualify). Without one `CREATE INDEX` is refused with a hint. The first scope index declared on a relation starts its journal at the end of its build, so its segments carry no stamp and the reads say `unfilled` until a `REINDEX` of the
scope index (or the rewrite the maintenance makes) takes it; the indexes built after it are stamped by their build.

**`cols(..)`.** `scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(created_at, kind, value_num)'`: the scope segments of that scope keep only the listed columns of the index,
the member key, the column the units are cut by and the derived key; every other leaf has no chunk in them. A statement that reads a column outside the list reads the entity rows instead
(`Scope Fallback: column pad not in the scope units`). The subset is smaller: the scope segments of the generic test table are smaller with `cols(kind, value_num)` than with every column, and
smaller again with `cols(created_at)`; the entries are the same.

## What a read does

A scan of the index with a scope conjunct (the rows whose member has a pair with a key) reads the scope units of those keys, less the members that changed in the relation of pairs since the
stamp of the units (the journal says which), plus the entity rows of those of them that have a pair with a key now (the patch), and the rows of the write buffers' members the same way. A statement
that the scope cannot answer reads the entity rows of the members the relation lists for the keys; the answer is the join's in every case (the tests compare md5 against the plain join after every
state: edits of pairs, writes, deletes, VACUUM, promotion, merge, rewrite, a crash at each directory update). The scan takes the scope conjunct from the join the way an application writes it
(the recognition below), or from the marker `sift.in_scope(index, key_name, fk, key or keys)` of the testing flavour (`sift.in_scope_pairs` for the meaning of a join).

## What the planner recognises

A statement that joins the scanned table to the scope relation of one of its scope indexes is planned twice: as written, and with that join taken out of the `FROM` list and its two
conditions, the equality of the member column with the declared column of the table and the binding of the scope key, made the scope conjunct of the scan (`EXPLAIN`: `Scope Index`). The
estimated costs of the two plans choose: the scope read leaves the share of the rows that the pairs of the keys own (`pg_stats` of the key column of the relation, the most common keys by their
own frequency) of the distinct members of the table, where the join's key filter on the member column leaves the units that hold a member of the keys, most of them for facts ordered by time. The
rows are those of the join in every case; whether the index may be read at execution is the reader's verdict (`Scope Fallback`).

| form | counts a row | recognised |
|---|---|---|
| `JOIN scope_rel s ON s.member = f.fk WHERE s.scope_key = k` (also `IN (k1, k2)` with one distinct key) | once for each pair | yes |
| `f.fk IN (SELECT member FROM scope_rel WHERE scope_key = k or IN (list))`, `EXISTS`, a CTE of the members, a set-returning SQL function of them | once | yes, any number of constant keys |
| the same through a view, a security barrier view that projects the two columns, an inlined `STABLE` SQL function | as written | yes |
| `JOIN` with several keys | once for each pair | no: a member whose pairs changed needs the join, which counts them |
| `GROUP BY` the scope key or any other column of the relation read | | no: the units give the rows, not the pair |
| a table or sub-select of keys, a parameter of a generic plan (the custom plan has the constant) | | no: the keys are constants |
| another condition on the relation, a view with a condition, `DISTINCT` or a join inside the sub-select, `NOT EXISTS`, an outer join, row-level security on the relation | | no |

A statement that is nearly one keeps the join and says why in `sift.explain_why` (arm `scope`, slug `scope`). `SET sift.agg_scope = off` turns the recognition off; a top-N, a `GROUP BY` with an
`ORDER BY .. LIMIT` and the aggregates are the scan's as ever.

**Many to many.** A member may be in several keys: a fact has one entry for each. A read of one key counts each fact of a member that has the pair once. A read of several keys with the meaning of
`IN` or `EXISTS` counts a fact once whichever of the keys its member has (`Scope Semantics: once`): the units of all but the least key of a member leave the member's facts out, from lists the read
builds from the pairs, bounded by `work_mem` and by 64 keys that have a list (`dedupe over work_mem`, `dedupe keys`: the entity rows are read then). A join and `GROUP BY` the key count a fact once for each pair
(`Scope Semantics: per pair`). A read per pair of several keys that has changed members cannot be patched from the entity rows (one row each); it is not a fallback to the entity rows, it needs
the join, which the planner keeps (only the testing marker `sift.in_scope_pairs` reaches it, and raises an error that says so).

## EXPLAIN

`Scope Index`, `Scope Keys`, `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows`, `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled`, `several stamps`, `scope segments do not cover the rows`, `write buffer`, `top-n, changed pairs` (a top-N cannot take the patch),
`dedupe over work_mem`, `dedupe keys`, and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the next `REINDEX` of
the scope index fills it again), `trimmed`, `snapshot` (a serializable or non-MVCC snapshot), `snapshot older than the stamp`, `stamp aged`, `too many changed keys`. A scope read is decided once, by the
leader (the verdict, the changed members and the patch are made from the snapshot of the statement): it has no workers.

**Where the conjunct may be.** The scan takes the conjunct when it filters the rows the scan reads: the `WHERE` of the statement (when no outer join of the scan can null the scanned table), the conditions
inside the nullable input of an outer join (the `ON` conjuncts that read only that input, the `WHERE` of a sub-select or of an inlined CTE: pushing them below the join is exact), a lateral child, a
materialized CTE (its own scan). The dashboard shape, a small table of periods left-joined to the scoped events, reads the scope units of the key. A conjunct that does not filter the scanned rows
stays an ordinary filter, which the scan tests on every row, and `EXPLAIN` says why in `Scope Fallback`: the `ON` clause of a join that keeps the scanned rows (a `LEFT JOIN` from the events, a `FULL
JOIN`: the rows that fail it are output rows), a conjunct over the rows an outer join made of the unmatched ones (`WHERE conj OR e.id IS NULL` on the nullable side: the rows the join made pass
it), an expression around it, a key that is not a constant, a second scope (a scan reads one), the setting off. A `WHERE conj` over the nullable side of a `LEFT JOIN` drops the unmatched rows, which
the scan cannot do: the statement keeps the normal plan (`sift.explain_refusals`: `outer_join`).

## Settings

| Setting | Default | Gain | Impact | Why the default | Off |
|---|---|---|---|---|---|
| `sift.agg_scope` | on | a read of one key from the units: 0.7 to 1.0 million instructions where the entity rows of the members cost 2.6 to 3.4 (100,000 events, 5,000 entities, 40 tenants, the largest tenant); two keys 1.9 million against 2.0 to 3.1 | nothing stored | a scope exists only when the user declares it | `SET sift.agg_scope = off`: the statement is the join as written; no `REINDEX` |
| `sift.scope_patch_keys` | 4096 | the changed members are read through the entity path for a few keys, not by a rewrite | each patched key is a probe of the entity path | a few hundred changed keys cost less than a rewrite of the units | `SET sift.scope_patch_keys = 1`: more than one changed key reads the entity rows; no `REINDEX` |
| `sift.scope_max_multiplicity` | 3 | `CREATE INDEX` refuses a scope whose members are in more keys than this on average, with the size and growth it would have had; the join answers the reads (see Break-even) | none | the units break even with the join at 2.5 (500,000 events) to 3.0 (2,000,000) entries a row | `SET sift.scope_max_multiplicity = 1000000` accepts any figure |

## Break-even: the multiplicity limit

A scope entry is a copy of the row in the index, so the scope segments grow with the average number of keys of a member (the entries a row has). The weighted formula of the program prices it for the classes
of a tenant read (sum by kind, one month, distinct by month, top 20, filter, latest 20; instructions of the backend, 100,000 to 2,000,000 events, 3 to 6 keys a member; measured by the lab of the by-address
study, 500,000 events: heap 42.2 MB, primary key 11.3 MB, entity index 12.4 MB; 2,000,000 events: 267 MB in all):

| | 100,000 events | 500,000 events | 2,000,000 events |
|---|---|---|---|
| benefit B (mean over the 6 classes, R = 1 each; S = join / units instructions) | 22.4 (S 2.1 to 4.1) | 26.7 at 2.7 entries a row, 24.4 at 5.4 (S 1.5 to 10.5) | 31.8 at 2.7 (S 1.8 to 22) |
| growth G of the table and its indexes | | +47% at 2.7, +92% at 5.4 | +51% at 2.7, +96% at 5.4 |
| growth of the index alone | | +254% at 2.7, +489% at 5.4 | +255% at 2.7, +482% at 5.4 |
| write W (10,000 inserts and their promotion, instructions) | | +65% at 2.7, +107% at 5.4 | |

G is 17 to 19% of the footprint and W 23% plus 16% for each entry a row has: the cost is C = G/3 + W/5 = 9.0 m + 4.6 (the complexity of the code is already paid). The benefit does not depend on the entries
a row has (the reads follow the rows of a key) and grows with the table (the join is linear in it): B = C at m = 2.0 (100,000 events), 2.5 (500,000) and 3.0 (2,000,000), and above those the index loses
(500,000 events at 2.7: B 26.7 against C 28.7; at 5.4: 24.4 against 52). So CREATE INDEX and REINDEX refuse above 3 entries a row, the break-even of a table where the join starts to cost,
and the refusal says the entries, the rows, and the size and growth the index would have had; `sift.scope_max_multiplicity` is the override for the members that are in many keys and whose reads are worth it.
A table of 2,048 blocks (16 MB) or more is estimated before its build, so a scope that is clearly over the limit does not spend the build on its segments: 32 runs of 8 consecutive blocks at fixed random
places give the entries of their rows as a ratio of their rows, and the build is refused ("about N entries for about N rows (N per row, from a sample)", without the growth) only when the ratio less 3.5 standard
errors of that ratio of 32 runs (about 99.9% one-sided) is over the limit. Every other scope is built and the exact figure after the build decides, so a scope near the limit is never refused by a sample.
Above the limit the join answers (the entity path), and a count or a sum by key is cheaper from a table of them (docs/INDEX-RECIPES.md). A scope copy flips at 1.84 copies a fact because it carries
the heap rows and their trigger as well; the units live in the index and flip at 3. The reads of the units are not cheaper at a higher multiplicity, and a table of 100,000,000 events moves the break-even
up (the join is linear in the table), which this limit does not price: raise it for such a table, from the figures of the NOTICE.

## What it costs

`CREATE INDEX` and `REINDEX` say it for each scope: `NOTICE: scope "tenant_id": 93,248 entries for 100,000 rows (0.93 per row); the index grows from 2.4 MB to 4.4 MB (+81%)` (the growth is said
from 1 MB of entities). The scope segments are written with the entity segments by the build, the parallel build, a promotion and a merge, in the same switch of the directory; a rewrite fills
them again when the journal says that many members changed (`sift.col_connect_refresh_pct`) or an entity segment lost its flag. The relation of pairs pays the journal index: a 32-byte record for each pair an UPDATE or DELETE ends and each pair inserted.

The test table of 100,000 events, 5,000 entities and 40 tenants (one scope, a 5-column index): the index is 2,523,136 bytes without the scope and 4,489,216 with it (4,358,144 with `cols(created_at, kind,
value_num)`), the build takes 250 ms without and 406 ms with it, and writes 2.9 MB of WAL without and 5.2 MB with it. Keys of fewer than 2,048 entries share the units of the scope segments, and the
scope segments have no point directory, which the reads of a key do not use.

**Building at the same time.** The build of a `sift_col` index that tracks changes installs the two triggers of its log on the table when it has read it, which takes `ShareRowExclusiveLock`
while `CREATE INDEX` holds `ShareLock`: two such builds on one table that ran together (`pg_restore -j`) each waited for the other. An event trigger before `CREATE INDEX` of a `sift_col` index
(`sift_col_build_lock`) takes the stronger lock first, so the second build waits at its start; the options a build decides are written under the tuple lock of the catalog row of its index. `REINDEX`
of two indexes of one table at the same time is not covered by the event trigger (PostgreSQL has none for it).

## Scope index or scope copy

Two ways to make a request by a key above the fact table (a tenant, a group) cost what the key holds. Both are exact for the rows and neither needs a change to the query that the planner does not already
make; they differ in what they keep.

| | scope index (this page) | scope copy ([INDEX-RECIPES](INDEX-RECIPES.md#keys-above-the-fact-table)) |
|---|---|---|
| what is stored | the rows replicated by scope key inside the segments of the index of the table | a second table of the facts, once for each scope key of the member, with a `sift_col` index keyed by the scope key |
| size (100,000 events, 5,000 entities, tenants and groups: all tables and indexes of the fixture, 13.7 MB with the plain index) | 17.9 MB with one index of two scopes (+4.2 MB) | 35.0 MB with the two copies (+21.3 MB) |
| WAL of the builds (same fixture; 2.6 MB for the plain index) | 6.7 MB | 24.6 MB |
| writes to the facts | none beyond the index's (the write buffer of the index; the pairs are read through their journal) | a statement trigger applies the change to the copy in the statement |
| writes to the pairs | the journal index of the pairs (a 32-byte record a pair) and a read that patches the changed members, a rewrite when many changed | a statement trigger refreshes the members' rows |
| a pair table that is a view | no: the pairs are a table with a unique btree | yes, `sift.scope_follow_pairs` keeps it as the view's answer |
| the planner uses it | the join forms of the statement, as the application writes them (the recognition above) | the copy is a table: `WHERE tenant_id = k` on it |
| state | EXPERIMENTAL | released API (`sift.scope_create`, `sift.scope_verify`, `sift.scope_drop`) |

Use the copy for a request that names the key and wants the rows of the key by a plain query on a table. Use the index when the facts must stay one table (no second copy to size, to vacuum and to
keep by triggers on the write path). The copy stores a fact once for each key of its member, so it grows with that figure (refused above 3
copies a fact); the index's growth is said by the `NOTICE` and bounded by `sift.scope_max_multiplicity`.

## Limits

EXPERIMENTAL; one constant key or a list of constants per conjunct, no scope of a partitioned table of pairs; text keys through a dictionary table; a top-N over changed members reads the entity
rows; several keys with a per-pair meaning (a join, not `IN` or `EXISTS`) keep the join; a key that is a parameter of a generic plan, a table of keys and a `GROUP BY` the key are not recognised; at most 4 scopes and 8 connections plus scopes on an index; a library before 5.0 refuses the index. The reserved
by-reference unit kind (bits 8 to 15 of the scope stamp, directory flag 0x0010) is never written by 5.0: a library that finds one refuses the segment ("Use a newer pg_sift"), and the entity
rows still answer.
