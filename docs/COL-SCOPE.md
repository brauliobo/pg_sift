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
smaller again with `cols(created_at)`; the entries are the same. A leaf with no chunk is NULL in every row of the unit and its zone says so: VACUUM, a promotion,
a rewrite, a merge and `sift.col_verify` read the units as they are, whether or not the list names the primary key (test `col_scope_cols_maint`).

## Pairs that are soft deleted: a partial unique btree

The pairs may be the rows of a PARTIAL unique btree of the relation: the predicate says which rows are pairs, and the rows that satisfy it are unique on the two columns. A bridge table that soft
deletes keeps its all-rows unique btree (the primary key) and adds the one that defines the pairs:

    CREATE UNIQUE INDEX memberships_active ON memberships (dim_a_id, group_id) WHERE deleted_at IS NULL;
    CREATE INDEX memberships_journal ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'group_id: dim_a_id -> memberships(dim_a_id, group_id)');

No table exists for the index alone, and the application deletes as it does. An `UPDATE` that sets `deleted_at` ends a pair and one that clears it creates one: the journal records the member of both,
so a read takes the stored entries of the members that did not change and reads the entity rows of those that did. A statement that updates a column no index holds is a HOT update and writes
nothing to the journal. The scope entries are the active pairs only (soft deleted rows have none), and the answer is the join on the active rows in every state.

- **Which btree.** `using <index>` after the columns names it (`memberships(dim_a_id, group_id) using memberships_active`); without it the only partial unique btree on exactly the two columns, else the lowest oid of
  the plain ones; two partial ones are an error that asks for `using`. The `NOTICE` of `CREATE INDEX` and `REINDEX` names it and its predicate; the index records it (the dependency), every read uses it, and one made
  later changes nothing until `REINDEX` chooses again. `ALTER INDEX` with another `using` than the recorded btree is refused: the segments were written under the recorded one.
- **Journal index.** A plain one, or a partial one whose predicate the predicate of the pairs' btree implies (`WHERE deleted_at IS NULL` beside the same predicate: a soft delete writes one record). A partial one
  that a pair can be inserted outside of does not qualify: it never hears of that pair.
- **What the predicate can be.** What PostgreSQL accepts in an index predicate: immutable, over the columns of the row (`IS NULL`, comparisons with constants, boolean columns). `now()` is refused by PostgreSQL
  (`functions in index predicate must be marked IMMUTABLE`), so a pair that ends by the clock needs a column the application or a scheduled `UPDATE` sets (`UPDATE grants SET expired = true WHERE expired_at <= now() AND NOT expired`,
  a journaled write: exact as of the sweep, so the staleness is the sweep interval), or the copy path ([INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view), which has `sift.scope_expire_pairs`).

## What a read does

A scan of the index with a scope conjunct (the rows whose member has a pair with a key) reads the scope units of those keys, less the members that changed in the relation of pairs since the
stamp of the units (the journal says which), plus the entity rows of those of them that have a pair with a key now (the patch), plus the delta: the rows of the write buffers, which no scope entry describes. The delta reads the buffer rows once,
keeps those whose member did not change, and copies each once for each pair of its member with a key under the statement's snapshot (the pairs come from the pair btree, the leaves of the journal index of the pairs or a scan of the
relation, whichever is cheapest), as the promotion would write them; the scan reads the copies as one more unit of each key, so the visibility check and the tombstones are those of the
write buffer's rows. A buffered row of a member whose pairs changed is read with the rest of that member's rows by the patch. A statement
that the scope cannot answer reads the entity rows of the members the relation lists for the keys (a read of every key by the key has no such fallback: below); the answer is the join's in every case (the tests compare md5 against the plain join after every
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
| `GROUP BY` the scope key ("every tenant"), or any other column of the relation read | once for each pair | not by the planner yet: the reader gives it (below; the testing markers reach it) |
| the scope key bound by a parameter: `= $1`, `= ANY ($1)`, `IN ($1, $2)`, a stable expression (`current_setting(..)::int`), the output of an InitPlan (`= (SELECT ..)`, `= ANY (ARRAY(SELECT ..))`) | as written | yes: the keys are those of the execution (below); a join (a row for each pair) only for a scalar, an array may hold several keys |
| a table of keys joined to the scope relation (`JOIN keys k ON k.key = s.key`, `s.key IN (SELECT key FROM keys ..)`) | | no: the keys of the table are not known when the plan is made (`= ANY (ARRAY(SELECT key FROM keys ..))` is) |
| another condition on the relation, a view with a condition, `DISTINCT` or a join inside the sub-select, `NOT EXISTS`, an outer join, row-level security on the relation | | no |

**Parameters.** A key that is a parameter of a prepared statement (or the output of an InitPlan, or a stable expression) is read when the scan starts, so a generic plan has the scope read for every value: the plan
is made once, and each execution has the keys, the verdict of the reader and the changed pairs of its own snapshot (a cached plan stays exact when the pairs change between two executions; NULL and the NULLs of an
array match nothing, an empty set reads no unit). `EXPLAIN` shows `Scope Keys: $1`, and a run `Scope Keys: $1 (2 keys)`; more keys than the dedupe lists or than `work_mem` read the entity rows
(`Scope Fallback: dedupe keys`, `dedupe over work_mem`) as for constants. A custom plan has the constants and is priced for them. A generic plan has no value to price: a scalar is the pairs of the average key
(the rows `pg_stats` expect for an equality on the key column), an array that is not a list the 10 keys PostgreSQL assumes for an array it does not know, and the choice between the scope read and the join is made
once for all values, so a tenant far above the average pays the plan the average chose (the scope read grows with the tenant, the join with the table).

A statement that is nearly one keeps the join and says why in `sift.explain_why` (arm `scope`, slug `scope`). `SET sift.agg_scope = off` turns the recognition off; a top-N, a `GROUP BY` with an
`ORDER BY .. LIMIT` and the aggregates are the scan's as ever.

**Many to many.** A member may be in several keys: a fact has one entry for each. A read of one key counts each fact of a member that has the pair once. A read of several keys with the meaning of
`IN` or `EXISTS` counts a fact once whichever of the keys its member has (`Scope Semantics: once`): the units of all but the least key of a member leave the member's facts out, from lists the read
builds from the pairs, bounded by `work_mem` and by 64 keys that have a list (`dedupe over work_mem`, `dedupe keys`: the entity rows are read then). A join and `GROUP BY` the key count a fact once for each pair
(`Scope Semantics: per pair`). A read per pair of several keys that has changed members cannot be patched from the entity rows (one row each), and neither can a read of every key (the row has no key): it is not a fallback to the entity rows, it needs
the join, which the planner keeps (only the testing markers `sift.in_scope_pairs` and `sift.in_scope_all` reach it, and raise an error that says so).

**The patch is priced.** A member that has a pair now (a restore, a new pair) has its facts read through the entity rows, and an index whose units are cut by time holds some of them in every unit: the
patch then reads most of the table beside the scope units. On 1,000,000 events, 20,000 members and 500 keys (a key of 995 members; instructions of the backend) one restored member reads 53,752 entity
rows and costs 7.4 million, 10 members 645,046 rows and 33.2 million, 30 or more the whole table and 49.3 to 61.3 million for 30 to 1,000, where the join costs 20.6 to 24.3. So each read counts what its
two routes would read, from the zones and set summaries of the units as the scan prunes them, and takes the cheaper: the patch (the scope units of the keys at 20 instructions an entry, the entity rows
of the members to patch at 47 a row) or the entity path (the members the relation lists for the keys, read from the leaves of its journal index, and the entity rows that may hold one of them at 21 a row,
the price of the join), which says `Scope Fallback: patch dearer than the entity path`. An index whose units are cut by the member key reads only the units of the members it patches and stays a patch (a
hundred restored members read 9% of the test table); an index cut by time keeps the patch for a member or two and takes the entity path from about ten (10 to 1,000 members: 19.7 to 32.0 million, against 20.6
to 24.3 for the join; 100 members 20.7 against 20.8). Pricing costs 1.9 million on a patch of one member (9.3 million against 7.4). The changed members one read lists are bounded by the same price (the entity
rows of the index at 21 over 5,600 for a changed member's probe): more read the entity path (`too many changed keys`). The setting `sift.scope_patch_keys` is gone; the testing setting `sift.test_scope_patch`
takes the patch whatever it costs.

**One price.** The two routes are priced by `sift_scope_entity_cheaper` (`src/col/scope.h`, with `sift_scope_units_price` and `sift_scope_entity_price`): pure functions of the counts of a `SiftScopeCounts` (keys, `once`, the entries of
the units, the entity rows of the patch, the delta rows, the pairs and the entity rows of the listed members), so the executor (from the zones of the units it read) and the planner (from its estimates)
decide from the same inputs, and the same snapshot and keys give the same route. A read of several keys counted once adds 7 instructions an entry for the drop lists.

**The delta is priced the same way.** The rows of the write buffers cost 900 instructions a row the pairs select (copy, encode and sort), against the 21 of an entity row of the entity path, and a
read whose delta costs more than 4,000,000 instructions is priced against the entity path, as the patch is. Measured on 300,000 events, 5,000 members and 50 tenants (instructions of the backend, warm, median of 5, a read of one tenant): the clean read costs 1.49 million and the join 3.32; with 2,000 buffered
rows the read costs 1.95 million (110 blocks hit, none read; join 3.48); with 20,000 buffered rows 2.63 million (join 3.96). Before, any buffered row made the read take the entity rows of its members
besides the units, 32.5 million with 2,000 buffered rows and 93.7 with 20,000 when forced, and the entity path (8.8 and 9.5 million, dearer than the join) when priced. A tenant that holds a large share of the
members with a buffer of tens of thousands of rows can still cost more than the join: the copies are encoded for each read. 20 changed pairs still cost 5.3 million (the entity rows of the changed members, against
the join's 3.7): the rows of a changed member are read through the entity path.

**Every key, grouped by the key.** A read of every key (`Scope Keys: all`, no key list, `Scope Semantics: per pair`) reads all the scope units, a fact once for each pair of its member, and its group key is the derived leaf
`<key_name>#scope`: every entry has its key (`Scope Group Key: <key_name>`), so the scan groups by it as the engine groups by any leaf. The units of a key hold that one key (the units of small keys share a unit, a run of keys), so a
unit with no member to leave out answers from its pre-aggregates, histograms and sums of its entries, with no hash and no row read (`Units From Pre-aggregates`); the gate that kept a read of several keys from them was written for the dedupe of `IN` and `EXISTS`, which a read per pair does not
have. The rest is as for one key: the stored entries of the members that changed leave (`fk NOT IN`), and the rows of the write buffers are the delta, a copy for each pair of their member. What this read cannot do is the patch:
the entity rows of a changed member that has a pair now carry no key, and the group key is never NULL, so the reader refuses (`per pair, changed pairs`, and the other reasons of the verdict) and the statement needs the join, which is the statement as written; the planner chooses between the two
at the start of the scan from the price of one function (`sift_scope_entity_cheaper`, with the keys the relation lists). Until the planner recognises the join, the testing markers `sift.in_scope_all(index, key_name, fk)` in the `WHERE` clause and
`sift.scope_key(index, key_name, fk)` as a group key or an aggregate stand for it, and a refused read raises the error the planner will replace by the join (test `col_scope_groupkey`). Measured on 300,000 events, 5,000 members and 50 tenants
(instructions of the backend, warm, median of 5; `sum(amount), count(*)` for every tenant): the read costs 1.94 million instructions (86 units from pre-aggregates, 408 blocks hit, 213 read cold; the join 52.9 million, 206 hit, 176 read), with 2,000 buffered rows 12.6 million (2,190 copies, about 4,800 instructions
each, of which sorting the pairs and the copies is 40%; the join 53.7), and with 20 changed pairs the reader says why after 0.94 million instructions, which the join then adds to its 56.7.

## EXPLAIN

`Scope Index`, `Scope Keys` (`all` for every key), `Scope Group Key` (a group key of the scan is the derived key), `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows`, `Scope Delta Rows` (the copies of write buffer rows the scan reads, not counted in the patched rows), `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled`, `several stamps`, `scope segments do not cover the rows`, `write buffer` (the pairs of the buffered rows are more than work_mem holds), `top-n, changed pairs` (a top-N cannot take the patch or the delta of a changed pair),
`dedupe over work_mem`, `dedupe keys`, `patch dearer than the entity path`, and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the next `REINDEX` of
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

## Replicate or reuse

A read of the facts of one key (a tenant, a group) can be served two ways. **Replicate**: the scope index of this page, whose units hold the rows of each key. **Reuse**: no scope index; the plain `sift_col` index of the table is read by the
member keys of the key, `fact.member IN (SELECT member FROM pairs WHERE key = k)`, which is exact at the time of the statement, keeps no journal, grows nothing and has no limit on the keys of a member. Both give the same rows. The
planner reads the scope units whenever the scope index exists and the statement has a recognised form (`sift.agg_scope`); where there is none (not declared, or refused above the limit) the join is read by the member keys. The
table is the measured cost of each, and the rule it leads to.

Measured on a table of `events` (member, time, kind, value, quantity) joined to a table of (member, tenant) pairs, 2.7 entries a row, four tenants of 0.1%, 3.3%, 6.6% and 33% of the events, the six requests of a dashboard
(sum by kind, sum by kind in one month, distinct members by month, top 20 members, a filter on the value, latest 20). Warm: instructions of the backend (geometric mean of the six) and shared buffers hit; cold: blocks read with
every relation of the statement evicted. Every pair of cells returned the same rows.

| events | tenant | replicate: M instr, hit, cold blocks | reuse: M instr, hit, cold blocks | reuse / replicate (instr) |
|---:|---:|---|---|---:|
| 100,000 | 0.1% | 0.90, 56, 8 | 2.32, 52, 49 | 2.6x |
| 100,000 | 33% | 1.93, 108, 42 | 5.09, 92, 86 | 2.6x |
| 500,000 | 0.1% | 1.09, 70, 16 | 3.68, 151, 132 | 3.4x |
| 500,000 | 3.3% | 1.71, 94, 39 | 5.78, 198, 167 | 3.4x |
| 500,000 | 33% | 5.03, 294, 167 | 14.07, 235, 211 | 2.8x |
| 2,000,000 | 0.1% | 1.77, 152, 82 | 9.62, 502, 452 | 5.4x |
| 2,000,000 | 3.3% | 3.88, 247, 142 | 16.68, 648, 584 | 4.3x |
| 2,000,000 | 6.6% | 5.19, 311, 181 | 20.86, 656, 597 | 4.0x |
| 2,000,000 | 33% | 13.12, 728, 428 | 51.20, 816, 760 | 3.9x |

Reuse over replicate for each request at 2,000,000 events (instructions; tenants of 0.1%, 3.3%, 6.6%, 33%):

| request | 0.1% | 3.3% | 6.6% | 33% |
|---|---:|---:|---:|---:|
| sum by kind, all time | 12.7x | 17.8x | 16.5x | 13.3x |
| sum by kind, one month | 2.9x | 3.6x | 4.4x | 10.2x |
| filter on the value | 12.9x | 8.5x | 5.7x | 2.5x |
| distinct members by month | 4.6x | 3.1x | 2.4x | 1.4x |
| top 20 members | 6.6x | 2.8x | 2.1x | 1.0x |
| latest 20 | 1.7x | 1.4x | 2.0x | 6.9x |

Reuse costs about 6 instructions a row of every unit that holds a member of the key, because the member column is decoded to find them, and its member list about 1.9 thousand instructions a member; replicate reads the rows of the key
and answers sums and counts by kind and month from stored combinations. So reuse grows with the table (about 25 instructions an event for the 33% tenant, about 5 for the 0.1% one), replicate with the tenant. At 10^8 events the
slopes of the three sizes give about 2.5 G instructions for reuse and 0.65 G for replicate for the 33% tenant, about 0.5 G and 0.09 G for the 0.1% tenant (extrapolated, not measured).

What a replicated scope index costs (same table, `sift.scope_max_multiplicity` raised for the 5.4 rows; entries a row = multiplicity):

| events | entries a row | entity index | with the scope | build | WAL of the build |
|---:|---:|---:|---:|---:|---:|
| 500,000 | 1.05 | 13.0 MB, 1.4 s | 25.4 MB (+95%) | 2.4 s | 50 MB (13 MB without) |
| 500,000 | 2.7 | 13.0 MB | 44.2 MB (+240%) | 5.0 s | 89 MB |
| 500,000 | 5.4 | 13.0 MB | 78.0 MB (+500%) | 5.7 s | 157 MB |
| 2,000,000 | 1.05 | 55.3 MB, 10 s | 106.5 MB (+93%) | 11.2 s | 215 MB (55 MB without) |
| 2,000,000 | 2.7 | 55.3 MB | 192.3 MB (+248%) | 21.4 s | 557 MB |
| 2,000,000 | 5.4 | 55.3 MB | 335.2 MB (+506%) | 64.8 s | 676 MB |

Writes to the facts cost the same with and without the scope (10,000 inserted, updated or deleted facts, 500,000 events: 537, 1,021 and 493 M instructions either way); the promotion that applies the journal costs 470 M
instructions and 1.1 MB of WAL against 74 M and 0.3 MB (+396 M, +0.8 MB for each 10,000 facts); 100 pairs added to a key cost 0.5 M more. Reuse costs nothing to write. A table of the totals by member, kind and month kept by triggers
is not an alternative: its cells are 85% of the facts at 25 facts a member over 10 kinds and 12 months, and its writes cost +125% (insert), +73% (update) and +140% (delete) instructions.

**The rule.** Declare a scope (`WITH (scope = ..)`) when the requests by a key are sums and counts by kind or time, the keys are up to about a tenth of the facts, and the members are in few keys (up to 3 entries a row, the
limit; the growth above is what the `NOTICE` says). Leave it undeclared when the requests are top-N, distinct or latest rows over keys of a third of the facts or more, when the members are in many keys, when the table is small (reuse
is 2 to 5 M instructions at 100,000 events), or when the extra size, build and 0.8 MB of WAL for each 10,000 facts of promotion are not worth the saving. Above the limit `CREATE INDEX` refuses the
scope and says the size it would have had; the join answers, at the cost of the table above and nothing more.

With both a scope index and an entity index on a table, the planner chose the scope read in all 48 cells measured (2,000,000 events at 2.7 entries a row and 500,000 at 5.4, four tenants, six requests), and it was never
more than 1% dearer than the join read measured on its own (the closest cells: top 20 of the 33% key, 82.8 against 85.3 M instructions at 2,000,000 events, and 22.3 against 22.1 M at 500,000 events and 5.4 entries a row);
`sift.agg_scope = off` gives the join read for a comparison.

## What it costs

`CREATE INDEX` and `REINDEX` say it for each scope: `NOTICE: scope "tenant_id": 93,248 entries for 100,000 rows (0.93 per row); the index grows from 2.4 MB to 4.4 MB (+81%)` (the growth is said
from 1 MB of entities). The scope segments are written with the entity segments by the build, the parallel build, a promotion and a merge, in the same switch of the directory; a rewrite fills
them again when the journal says that many members changed (`sift.col_connect_refresh_pct`) or an entity segment lost its flag. The relation of pairs pays the journal index: a 32-byte record for each pair an UPDATE or DELETE ends and each pair inserted.

The test table of 100,000 events, 5,000 entities and 40 tenants (one scope, a 5-column index): the index is 2,523,136 bytes without the scope and 4,489,216 with it (4,358,144 with `cols(created_at, kind,
value_num)`), the build takes 250 ms without and 406 ms with it, and writes 2.9 MB of WAL without and 5.2 MB with it. Keys of fewer than 2,048 entries share the units of the scope segments, and the
scope segments have no point directory and no global order (`order_array_columns` writes its arrays in the entity segments only: an array lists each row of a segment once, a scope segment holds a
fact once for each of its keys, and the reads of a key go by unit), which the reads of a key do not use.

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

EXPERIMENTAL; one key or a list of keys (constants or parameters) per conjunct, no scope of a partitioned table of pairs; text keys through a dictionary table; a top-N over changed members reads the entity
rows; several keys with a per-pair meaning (a join, not `IN` or `EXISTS`) keep the join, whatever an array parameter holds, and so does every key by the key when a changed member has a pair now; a join to a table of keys and the planner's recognition of a `GROUP BY` the key are not in this release; at most 4 scopes and 8 connections plus scopes on an index; a library before 5.0 refuses the index. The reserved
by-reference unit kind (bits 8 to 15 of the scope stamp, directory flag 0x0010) is never written by 5.0: a library that finds one refuses the segment ("Use a newer pg_sift"), and the entity
rows still answer.
