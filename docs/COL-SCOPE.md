# Scope index (EXPERIMENTAL)

A scope is a key a `sift_col` index cuts its rows by that the rows do not hold: they reach it through a table of `(member, scope key)` pairs. Events of entities, entities in tenants: the question
is "the events of tenant 7", and the tenant is in the pairs of the entity, not in the event. The scope index stores the rows replicated by scope key in segments of their own, cut by `(scope key,
time)`, so a read of one tenant reads the units of that tenant and no others, where the join reads the entity of each member. New in 5.0, column format minor 45, opt-in, and EXPERIMENTAL: the
declaration, the formats and the settings may change in a later release. `sift_table` has no scope. Unless a figure says otherwise, the figures on this page are user instructions of one backend, warm, on generated lab fixtures of 100,000 to 2,000,000 rows (blocks hit and read where stated): they say how the arms compare on those shapes, not what a real database will measure.

The alternative to the index is the [scope copy](#the-scope-copy-the-alternative), a second table kept by triggers; this page is about the index, in this order: [the expected case](#the-expected-case), [the declaration](#the-declaration), [what a read does](#what-a-read-does) and [what the planner recognises](#what-the-planner-recognises), [EXPLAIN](#explain) and the refusals, [what it costs](#what-it-costs), [the edges](#edges), what happens [without a scope index](#without-a-scope-index-replicate-or-reuse), and [the copy](#the-scope-copy-the-alternative).

## The expected case

A table of facts that many tenants (or groups) share, most of them small or medium, and requests that aggregate over one tenant or a few: sums and counts by kind or by month, a filter on a measure, distinct members, the top or the latest rows. Without a scope index such a read follows the table: it reads the units that hold a member of the tenant, so its cost grows with the rows of the table. With one it reads the units of the tenant.

Measured on `events` joined to a table of (member, tenant) pairs, 2.7 entries a row, the six requests of a dashboard (geometric mean of the instructions of the backend, shared buffers hit, blocks read cold; the right column is the plain `sift_col` index read by the member keys of the tenant, the better of two layouts of it; [the full table](#without-a-scope-index-replicate-or-reuse) has every size, every tenant and every request):

| events | tenant (of the events) | scope index | no scope index |
|---:|---:|---|---|
| 100,000 | 0.1% | 0.90 M instr, 56 hit, 8 cold blocks | 2.32 M, 52, 49 |
| 500,000 | 3.3% | 1.71 M, 94, 39 | 5.78 M, 198, 167 |
| 2,000,000 | 0.1% | 1.77 M, 152, 82 | 9.62 M, 502, 452 |
| 2,000,000 | 3.3% | 3.88 M, 247, 142 | 16.68 M, 648, 584 |
| 2,000,000 | 33% | 13.12 M, 728, 428 | 51.20 M, 816, 760 |

The scope index read fewer instructions than the member keys in every cell measured, from tenants of 0.1% to 33% of the events, and the rows are the same. The sums and counts by kind gain most (12.7 to 17.8 times fewer instructions at 2,000,000 events, across the four tenants). The price is size, build time and the promotion of the writes ([What it costs](#what-it-costs)): at 2,000,000 events the index grows by 93% at 1.05 entries a row and by 248% at 2.7. Declare a scope when the requests by a key are sums and counts by kind or time and the members are in few keys (up to 3 entries a row, the limit; the growth is what the `NOTICE` says); [the edges](#edges) say where the gain is small and what is not supported.

## The declaration

The recipe is two statements: a journal index on the table of pairs (it records the pairs that change), then the `scope` option on the index of the facts. `CREATE INDEX` says in a `NOTICE` which btree of the pairs it recorded and how many entries the scope holds and what the index grows by. The queries stay as the application writes them: the planner reads the join forms against the units (below).

    CREATE INDEX tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

`key_name: fk -> [schema.]relation(member, scope_key)`: the name of the derived key, the column of the index that holds the member key (`int2`, `int4`, `int8`, `text`, `varchar` or `uuid`), and the relation of pairs with its
two columns, integers or [text, varchar or uuid](#key-types). Up to 4 scopes, separated by `;`, each with its own name. The relation is a heap table (not a view, not the indexed table), with an immediate btree on exactly `(member,
scope_key)` in either order, unique (a primary key or a unique constraint) or not ([a pair may be several rows](#pairs-in-several-rows-a-multiset)), both columns NOT NULL; an unqualified name is in the schema of the indexed table. The index depends on that btree: `DROP INDEX`
of it, or `DROP TABLE` of the relation, needs `CASCADE`, which drops the index. The dependency is also the record of which btree defines the pairs: `CREATE INDEX` and `REINDEX`
choose it (a unique one before a non-unique one, then the lowest oid of those that qualify) and say which in a NOTICE, every read uses that one, and a btree made or rebuilt (`REINDEX .. CONCURRENTLY`) afterwards changes nothing until the next
`REINDEX` of the scope index chooses again; `ALTER INDEX` keeps it. `ALTER INDEX .. SET (scope = 'off')` or `RESET (scope)` removes the declaration; `REINDEX` drops the scope segments.
The time cut of the scope units is the first `date`, `timestamp` or `timestamptz` column of the index, else none.
A scope may also be the union of this relation and a relation joined through a second one ([two paths](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant)).

**The journal index.** The relation of pairs must have a `sift_col` index with `track_changes = 'ctid'` whose first column is the member, that holds the scope key and that has no `WHERE` (a partial index never hears of a pair inserted
outside its predicate, so it does not qualify). Without one `CREATE INDEX` is refused with a hint. The first scope index declared on a relation starts its journal before its build reads anything and stamps its segments with it: the writers of the relation of pairs wait
for the statement, as those of the table do for a `CREATE INDEX`. A `CREATE INDEX CONCURRENTLY` starts it at the end of its build instead, so its segments carry no stamp and the reads say
`unfilled` until a `REINDEX` of the scope index (or the rewrite the maintenance makes) takes it.

**`cols(..)`.** `scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(created_at, kind, value_num)'`: the scope segments of that scope keep only the listed columns of the index,
the member key, the column the units are cut by and the derived key; every other leaf has no chunk in them. A statement that reads a column outside the list reads the entity rows instead
(`Scope Fallback: column pad not in the scope units`). The subset is smaller: the scope segments of the generic test table are smaller with `cols(kind, value_num)` than with every column, and
smaller again with `cols(created_at)`; the entries are the same. A leaf with no chunk is NULL in every row of the unit and its zone says so: VACUUM, a promotion,
a rewrite, a merge and `sift.col_verify` read the units as they are, whether or not the list names the primary key (test `col_scope_cols_maint`).

## What a read does

A scan of the index with a scope conjunct (the rows whose member has a pair with a key) reads the scope units of those keys, less the members that changed in the relation of pairs since the
stamp of the units (the journal says which), plus the entity rows of those of them that have a pair with a key now (the patch), plus the delta: the rows of the write buffers, which no scope entry describes. The delta reads the buffer rows once,
keeps those whose member did not change, and copies each once for each pair of its member with a key under the statement's snapshot (the pairs come from the pair btree, the leaves of the journal index of the pairs or a scan of the
relation, whichever is cheapest), as the promotion would write them; the scan reads the copies as one more unit of each key, so the visibility check and the tombstones are those of the
write buffer's rows. A buffered row of a member whose pairs changed is read with the rest of that member's rows by the patch. A statement
that the scope cannot answer reads the entity rows of the members the relation lists for the keys (a read of every key by the key has no such fallback: below); the answer is the join's in every case (the tests compare md5 against the plain join after every
state: edits of pairs, writes, deletes, VACUUM, promotion, merge, rewrite, a crash at each directory update; the reader's own earlier commands, its savepoints and a cursor opened before a change: `col_scope_own_command`). The scan takes the scope conjunct from the join the way an application writes it
(the recognition below), or from the marker `sift.in_scope(index, key_name, fk, key or keys)` of the testing flavour (`sift.in_scope_pairs` for the meaning of a join).

## What the planner recognises

A statement that joins the scanned table to the scope relation of one of its scope indexes is planned twice: as written, and with that join taken out of the `FROM` list and its two
conditions, the equality of the member column with the declared column of the table and the binding of the scope key, made the scope conjunct of the scan (`EXPLAIN`: `Scope Index`). The
estimated costs of the two plans choose (and, at execution, the real keys: below): the scope read leaves the share of the rows that the pairs of the keys own (`pg_stats` of the key column of the relation, the most common keys by their
own frequency) of the distinct members of the table, where the join's key filter on the member column leaves the units that hold a member of the keys, most of them for facts ordered by time. The
rows are those of the join in every case; whether the index may be read at execution is the reader's verdict (`Scope Fallback`).

| form | counts a row | recognised |
|---|---|---|
| `JOIN scope_rel s ON s.member = f.fk WHERE s.scope_key = k` (also `IN (k1, k2)` with one distinct key) | once for each pair | yes, where the recorded btree is unique; with a non-unique one (a [multiset](#pairs-in-several-rows-a-multiset)) a join counts a fact once for each row: no |
| `f.fk IN (SELECT member FROM scope_rel WHERE scope_key = k or IN (list))`, `EXISTS`, `f.fk = ANY (ARRAY(SELECT member ..))` (planned also as the `IN`), a CTE of the members, a set-returning SQL function of them | once | yes, any number of constant keys |
| the same through a view, a security barrier view that projects the two columns, an inlined `STABLE` SQL function | as written | yes |
| a scope of two paths: `f.fk IN (SELECT member FROM rel WHERE key = k AND <predicate> UNION [ALL] SELECT m.member FROM rel2 g JOIN vrel m ON m.via = g.via WHERE g.key = k AND <predicates>)`, `EXISTS`, the same through a view or an inlined function | once | yes, any number of constant or parameter keys, the same in both selects |
| a scope of two paths: `JOIN (SELECT .. UNION SELECT ..) v ON v.member = f.fk WHERE v.key = k` (the `UNION` distinct, the member and the key projected, or the member alone), also through a view | once for each pair (once with the member alone) | yes, for one key, several keys, every key and the key as a group key (the node holds the join over the union, whose plan is a child of the node, to run where the units cannot count a row for each pair or cost more); `UNION ALL` under a join counts a member that both paths reach twice and keeps the join |
| `JOIN` with several keys (`= ANY ($1)`, `IN (k1, k2)`, a table of keys) | once for each pair | yes, where the plan holds the join as written (below): the node runs it where the units cannot count a row for each pair, or cost more |
| `JOIN scope_rel s ON s.member = f.fk GROUP BY s.scope_key` (every key: with other group columns, aggregates, `HAVING`, `ORDER BY` the key, a top-N over the groups, `count(DISTINCT ..)`; the key of type `int2`, `int4` or `int8` is the group column's type) | once for each pair | yes: every key is read as the derived leaf `<key_name>#scope`, and `WHERE s.scope_key = ANY (..)` with the key grouped by is the same read of those keys (below) |
| the key in an expression, an aggregate of it, `ROLLUP` / `CUBE` / grouping sets or `DISTINCT ON` over it, another column of the relation read, a range of keys, `GROUP BY` the key with a `LEFT JOIN` | | no |
| the scope key bound by a parameter: `= $1`, `= ANY ($1)`, `IN ($1, $2)`, a stable expression (`current_setting(..)::int`), the output of an InitPlan (`= (SELECT ..)`, `= ANY (ARRAY(SELECT ..))`) | as written | yes: the keys are those of the execution (below); a join (a row for each pair) only for a scalar, an array may hold several keys |
| a table of keys: `s.key IN (SELECT key FROM keys WHERE ..)`, `EXISTS`, `JOIN keys k ON k.key = s.key` | as written | yes, read once when the scan starts (below); a join (a row for each pair) only where the table gives one key: a unique index on the key column with the other columns bound, or a `DISTINCT` sub-select |
| the conditions of the relation that are the predicate of its partial unique btree (`AND s.deleted_at IS NULL`, in the `WHERE`, an `ON`, a plain view or a sub-select), stated as the btree has it | as written | yes: the pairs are the rows that satisfy it |
| a statement that does not state the predicate of the btree (the soft deleted rows are rows of the join and no pairs of the units), another condition on the relation, a join to a relation of a scope of two paths that is not the union of its two paths (its pairs are the union of the direct grants and those of the groups, the rows of none of them), a union with one select, another key or no key in a select, a select without the predicate of its btree or with another condition, `DISTINCT`, `LIMIT` or an aggregate in a select, a view with a condition of its own (`security_barrier`), `DISTINCT` or a join inside the sub-select, a view over a relation that has child tables or a sample (their rows are rows of the join and no pairs), `NOT EXISTS`, an outer join, row-level security on the relation, the relation inside a join in the sub-select of a semi join (`IN (SELECT .. WHERE x IN (SELECT .. FROM scope_rel ..))`) or in a sub-select that is no select of it alone (lateral, of several tables), in a plan of its own (`NOT IN`, `OR`, `f.fk = ANY (ARRAY(SELECT member FROM scope_rel WHERE ..))`, an InitPlan that runs before the scan and is no join to recognise: it is read as `IN (SELECT ..)` (the planner hook plans both and keeps the cheaper plan) where the sub-select has no `ORDER BY`, `LIMIT`, `DISTINCT` or aggregate and the member is a column of the scanned table; a scalar sub-select, a `MATERIALIZED` CTE or one used twice, the select list, an aggregate argument, `HAVING`), under a partitioned scan, a second join to a scope relation next to the one that is read | | no, and `sift.explain_why` says which |

**The predicate of the pairs.** The scope units hold the pairs, the rows of the unique btree of the relation that satisfy its predicate, so a statement is read from them only when the rows it takes from the relation, for the key it
binds, are those: every other condition on the relation must be implied by the predicate (it filters no pair out) and together they must imply it (it takes no row that is not a pair). `WHERE s.tenant_id = 4` over a btree
`WHERE deleted_at IS NULL` counts the soft deleted rows too, which the units do not hold, so the join runs as written (`the pairs of the scope are the rows of its unique btree that satisfy (deleted_at IS NULL): the statement does not state
it`); `WHERE s.tenant_id = 4 AND s.deleted_at IS NULL` is the pairs and is read from the units. A plain btree has no predicate and the relation no other condition. A condition that reads a parameter is proved only by a custom plan
(its value is a constant there): a generic plan keeps the join.

**Parameters.** A key that is a parameter of a prepared statement (or the output of an InitPlan, or a stable expression) is read when the scan starts, so a generic plan has the scope read for every value: the plan
is made once, and each execution has the keys, the verdict of the reader and the changed pairs of its own snapshot (a cached plan stays exact when the pairs change between two executions; NULL and the NULLs of an
array match nothing, an empty set reads no unit). `EXPLAIN` shows `Scope Keys: $1`, and a run `Scope Keys: $1 (2 keys)`; more keys than the dedupe lists or than `work_mem` run the join as written
(`Scope Fallback: dedupe keys`, `dedupe over work_mem`) as for constants. A custom plan has the constants and is priced for them. A generic plan has no value to price: a scalar is the pairs of the average key
(the rows `pg_stats` expect for an equality on the key column), an array that is not a list the 10 keys PostgreSQL assumes for an array it does not know, and the choice between the scope read and the join is made at execution, for the keys the
execution has (below).

**A table of keys.** The keys of a user, a team or a role are a table, and the statement reads them in a join (`JOIN keys k ON k.key = s.key WHERE k.user_id = 7`), an `IN` or an `EXISTS`. The planner takes the table out of
the join and has the scan read its keys once when it starts, in the statement's snapshot and with the table's own conditions (as the output of an InitPlan: `Scope Keys: ARRAY(SELECT ..)`), so the keys of each execution of a
prepared statement are the rows of that moment. The table is read by the scan's own sub-query and no longer by the statement, so it must be a plain heap or
partitioned table the role reads without row-level security, that the statement does not modify, with conditions that are immutable or stable and cannot raise an error. A join counts a fact once for each pair and each
row of the key table, so it is recognised only where the table holds the key once (a primary or unique non-partial index on the key column, the other columns bound by `=`, or a `DISTINCT` / `GROUP BY` sub-select);
`IN` and `EXISTS` count once and take any number of keys. A key table that is a view, a materialized or foreign table, a sub-select, a table with row-level security, one that is joined to another table or whose columns the
statement reads, keeps the join and says why in `sift.explain_why`. The price is made of the key table's rows (the estimate of its conditions; 10 for an unbounded set) and the pairs of the average key, so
a user of the few largest tenants of a skewed set is priced for the average ones. Measured on 300,000 events, 5,000 members and 50 skewed tenants (707 members and 42,420 facts in the largest, 50 and 3,000 in the smallest; `IN (SELECT .. FROM user_keys WHERE user_id = $1)`; instructions of the backend, prepared statement, warm, median of 5):

| keys of the user | node | join as written | scope read forced (constant keys) |
|---|---|---|---|
| 3 small tenants | 2.07 M (scope) | 4.42 M | 0.96 M |
| 1 tenant of average size | 1.46 M (scope) | 4.12 M | 0.35 M |
| the 10 largest | 12.35 M (join) | 12.14 M | 9.29 M |
| the 30 largest | 18.27 M (join) | 18.05 M | 15.88 M |

The node ran the join for the 10 largest and the 30 largest tenants (`Scope Fallback: join is cheaper for 10 keys`) where the scope read with constant keys costs less (the forced read has no key table to read, about 1.1 M instructions in the small cells): the choice is not always the cheaper arm for these keys. The same users through
`JOIN user_keys k ON k.tenant_id = s.tenant_id WHERE k.user_id = $1` read in 1.77, 1.72, 2.30 and 2.78 M against 5.6, 4.4, 24.1 and 39.1 M for the join as written.

**The scope read or the join, for the keys of the execution.** The plan of a scope read holds the join as written too (the first pass's scan, packed with the node), with its price in instructions at 1, 3, 10, 30 and 100 keys of
the average size and at every key (linear between them: `AGG_ALT_ROW` a row of the table, `AGG_ALT_PAIR` a pair of the relation, `AGG_ALT_OP` an operator of the model beyond the first key's, `src/agg/cost/model.h`). When
the scan starts it counts the entries of the keys it really has (a constant list, a parameter, an array, the rows of a table of keys), prices the units with `src/col/scope.h` (the entries, the
aggregation of a row the planner supplies, the dedupe lists the read made, the patch and the delta of the changed members) and runs the join where it costs less: `Scope Fallback: join is cheaper for 30 keys` (`for every key` for a read of every
key). The reader's refusals (`top-n, changed pairs`, `dedupe keys`, `too many changed keys`, the journal's words) are the same choice with the reader's reason: the join answers, which counts a row for each pair of a member in several keys
and the units cannot once the pairs of the member changed, so a read of several keys that counts a row for each pair never raises an error. A rescan decides again; `EXPLAIN` always says which way the node went (the scope lines, or
`Scope Fallback`). The join runs in the leader as a serial scan (a scope read has no workers). The generic plan has the same alternative: the key count it was priced for is the 10 PostgreSQL assumes for an array, the
execution has the real one. No plan holds the alternative where the join as written has no plan of this scan (the keys are the output of an InitPlan array, or the join is to a plan of its own: a table of keys read through a join that is
not a semi join of one key), where the scope index is on a view, or where workers were planned: those keep the choice of the plan (the estimated costs).
Measured on 120,000 and 150,000 events with 1,200 and 50,000 members, and 300,000 events with 5,000 members in several of 50 tenants (instructions of the backend, execution of a prepared statement, warm, median of 3):

| shape | keys | join as written | scope read | node |
|---|---|---|---|---|
| 30 members of 111 rows a key | 1 / 10 / 30 | 1.9 / 3.3 / 5.9 M | 0.45 / 2.9 / 7.3 M | scope / join / join (0.49 / 3.4 / 6.1 M) |
| 500 members of 3 rows a key | 1 / 10 / 30 | 3.5 / 4.2 / 7.3 M | 0.46 / 5.1 / 10.3 M | scope / scope / scope (0.49 / 5.1 / 10.4 M) |
| 110 members of 55 rows, 10% in two keys | 1 / 10 / 30 / 50 | 3.2 / 7.4 / 13.6 / 19.1 M | 0.46 / 6.4 / 20.4 / 48.2 M | scope / scope / join / join (0.50 / 6.5 / 13.8 / 19.3 M) |

and with changed pairs (the third shape, one tenant of 110 members, join 3.9 M clean): 20 pairs of the tenant changed, scope forced 12.2 M, node 5.1 M (join 4.4); 20 pairs of every tenant, 30 keys: scope forced 141.6 M, node 20.5 M (join 16.6);
every pair of every tenant, 30 keys: scope forced 501 M, node 26.8 M (join 17.6; the reader lists the changed members before it refuses). The node's choice is the cheaper of the two prices at the keys of the execution; in the 19 cells of `col_scope_price_terms` the units' price took the cheaper arm every time, and the join's price is read from the plan of the first pass at 1, 3, 10, 30 and 100 keys of the average size. A statement that is nearly one keeps the join and says why in `sift.explain_why` (arm `scope`, slug `scope`). `SET sift.agg_scope = off` turns the recognition off; a top-N, a `GROUP BY` with an
`ORDER BY .. LIMIT` and the aggregates are the scan's as ever.

**Many to many.** A member may be in several keys: a fact has one entry for each. A read of one key counts each fact of a member that has the pair once. A read of several keys with the meaning of
`IN` or `EXISTS` counts a fact once whichever of the keys its member has (`Scope Semantics: once`): the units of all but the least key of a member leave the member's facts out, from lists the read
builds from the pairs, bounded by `work_mem` and by 64 keys that have a list (`dedupe over work_mem`, `dedupe keys`: the entity rows are read then). A join and `GROUP BY` the key count a fact once for each pair
(`Scope Semantics: per pair`). A read per pair of several keys, and a read of every key, read the rows of a key whose pairs changed once from the entity segments and expand them by the pairs they have now (below: the changed pairs).

**Changed pairs.** The stored entries of a key whose pairs changed since the scope segments were written (the journal says which: added, removed, moved) leave the units at the source: the scan clears their bits in the liveness
selection of each unit whose zone of the key column holds one (`ColSource.sx`), so that no unit answers from its pre-aggregates, a segment from its summaries or a unit net of its dead rows when it holds one. The rows of those keys that
have a pair now are read once from the table segments by their key (the point directory of the key column names them; with none, the units whose zone leaves room for one are read, and the key chunk of each is tested), decoded only
for the columns the scan reads and at those rows, split by the visibility map (read after the rows, as VACUUM orders it) into the rows of all-visible pages and those the heap must check, and expanded by the pairs they have now with the
rows of the write buffers: one unit in memory for each key, the delta (`Scope Delta Rows`). Every row of the delta has its key, so the read of every key grouped by the key, and the read of several keys that counts a row for each
pair (`Scope Semantics: per pair`), no longer need the join for a changed pair, and a read of a key that has no pair now reads nothing beside the units. The pairs are listed from the btree of the relation with its predicate for the keys that
changed (never from a plain journal index of a relation whose btree is partial: the leaves list the soft deleted pairs too), and from the leaves of its journal index for every key; a pair with a NULL in either column is none.
Tests `col_scope_changed_pairs` (every state of the pairs, every read shape, every cell md5 equal to the join, the rows read bounded by the rows of the changed keys), `col_scope_groupkey`, `col_scope_delta`, `col_scope_patch`. A top-N
still cannot take the delta of a changed pair or of the buffers (`top-n, changed pairs`).

**The read is priced.** A key that has a pair now has its rows read from the table segments, and an index whose units are cut by time holds some of them in every unit: the read then opens every unit of them beside the scope units.
Each read counts what its two routes would cost and takes the cheaper. The terms are the constants of `src/col/scope.h` (instructions of the backend; the test `col_scope_price_terms` prints each one): 450,000 before any unit (the journal's vouch, the directory, the statement),
36,700 a unit read row by row or 13,300 a unit its pre-aggregates answer, 18 an entry of the keys plus what the planner counts for the aggregation of a row, 20 more for each entry when pairs changed (the source leaves out their stored entries) and 15 more for each key whose rows a drop list thins.
A read of several keys counted once (`IN`, `EXISTS`) lists the pairs of the relation whole (39 a pair, whatever the keys) and those of its keys (286 each) to dedupe them (`Scope Dedupe Keys`) and cannot answer a unit from its pre-aggregates.
A change since the stamp costs the journal 150,000 before its first changed member, then for each changed member 3,300 at 5,500 pairs in the relation (grown by 0.45 log2 of the pairs over 5,500) for the probe of its pairs, or the relations' pairs read whole from the leaves of their journal indexes
(138 a pair; 300 a pair of a relation whose pairs only its btree lists), whichever is less. The patch is 43,000 a unit of the table segments that holds a row of the changed members, 13 a row looked at to find them where no point directory of the member leaf names them, and 1,600 a copy of the delta
(a changed member's row or a buffered row, copied for each pair, made a unit and read). The entity path (the join as written) is 25 a pair of the relation read through its journal index (300 through its btree), 36,700 a unit of the rows that may hold a member of the keys, 25 an entity row of them and 70 an entry the key filter keeps;
the read that costs more says `Scope Fallback: patch dearer than the entity path`. The prices are pure functions of the counts of a `SiftScopeCounts` (`sift_scope_patch_price`, `sift_scope_members_price`, `sift_scope_list_price`, `sift_scope_units_price`, `sift_scope_entity_price` and
`sift_scope_entity_cheaper`: the keys and `once`, the units and entries, the share of the units the pre-aggregates answer, the planner's instructions an entry, the changed members, the units and rows the patch reads, the delta rows,
the pairs and the units and rows of the listed keys), so the executor (from the zones of the units it read) and the planner (from its estimates) decide from the same inputs. Changed members make the units of the keys they are in lose their pre-aggregates: the share of units answered from them falls as `exp(-1.8 changed / keys)`.
A patch and a delta that cost less than pricing them (1,000,000: listing the keys and counting their rows) are read without it. The changed members one read lists are bounded by the table rows of the index at 21 over the probe of a changed member, and by what the join as written
saves: the node takes the changed pairs its join leaves room for (its price less the clean read, over the probe) as the cap of the reader, which refuses (`too many changed keys`) as soon as it has counted one more, without listing the
pairs, and the read runs the join. The changed members are counted in a hash table, not sorted; there is no setting for the cap, and the testing setting `sift.test_scope_patch` takes the patch whatever it costs. A key cut index reads only the units of the keys it patches.
The test `col_scope_price_terms` compares the price with the read measured in 19 cells (instructions of the backend, warm, the scope read forced; shapes of 30 keys of 111 rows, 500 of 3 rows and 110 of 55 rows with a tenth of them in two keys): the arm the price takes is the cheaper one in every cell, the 13 cells of clean reads are priced within 8%
and the 6 cells with changed pairs from 19% under to 20% over the measurement. The fits that gave the terms were made on fixtures that this page no longer lists; the test is the record of the figures.

**The delta is cheap to expand.** The copies of a read are sorted by (key, address) and the pairs by (key of the fact, tenant): a radix sort of the entries (a few passes over the key ranges) replaces the comparison sort (a call of a
function for each of some twenty steps of each entry), which were more than half of the read of 2,000 buffered rows for every tenant: 12.3 million instructions (2,190 copies, 4,730 each over the clean read's 1.94) became 7.1 (2,330 a copy; the decode of the buffers is the same
for the join). The price takes 1,600 instructions a copy.

**Every key, grouped by the key.** A read of every key (`Scope Keys: all`, no key list, `Scope Semantics: per pair`) reads all the scope units, a fact once for each pair of its member, and its group key is the derived leaf
`<key_name>#scope`: every entry has its key (`Scope Group Key: <key_name>`), so the scan groups by it as the engine groups by any leaf. The units of a key hold that one key (the units of small keys share a unit, a run of keys), so a
unit with no member to leave out answers from its pre-aggregates, histograms and sums of its entries, with no hash and no row read (`Units From Pre-aggregates`); the gate that kept a read of several keys from them was written for the dedupe of `IN` and `EXISTS`, which a read per pair does not
have. The rest is as for one key: the stored entries of the keys that changed leave at the source and their rows are read once from the table segments and expanded by the pairs they have now (above), and the rows of the write
buffers are the delta, a copy for each pair of their dim_a_id. The planner chooses between the read and the join at the start of the scan from the price of one function (`sift_scope_entity_cheaper`, with the counts of the read: a
read of every key has no entity path of its own, so only the planner can take the join). The planner recognises the join: `JOIN scope_rel s ON s.member = f.fk GROUP BY s.key` (tests `agg_scope_group_key`, `col_scope_groupkey`, `col_scope_changed_pairs`, `agg_scope_choice`) is planned as written and again with the relation taken out, and the node holds the join as written, which it runs where it costs less (below), so the reader's refusal of a read that counts a row for each pair never raises. Measured on 300,000 events,
5,000 dim_a_ids and 50 tenants (instructions of the backend, warm, median of 5; `sum(amount), count(*)` for every tenant): the read costs 1.99 million instructions (86 units from pre-aggregates, 409 blocks hit; the join 53.0 million, 206 hit),
with 2,000 buffered rows 7.1 million (2,190 copies; the join 53.6; 12.3 before the copies were sorted by radix), with 20 changed pairs 39.4 million (a unit with a changed key's entries is read row by row; the join 56.2; the
reader refused it after 0.94 million instructions, which the join added to its 56.7) and with 200 changed pairs 89.5 million (the join 56.6: the delta's 25,140 copies cost more than the join, which the planner will choose).
The node chooses by itself: a read grouped by the key answers the units that hold no changed member from their pre-aggregates and reads the units of the changed members row by row (the terms above; the share of the entries in such units is 1 - exp(-1.8 changed / keys)), and the join as written costs 110 instructions a row of the keys beyond the first more than the model's operators count (`AGG_ALT_JOIN`). Clean, with 20 and with 200 changed pairs of the same table: the node runs the scope read in 1.4 million instructions (the join 53.0), the scope read in 38.9 (the join 56.0), and the join in 58.2 (`Scope Fallback: join is cheaper for every key`; the scope read forced costs 90.8).

## EXPLAIN

A statement that cannot be read from the units is still answered, by the join as written, and says why. `EXPLAIN` shows `Scope Index` when the units were read and `Scope Fallback: <why>` when they were not; `SELECT * FROM sift.explain_why('<query>')` (arm `scope`) returns the reason without running the statement. `SET sift.agg_scope = off` gives the join as written for a comparison.

`Scope Index`, `Scope Keys` (`all` for every key), `Scope Group Key` (a group key of the scan is the derived key), `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows` (the rows of the entity segments read for the changed keys that have a pair now), `Scope Delta Rows` (the copies of the rows of the write buffers and of the patched rows the scan reads, one for each pair), `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled`, `several stamps`, `scope segments do not cover the rows`, `write buffer` (the pairs of the buffered rows are more than work_mem holds), `top-n, changed pairs` (a top-N cannot take the patch or the delta of a changed pair),
`dedupe over work_mem`, `dedupe keys`, `patch dearer than the entity path`, `join is cheaper for N keys` (the node ran the join as written), `pairs are a multiset (non-unique btree): a join counts a fact once for each row` (a read for each pair of the pairs of a non-unique btree), and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the next `REINDEX` of
the scope index fills it again), `trimmed`, `snapshot` (a serializable or non-MVCC snapshot), `snapshot older than the stamp`, `stamp aged`, `too many changed keys`. A scope read is decided once, by the
leader (the verdict, the changed members and the patch are made from the snapshot of the statement): it has no workers.

**Where the conjunct may be.** The scan takes the conjunct when it filters the rows the scan reads: the `WHERE` of the statement (when no outer join of the scan can null the scanned table), the conditions
inside the nullable input of an outer join (the `ON` conjuncts that read only that input, the `WHERE` of a sub-select or of an inlined CTE: pushing them below the join is exact), a lateral child, a
materialized CTE (its own scan). The dashboard shape, a small table of periods left-joined to the scoped events, reads the scope units of the key. A conjunct that does not filter the scanned rows
stays an ordinary filter, which the scan tests on every row, and `EXPLAIN` says why in `Scope Fallback`: the `ON` clause of a join that keeps the scanned rows (a `LEFT JOIN` from the events, a `FULL
JOIN`: the rows that fail it are output rows), a conjunct over the rows an outer join made of the unmatched ones (`WHERE conj OR e.id IS NULL` on the nullable side: the rows the join made pass
it), an expression around it, a key that is not a constant, a second scope (a scan reads one), the setting off. A `WHERE conj` over the nullable side of a `LEFT JOIN` drops the unmatched rows, which
the scan cannot do: the statement keeps the normal plan (`sift.explain_refusals`: `outer_join`).

## What it costs

`CREATE INDEX` and `REINDEX` say it for each scope: `NOTICE: scope "tenant_id": 93,340 entries for 100,000 rows (0.93 per row); the index grows from 1.4 MB to 2.3 MB (+67%)` (the NOTICE counts the bytes of the
segments, not the file, and says the growth from 1 MB of entities). The scope segments are written with the entity segments by the build, the parallel build, a promotion and a merge, in the same switch of the directory; a rewrite fills
them again when the journal says that many members changed (`sift.col_connect_refresh_pct`), when the reads that pay for the changed members cost more than the rewrite (a thousand
reads against the 1,500 instructions an entity row and 680 a column of an entry the rewrite takes: 20 members of a table of 60,000 rows are refreshed, 1 is not) or an entity segment lost its flag. The relation of pairs pays the journal index: a 32-byte record for each pair an UPDATE or DELETE ends and each pair inserted.

The fixture of this page (100,000 events of integer id, member, kind and value and a `timestamptz` over a year; 5,000 members, 4,667 of them in one of 40 tenants, so 0.93 entries a row; a 5-column index with the default choices; one backend, no parallel
build): the index file is 1,368,064 bytes without the scope and 2,383,872 with it (+74%; 2,252,800 with `cols(created_at, kind, value_num)`, +65%); the build takes 248 to 375 ms without and 321 to 466 ms with it (3 runs on a shared host: the difference is within the
spread), and writes 1.34 MB of WAL without and 2.27 MB with it (the journal index of the pairs not counted). Keys of fewer than 2,048 entries share the units of the scope segments, and the
scope segments have no point directory and no global order (`order_array_columns` writes its arrays in the entity segments only: an array lists each row of a segment once, a scope segment holds a
fact once for each of its keys, and the reads of a key go by unit), which the reads of a key do not use.

**Size, build and WAL by entries a row.** The same table as [the expected case](#the-expected-case) (`sift.scope_max_multiplicity` raised for the 5.4 rows; entries a row = multiplicity):

| events | entries a row | entity index | with the scope | build | WAL of the build |
|---:|---:|---:|---:|---:|---:|
| 500,000 | 1.05 | 13.0 MB, 1.4 s | 25.4 MB (+95%) | 2.4 s | 50 MB (13 MB without) |
| 500,000 | 2.7 | 13.0 MB | 44.2 MB (+240%) | 5.0 s | 89 MB |
| 500,000 | 5.4 | 13.0 MB | 78.0 MB (+500%) | 5.7 s | 157 MB |
| 2,000,000 | 1.05 | 55.3 MB, 10 s | 106.5 MB (+93%) | 11.2 s | 215 MB (55 MB without) |
| 2,000,000 | 2.7 | 55.3 MB | 192.3 MB (+248%) | 21.4 s | 557 MB |
| 2,000,000 | 5.4 | 55.3 MB | 335.2 MB (+506%) | 64.8 s | 676 MB |

These rows are of the lab of the reuse measurements below. The growth in percent follows the size of the entity index, which follows the data: a build on the fixture of "What it costs" scaled to 500,000 events (25,000 members, 200 tenants, 2.70 entries a row, one 5-column index, serial build) went from 6,938,624 to 20,152,320 bytes (+190%; 9.8 bytes an entry where the row above has 23), 1.2 s to 3.6 s and 6.9 to 20.2 MB of WAL. The +254% and +255% of the table below are of the lab of the by-address study, whose entity index at 500,000 events is 12.4 MB.

**Writes.** The statement that writes the facts costs the same with and without the scope: the rows wait in the write buffer of the index, and the scope entries are written when the buffer is promoted, in the same switch of the directory. Ten thousand inserted facts (a 2,400,000-row table, 1.05 entries a row; the instructions of the backend; the WAL of the statement, about 4 MB for the heap and its two b-trees, varies by 0.3 MB from run to run, so it is not a difference):

| 10,000 facts | no `sift_col` index | entity index | with the scope |
|---|---:|---:|---:|
| INSERT statement | 224 M instr | 380 M | 360 M |
| promotion of the buffer | | 224 M, 0.24 MB WAL | 404 M, 0.39 MB WAL (+180 M, +0.15 MB) |
| promotion at 9,600,000 rows | | 217 M, 0.26 MB | 463 M, 0.48 MB (+246 M, +0.22 MB) |
| promotion at 500,000 rows, 2.7 entries a row | | 74 M, 0.3 MB | 470 M, 1.1 MB (+396 M, +0.8 MB) |

So the promotion is where the scope is paid: +180 to +246 M instructions and +0.15 to +0.22 MB of WAL for each 10,000 facts at 1.05 entries a row, +396 M and +0.8 MB at 2.7, and it grows with the entries a row and little with the size of the table (+180 M at 2,400,000 rows, +246 M at 9,600,000). UPDATE and DELETE of the facts were measured at 2.7 entries a row only (1,021 and 493 M instructions for 10,000, the same with and without the scope).
A pair edit costs its journal record: 100 pairs ended by one UPDATE took 18 to 21 M instructions and 40 to 46 KB of WAL with the journal index, 13 M and 16 KB without it; 100 pairs inserted 6 to 7 M and 35 KB, against 7 M and 14 KB (9,600,000 rows). That is about 220 bytes of WAL for each pair edit over the 32-byte record, which includes the pages of the journal. Reuse costs nothing to write.

**An access model of nine tenants.** A table of facts (a 13-column index, one scope of two hops: grants to a member directly and to a group of members, 1,674 members, 1.05 entries a row) of nine tenants of 2.2% to 36% of the facts, the nine requests of a dashboard (sum and count by kind and by month, count by kind, latest 20, top 20 members, distinct members by month, by member, a filter on two columns, a date range, a count over a nested array); the statement is written with the view of the pairs and is the same in the three columns. Geometric mean over the nine requests, instructions of the backend (warm), shared buffers hit (warm) and blocks read with the shared buffers evicted (cold); every cell returned the same rows in the three arms. The larger tables are disjoint copies of the 2,400,000-row table (the shares of the tenants are the same), not other data:

| facts | tenant | join (member keys) | scope index | stock PostgreSQL (`sift.enable_agg = off`) | index: entity / with the scope, build, WAL |
|---:|---:|---|---|---|---|
| 100,000 | 36% | 12.8 M instr, 123 hit, 85 cold | 4.7 M, 60, 32 | 139 M | 3.2 / 5.5 MB (+75%), 0.6 / 0.9 s, 3.1 / 5.4 MB |
| 100,000 | 2.2% | 12.9 M, 128, 92 | 2.7 M, 29, 12 | 85 M | |
| 500,000 | 36% | 36.1 M, 429, 312 | 10.9 M, 191, 123 | 705 M | 14.6 / 26.4 MB (+81%), 2.7 / 7.6 s, 14.6 / 26.5 MB |
| 500,000 | 2.2% | 30.3 M, 394, 302 | 3.4 M, 46, 29 | 417 M | |
| 2,400,000 | 36% | 94.9 M, 1,127, 863 | 34.4 M, 747, 510 | 3,463 M | 60.9 / 110.9 MB (+82%), 13.8 / 17.7 s, 61 / 111 MB |
| 2,400,000 | 12% | 84.9 M, 1,159, 943 | 14.4 M, 324, 243 | | |
| 2,400,000 | 2.2% | 47.1 M, 673, 548 | 5.8 M, 130, 98 | 2,085 M | |
| 4,800,000 | 36% | 254 M, 3,222, 2,259 | 62.1 M, 1,305, 874 | 6,844 M | 110.8 / 208.8 MB (+88%), 20.2 / 41.9 s, 111 / 210 MB |
| 4,800,000 | 2.2% | 98.9 M, 1,295, 852 | 8.4 M, 148, 109 | 4,167 M | |
| 9,600,000 | 36% | 477 M, 6,194, 1,639 | 117 M, 2,535, 1,775 | 9,471 M | 223.1 / 418.3 MB (+88%), 32.7 / 60.7 s, 224 / 422 MB |
| 9,600,000 | 2.2% | 184 M, 2,456, 1,653 | 13.9 M, 286, 227 | 499 M (an index plan) | |

The scope index read fewer instructions than the join in all 243 cells of 100,000, 500,000 and 2,400,000 facts (9 tenants, 9 requests each), from 0.66 (the 36% tenant, count by kind) to 0.05 of the join, and the cold blocks of the two tenants shown fell by 1.7 to 10 times; the saving grows with the table (the 2.2% tenant: 10 M instructions at 100,000 facts, 170 M at 9,600,000), and no tenant share measured, up to 36%, turned it into a loss when the pairs were unchanged. A tenant of 36% saves 3 to 4 times the instructions and reads about as many cold blocks at 9,600,000 facts (1,775 against 1,639: the join reads the entity units that hold the member keys, the scope the units of the key). The requests that gain least are the count by kind, by member and the top 20 members (0.57 to 0.66 of the join at the 36% tenant); the date range and the latest 20 gain most (geometric means 0.09 and 0.13 at 2,400,000 facts). The statements of the 4,800,000 and 9,600,000 rows were read for four tenants; the latest 20 of those two sizes was measured with the scope read first in the session, because a scope read of it that follows the join read of the same statement in one session returned other rows there (a known defect, under investigation); every other cell is equal in any order.

**After writes, the same table (2,400,000 facts).** 600 facts in the write buffer and 20 pair edits (10 of the 36% tenant's direct pairs ended, 10 of its members granted to the 2.2% tenant, 5 members of a group of the 27% tenant left it and 5 joined it); the maintenance had not rewritten the segments (`sift.col_auto_merge = off` in this lab), the rows are the same in all cells. Instructions of the backend, scope against join:

| tenant | what changed | scope | join | the scope read |
|---|---|---:|---:|---|
| 36% | 10 of its pairs ended | 13 to 224 M | 13 to 223 M | the join, `join is cheaper for 1 key` (ratio 1.00 to 1.04) |
| 27% | 5 members of a group left, 5 joined | 51 to 166 M | 51 to 239 M | patch and delta: 0.43 to 0.73 for sums, distinct, nested array; 1.08 to 1.15 for latest 20 (`top-n, changed pairs`) and top 20 members |
| 2.2% | received 10 members of the 36% tenant | 32 to 150 M | 25 to 107 M | **the scope patch, dearer than the join**: 1.3 to 4.0 times, 97.5 against 27.9 M for the top 20 members, 105.0 against 26.4 M for the filter (the patch reads the 22,262 rows of the 10 members, 3,300 hits against 500) |

A tenant that received members is the case where the planner chose the scope read and the join was the cheaper one: the price of the patch is below its measured cost when the changed members hold a large part of the facts of the tenant (a reproduction is kept with the notes of the item). Until a rewrite (`REINDEX`, or the maintenance when the share of changed members or the cost of the reads calls for it) a tenant with changed pairs reads as the join at best and as 1.3 to 4 times it at worst, and a table where members of large tenants move often gets less of the saving.

**Building at the same time.** The build of a `sift_col` index that tracks changes installs the two triggers of its log on the table when it has read it, which takes `ShareRowExclusiveLock`
while `CREATE INDEX` holds `ShareLock`: two such builds on one table that ran together (`pg_restore -j`) each waited for the other. An event trigger before `CREATE INDEX` of a `sift_col` index
(`sift_col_build_lock`) takes the stronger lock first, so the second build waits at its start; the options a build decides are written under the tuple lock of the catalog row of its index. `REINDEX`
of two indexes of one table at the same time is not covered by the event trigger (PostgreSQL has none for it).

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
the heap rows and their trigger as well; the units live in the index and flip at 3. The reads of the units are not cheaper at a higher multiplicity. A larger table is expected to move the break-even
up (the join is linear in the table; nothing above 2,000,000 events was measured), which this limit does not price: raise it for such a table, from the figures of the NOTICE.

## Verifying a scope index

    SELECT * FROM sift.col_scope_verify('events_col');                  -- 10 random keys; no row: consistent
    SELECT * FROM sift.col_scope_verify('events_col', 50);              -- 50 keys
    SELECT * FROM sift.col_scope_verify('events_col', keys => ARRAY[7]); -- the key named

`sift.col_scope_verify(index, sample, keys)` runs, for each key and each scope of the index, the statement the planner recognises (`f.fk IN (SELECT member FROM rel WHERE key = k AND <predicate>)`, or the union of the two paths) twice in the
snapshot of the caller: through the scope units (`sift.enable_agg` on, `sift.agg_force`) and through the plain plan (`sift.enable_agg` off, the executor of PostgreSQL alone). Each side answers the number of rows and a signature of the first
column of the index for each member, and the function reports the keys whose two answers differ: `problem = 'differs'`, `scope_rows` and `join_rows` (the rows of each side) and `detail` (how many members differ and the first). The keys are
drawn at random among those that have a pair now (`sample`, default 10), or are the ones named by `keys`, which may also name a key with no pair: the check that the units hold nothing for a key whose pairs all went. A key whose answer was not
read from the stored units (a `CREATE INDEX CONCURRENTLY` not yet followed by a `REINDEX` says `unfilled`, a `Scope Fallback` says its reason) has `problem = 'unverified'` and the reason in `detail`: nothing was compared for it. No row means every
sampled key was read from the units and equals the join. It reads each sampled key twice and a third time to learn whether the units were used, and the plain side is the join of the key's members, so run it on a few keys of a large table;
it changes no setting of the session and no data, and runs with the privileges of the caller. In `REPEATABLE READ` it checks the snapshot of the transaction, on a standby the replayed state (a read-only session). `sift.col_verify(index, true)` checks the structure of the segments against the heap.

**Replication and upgrade.** A physical standby replays the index and reads it (`test/t/col_scope_replica.sh`). The facts and the pairs are heap tables, so a logical subscription carries them: a subscriber that has its own scope index (the facts, the pairs and their journals made there before the subscription copies the rows) keeps it as the apply worker writes the rows, since the journal of the pairs is kept by row triggers that fire for the apply worker, and its reads are the publisher's join; the index itself is not replicated. The oracle gate (`test/t/col_scope_oracle.sh`) compares every read of the primary, a standby, such a subscriber and a restored dump with a node that never loads pg_sift. `pg_upgrade` carries the index with its options and its dependencies on the btrees; the journals and then the scope index are rebuilt after it (`test/t/pg_upgrade.sh`).

## Settings

| Setting | Default | Gain | Impact | Why the default | Off |
|---|---|---|---|---|---|
| `sift.agg_scope` | on | a read of one key from the units: 1.0 to 1.1 million instructions where the entity rows of the members cost 1.9 to 2.0 (the fixture of "What it costs", sum and count by kind of one tenant, prepared statement, warm, one backend); two keys 1.55 million against 2.09 | nothing stored | a scope exists only when the user declares it | `SET sift.agg_scope = off`: the statement is the join as written; no `REINDEX` |
| `sift.scope_max_multiplicity` | 3 | `CREATE INDEX` refuses a scope whose members are in more keys than this on average, with the size and growth it would have had; the join answers the reads (see Break-even) | none | the units break even with the join at 2.5 (500,000 events) to 3.0 (2,000,000) entries a row | `SET sift.scope_max_multiplicity = 1000000` accepts any figure |

## Edges

The expected case needs nothing of what follows. The edges, each with what happens there:

- [where the gain is small](#where-the-gain-is-small): the requests that gain least, latest-N and top-N while pairs changed, small tables, members in many keys;
- the shapes of pair tables the declaration also takes: [soft deleted pairs](#pairs-that-are-soft-deleted-a-partial-unique-btree), [a pair in several rows](#pairs-in-several-rows-a-multiset), [two paths](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant) (two hops: a grant on a member or on a group);
- key types, partitioned tables and the other [limits](#limits).

### Where the gain is small

Tenant size is not a limit: in the table of [the expected case](#the-expected-case) and of [the member keys](#without-a-scope-index-replicate-or-reuse) the scope index read fewer instructions at every tenant share measured, 0.1% to 33% of the events. What differs is the request. At 2,000,000 events and the 33% tenant the member keys cost 13.3 times the instructions of the scope index for sums by kind, 10.2 for the same in one month, 6.9 for the latest 20 and 2.5 for a filter on the value; the gain is smallest for the top 20 members (1.0, a tie) and for distinct members by month (1.4), the requests that read rows of the largest keys. At 0.1% the same two gain 6.6 and 4.6 times.

- **Latest-N and top-N and changed pairs.** A top-N of the scan (`ORDER BY .. LIMIT n`, which is what a latest-N read is) cannot take the patch or the delta of a changed pair: while pairs of the keys changed since the segments were written, or rows of their members wait in the write buffer, it reads the entity rows and says `Scope Fallback: top-n, changed pairs` (test `agg_scope_param_keys`).
- **Small tables.** At 100,000 events the member keys cost 2.32 to 5.09 M instructions and the scope index 0.90 to 1.93 M (a saving of 1.4 to 3.2 M) on the six requests of the first table; on the nine requests of [the access model](#what-it-costs) at 100,000 facts the join costs 12.8 to 16 M (2 to 5 ms) and the scope index 2.7 to 4.7 M (0.4 to 1.9 ms); [the break-even](#break-even-the-multiplicity-limit) at that size is 2.0 entries a row.
- **Members in many keys.** Above 3 entries a row `CREATE INDEX` refuses the scope and says the size it would have had; the join answers, at the cost of [the member keys](#without-a-scope-index-replicate-or-reuse) and nothing more. Leave a scope undeclared too when the extra size, build and the 0.8 MB of WAL for each 10,000 facts of promotion are not worth the saving.
- **Changed pairs of a tenant that received members.** Until the segments are rewritten the read of a changed key patches the members that changed; when those hold much of the tenant's facts the patch costs 1.3 to 4 times the join and the planner still chooses it ([measured](#what-it-costs)).
- **The two plans.** With both a scope index and an entity index on a table, the planner chose the scope read in all 48 cells measured (2,000,000 events at 2.7 entries a row and 500,000 at 5.4, four tenants, six requests), and it was never more than 1% dearer than the join read measured on its own (the closest cells: top 20 of the 33% key, 82.8 against 85.3 M instructions at 2,000,000 events, and 22.3 against 22.1 M at 500,000 events and 5.4 entries a row).

### Pairs that are soft deleted: a partial unique btree

The pairs may be the rows of a PARTIAL btree of the relation: the predicate says which rows are pairs, and the rows that satisfy it are unique on the two columns (a partial btree that is not unique makes
the pairs [a multiset](#pairs-in-several-rows-a-multiset) of the rows that satisfy it). A bridge table that soft
deletes keeps its all-rows unique btree (the primary key) and adds the one that defines the pairs:

    CREATE UNIQUE INDEX memberships_active ON memberships (dim_a_id, group_id) WHERE deleted_at IS NULL;
    CREATE INDEX memberships_journal ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'group_id: dim_a_id -> memberships(dim_a_id, group_id)');

No table exists for the index alone, and the application deletes as it does. An `UPDATE` that sets `deleted_at` ends a pair and one that clears it creates one: the journal records the member of both,
so a read takes the stored entries of the members that did not change and reads the entity rows of those that did. A statement that updates a column no index holds is a HOT update and writes
nothing to the journal. The scope entries are the active pairs only (soft deleted rows have none), and the answer is the join on the active rows in every state.

- **Which btree.** `using <index>` after the columns names it (`memberships(dim_a_id, group_id) using memberships_active`); without it, among the unique btrees on exactly the two columns if there is one, else among the
  non-unique ones, the only partial one, else the lowest oid of the plain ones; two partial ones of that kind are an error that asks for `using`. The `NOTICE` of `CREATE INDEX` and `REINDEX` names it and its predicate; the index records it (the dependency), every read uses it, and one made
  later changes nothing until `REINDEX` chooses again. `ALTER INDEX` with another `using` than the recorded btree is refused: the segments were written under the recorded one.
- **Journal index.** A plain one, or a partial one whose predicate the predicate of the pairs' btree implies (`WHERE deleted_at IS NULL` beside the same predicate: a soft delete writes one record). A partial one
  that a pair can be inserted outside of does not qualify: it never hears of that pair.
- **What the predicate can be.** What PostgreSQL accepts in an index predicate: immutable, over the columns of the row (`IS NULL`, comparisons with constants, boolean columns). `now()` is refused by PostgreSQL
  (`functions in index predicate must be marked IMMUTABLE`), so a pair that ends by the clock needs a column the application or a scheduled `UPDATE` sets (`UPDATE grants SET expired = true WHERE expired_at <= now() AND NOT expired`,
  a journaled write: exact as of the sweep, so the staleness is the sweep interval), or the copy path ([INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view), which has `sift.scope_expire_pairs`).

### Pairs in several rows: a multiset

A table that keeps a row for each source of a pair (a grant for each way a tenant got a dim_a_id, a row for each role that put a dim_a_id in a group) holds a pair in several rows, and its application does not keep a helper
table only for the index. Declare the scope on a btree that is not unique, plain or partial, on exactly the two columns, in either order:

    CREATE TABLE tenant_pairs (id serial PRIMARY KEY, row_src int NOT NULL, dim_a_id int NOT NULL, tenant_id int NOT NULL);
    CREATE INDEX tenant_pairs_pair ON tenant_pairs (dim_a_id, tenant_id);
    CREATE INDEX tenant_pairs_journal ON tenant_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> tenant_pairs(dim_a_id, tenant_id)');
    NOTICE:  scope "tenant_id": the pairs of "tenant_pairs" (dim_a_id, tenant_id) are the distinct rows of its non-unique btree "tenant_pairs_pair"
    DETAIL:  A pair may be several rows: a read with the meaning of IN or EXISTS counts a fact once, a join counts it once for each row and runs as written.

A pair exists while at least one visible row that satisfies the predicate of the btree (if it has one) holds it: the PAIRS are the distinct `(dim_a_id, tenant_id)` of those rows. Deleting one of two rows keeps the pair, deleting the
last one ends it, a soft delete (a partial btree `WHERE deleted_at IS NULL`) of one of two rows keeps it and of both ends it, and moving the key of one row adds the new pair and keeps the old one while another row holds it. When
the relation has several btrees on the two columns the choice is: a unique one before a non-unique one, among those the only partial one, else the lowest oid of the plain ones (two partial ones of that kind are an error that asks
for `using`); `using <index>` names the one, and the `NOTICE` says which.

- **Exact because the journal names at least every changed dim_a_id.** The journal index records the dim_a_id of every row an `UPDATE` or `DELETE` ended and of every row inserted, whether or not the set of distinct pairs of that dim_a_id changed. A read
  takes the stored entries of the dim_a_id that did not change and reads the rows of the facts of those that did (the patch), expanded by their pairs under the snapshot of the statement. A duplicate row inserted or deleted
  therefore costs the patch of its dim_a_id, and a pair that did not change is read again from the btree: never a wrong answer. Nothing counts the rows of a pair when it is written (a refinement that skipped the record when
  another live row exists would be wrong under concurrency: two sessions that each delete one of two rows).
- **Every list of the pairs lists each once**, through one function (`sift_scope_pairs_distinct`): the fill from the whole relation, the fill that probes the btree for the dim_a_id of each batch, the reader's pairs of the keys and of
  the changed dim_a_id (the btree probed by key or by dim_a_id, the leaves of the journal index, which hold a row for each row of the relation, the relation read once) and the union of the two paths of a scope of
  two paths ([where a pair in both, or in several rows of each, is one pair](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant)). A pair taken twice would put a fact twice in the units of its key, which `sift.col_verify` and a build refuse
  (`rows must be added in ascending TID order`), and would count a fact twice in the delta. Tests `col_scope_pairs_multiset` (every state by every read shape), `col_scope_pairs_multiset_fill` (runs of thousands of rows of one pair
  across the leaf pages of the btree, in either order of its columns, partial, by each way the pairs are known: `sift.test_conn_fill`), `col_scope_pairs_multiset_partial`, `col_scope_pairs_multiset_twohop`.
- **Which forms are exact.** A read with the meaning of `IN` or `EXISTS` counts a fact once whatever the rows of the pair and is read from the units. A `JOIN` to the relation counts a fact once for each row of a pair, and a read
  for each pair (`GROUP BY` the scope key, several keys counted for each pair) once for each row too, which the units (one entry for each distinct pair) cannot: those run as written, the plan says why
  (`sift.explain_why`, arm `scope`: `pairs are a multiset (non-unique btree): a join counts a fact once for each row`; the testing marker `sift.in_scope_pairs` says it as `Scope Fallback`), and their rows are the join's.
  The planner learns it from the btree the index records (a dependency of the index, so a btree made later changes nothing until `REINDEX`). A scope of two paths defines its pairs as the distinct union, so a read for each pair of it counts each pair once whatever the rows hold.
- **The scope copy is unaffected.** `sift.scope_create` still needs the unique btree (a pair in two rows would give a fact two copies in one key) and refuses the relation of a multiset with its usual error; the pairs kept by
  `sift.scope_follow_pairs` are written by sift as the distinct rows of a view.
- **What it costs.** The same as for unique pairs: the units hold one entry for each distinct pair, a read of a dim_a_id whose rows were not written since the stamps is the stored read. The duplicate insert and delete
  of the test (1.13 rows a pair) patch the dim_a_id they touch like any other write to the relation.

### Pairs reached by two paths: a direct grant and a group grant

A tenant sees an entity when it has a grant on the entity or on a group the entity belongs to (a subscription). The pairs are not the rows of one relation but the union of two paths, and a
member can be reached by both. The declaration names the three relations and no table holds the union:

    facts(id, dim_a_id, ..)
    grants(id, tenant_id, dim_a_id, group_id, deleted_at, ..)     -- a grant names a member or a group; both columns may be NULL
    memberships(id, group_id, dim_a_id, deleted_at, ..)
    CREATE UNIQUE INDEX grants_direct  ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
    CREATE UNIQUE INDEX grants_group   ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
    CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
    CREATE INDEX grants_by_member         ON grants      USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX grants_by_group          ON grants      USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
    CREATE INDEX memberships_by_member    ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
    CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');

**The grammar.** One alternative after the `->`: `key: fk -> rel(member, key) [using ix] | rel2(via, key) [using ix2] via vrel(via, member) [using ix3] [cols(a, b)]`. The first relation is
the direct path (the rows of `rel` are pairs), `rel2` the joined path (a row of `rel2` gives its key to every member of its `via` value), and `vrel` lists the members of a `via` value. A text
that names a second path without `via` and its relation, or a relation with one column, is refused with what is missing. The three sets of columns must differ (`grants(dim_a_id, tenant_id)` twice,
or reversed, is refused), the columns of one role are of one integer type (`via` in `rel2` and `vrel`, `member` in `rel` and `vrel`, `key` in `rel` and `rel2`), and a relation is a heap table that
is not the indexed table. The columns of a scope of two paths may be nullable (a grant on a group has no member, a grant on a member no group): a row with a NULL in a column of its path is not a
pair of that path, as a join drops it; the columns of a scope of one path are still NOT NULL.

**The three btrees.** Each relation has its own recorded btree on exactly its two columns, unique or not, with the rule of the pairs of one relation (above): `CREATE INDEX` and `REINDEX` choose it (a unique one first,
then the only partial one, else the lowest oid of the plain ones; two partial ones are an error that asks for `using`) and say which, one `NOTICE` for each path (`the direct pairs of "grants" (dim_a_id,
tenant_id) are the rows of its unique btree "grants_direct" that satisfy (deleted_at IS NULL)`, then `the joined pairs` and `the via members`); `ALTER INDEX` is sticky (another `using` than the
recorded one is refused); `DROP INDEX` of a recorded btree is refused without `CASCADE`, and `REINDEX .. CONCURRENTLY` of it keeps the dependency. The dependencies are told apart by the relation
and its two columns, so one scope index depends on up to three btrees of the same relation or of several, and an index may hold a scope of two paths beside scopes of one. A scope of one path declared as before is unchanged.
The relations of a scope of two paths may hold a pair in several rows as well (a grant for each source of it, a row of the via relation for each source of a membership): the pairs are the DISTINCT union either way.

**The pair set.** The pairs of the scope are the DISTINCT `(member, key)` of the direct rows D and of the joined ones J, `J = { (m.member, g.key) : g in rel2, m in vrel, m.via = g.via }`, the rows of each
that satisfy the predicate of its btree. The SQL that says it:

    SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
    UNION
    SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id
     WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;

The tests compare every read with a join on this statement after every edit of the three tables.

**Once and per pair.** A member reached directly and through one or several groups is ONE pair: the scope segments store the distinct pairs and no fact row is stored twice for a key. A read with the
meaning of `IN` or `EXISTS` counts a fact once; a join or `GROUP BY` the key over the `UNION` counts it once for each pair, which is once for a key a member has by any number of paths
(`Scope Semantics: once`, `per pair`, as for one path). The dedupe across keys, the `work_mem` bound and the per-pair read of several keys that has a changed member are those of the section below.
`sift.scope_max_multiplicity` applies to the derived set: a member reached by two paths adds no entry, so the figure is the distinct entries for the rows of the table, as the `NOTICE` says it.

**The three journals.** The pairs of a member depend on (1) the rows of `rel` with that member, (2) its rows of `vrel`, and (3) the rows of `rel2` of every group it is in. Each relation needs a
journal index, a `sift_col` index with `track_changes = 'ctid'` whose first column is the journaled key and that holds the other column, plain or partial with a predicate that the predicate of the
recorded btree implies (the rule of the journal index above): J1 on `rel` is `(member, key)`, J2 on `rel2` is `(via, key)`, J3 on `vrel` is `(member, via)`. `CREATE INDEX` of a scope of two paths, as of one path,
is refused when any of the three is missing, or is a partial journal that a pair can be inserted outside of: the error names the first relation without one and the sift_col index it has that is not
a journal, and the hint has the `CREATE INDEX` of each missing journal. The scope index finds the journals when it is created or rebuilt and depends on them (a journal made later is found by
the next `REINDEX` of the scope index, which starts it before it reads and stamps the segments with it: the reads say `unfilled` until then). A journal rebuilt (`REINDEX` of it) is another lineage: the reads say
`other lineage` until the scope index is rebuilt, and the maintenance sees the segments as stale. Each journal has its own stamp.

**The members that changed.** A segment of a scope of two paths holds a stamp for each of the three journals. The members whose pairs changed since the stamps are the union of the members of J1, the members of J3
(the ones that left a group are there), and the members of every group in J2 as the recorded btree of `vrel` lists them for the reader's snapshot (the active rows, the predicate applied). The result is exact: the pairs of
a member depend on no other row (the three lists above), a row of one of them that changed names the member, or the group whose members are looked up now, in a journal; a member that left a group is in
J3. The read is the one of one path: the units of the keys less the changed members, plus the entity rows of those that have a pair with a key now (the patch), and the write buffer's
members the same way. The reader's cap bounds the changed members (a group with more members than the cap is `too many changed keys`: the entity rows are read; [the price](#what-the-planner-recognises)), and the refresh of the maintenance
counts them against `sift.col_connect_refresh_pct` as it counts those of one path, the members of a changed group included.

**Cost.** 100,000 facts, 2,000 entities, 200 groups, 20 accounts, 3,321 pairs; a read of one key (the account), instructions:u of the backend, cold and warm, in millions:

| | nothing changed | 1 group changed (10 members) | 100 groups changed (1,090 members, 135 patched) | the plain join |
|---|---|---|---|---|
| cold | 6.02 | 12.5 | 26.5 | 68.9 |
| warm | 1.34 | 7.8 | 21.8 | 63.6 |

The changes cost 6.5 M instructions for one group and 20.5 M for 100 groups over the first column, cold and warm alike; the join reads 63.6 M even warm.

**Column format minor 49.** A segment of an index that declares a scope of two paths holds three stamps and is written as column format minor 49 (docs/FORMATS.md); an index whose scopes all have one path
keeps writing minor 45 with one stamp for each scope. A library before 5.0, and a 5.0 build before minor 49, refuse the index (`Use a newer pg_sift`, `REINDEX`): `CREATE INDEX` says so in a `NOTICE`. A scope
declared with another number of paths than its segments were written with (`ALTER INDEX .. SET (scope = ..)` from one to two paths or back) is not read as stored (`unfilled`, the maintenance says `stale connection`)
until `REINDEX`.

**`sift.in_scope` and the planner.** The scan takes the conjunct from the join as an application writes it (the recognition below), or from the testing marker `sift.in_scope(index, key_name, fk, key or keys)` (`sift.in_scope_pairs` for the join meaning), as for one path. The planner reads the paths from `sift_scope_legs(index, scope, legs[2])` (2 legs for this declaration, the second one joined: `via_rel`, `via_join`, `via_member`, `via_btree`, `via_pred`) and recognises the `UNION` of the two
paths as the sub-select of `IN` or `EXISTS` and as the relation of a join (table below): each select reads the relation of its path with the predicate of its btree, both bind the same key by constants or parameters, and no other condition or column
is read. What else a statement says keeps the join as written, and `sift.explain_why` (arm `scope`) says which select breaks the proof.

**What stays impossible.** A relation that is a view, a CTE or a function (the three are heap tables with a btree on their two columns, unique or not); a condition on a third table (a group's own soft delete: the memberships of a deleted group
stay pairs; put the condition on the memberships or use the copy path of [INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view)); a predicate that depends on time (`now()` is refused in an index predicate: sweep it
into a column, see above); a partitioned table among the three; deny rows (a row that removes a pair another path gives); three or more hops, or more than one joined path.

### Key types

The scope key may be a `text`, `varchar` or `uuid` column of the pairs relation (a deterministic collation: equal values have equal bytes; `char(n)`, `citext` and nondeterministic collations are refused with their reason). The scope still cuts its
units by an int8: the code of the key in the shared value store of the key column ([COL-VSTORE](COL-VSTORE.md)), a store that is pinned for the scope, so that a code never moves. The index depends on the store (column format minor 50, which
older libraries refuse). A build or a rewrite of scope segments interns the keys it meets; a read only looks them up, so a standby can read. A key the store has not seen has no stored unit: the read gives it a code of its own for the length
of the statement, and its pairs come from the delta like those of any key. The planner takes the same forms as for an integer key (`=`, `IN`, `ANY`, a parameter, a table of keys); an equality under a nondeterministic collation keeps the join as written. A group by the key is read from the units: the groups are the codes, and the
output value comes back through the map of the statement; an `ORDER BY` or `LIMIT` in such a statement keeps the join, because the codes are not in the order of the values.

What it costs, against the integer key on the same data (300,000 facts, 3,000 members, 200 keys, about 2 pairs a member, one `sift_col` index with four columns, PostgreSQL 18, shared buffers 128 MB, one backend;
instructions are user-space, per statement, mean of 20 runs):

| | int | text | uuid |
|---|---|---|---|
| build (`CREATE INDEX`), s | 0.70 | 0.73 | 0.58 |
| WAL of the build, MB | 3.4 | 3.5 | 3.5 |
| index file, MB (the store of the key: 0 / 152 KB / 152 KB) | 3.47 | 3.59 | 3.59 |
| one key, thousand instructions (warm blocks hit / read; cold hit / read) | 1,046 (38 / 0; 403 / 23) | 1,008 (35 / 0; 455 / 27) | 988 (35 / 0; 533 / 27) |
| one key and a month, thousand instructions | 1,321 | 1,272 | 1,253 |
| every key grouped by the key, thousand instructions (warm 769 / 864 / 865 blocks hit) | 3,944 | 4,402 | 4,387 |
| five keys counted once, thousand instructions | 2,100 | 5,936 | 5,869 |
| one key with 375 changed pairs, thousand instructions (ms) | 87,720 (10.1) | 90,610 (8.7) | 90,679 (6.9) |

The mapping costs nothing a read can see for one key (the key is looked up once in the store, and the units answer). A list of keys counted once is read from the pairs relation by a scan of its btree, where an integer key reads
the leaves of the journal index of the relation: the strings of a leaf are not the codes of the units, so the scan translates every pair it reads (about 630 instructions a pair, 3.8 million for the 6,000 pairs here).

### Member types

The member may be a `text`, `varchar` or `uuid` column too, on both sides: the column of the indexed table and the member of the pairs relation are of one kind (text with varchar, uuid with uuid, an integer with an integer), under a deterministic collation, so that equal
values have equal bytes (`char(n)`, whose values are equal when they differ in blank padding, `citext`, a domain and a nondeterministic collation are refused at the declaration, with the column and the fix). A member is not coded in a store: the build, and a reader, give each value of the pairs and of a batch of
facts one code for the length of the statement (a map with no store), so the units are those of an integer member. The journal of the pairs relation, which a reader asks for the members that changed since the units were written, records for a member the 64-bit word of its bytes:
FNV-1a (offset basis 14695981039346656037, prime 1099511628211, over the bytes of the text, or the 16 bytes of the uuid), a fixed function with no seed, so the records a standby reads or a restore brings back are the ones written (the test `col_scope_textmember` pins
`''`, `'a'`, `'foobar'` and two uuids to their values, which an arm64 build must give too). The word is a filter, not an identity: a member whose word is another's is read as changed, which can only widen what is re-read. A `NULL` member of a fact has no pair, as for an integer; the empty string is a member like another.

A read of a string member is exact in every state the integer member is exact in, and reads the units under writes as the integer member does. The stored entries of the members whose journal word changed since the units were written are left out of the units (the source hashes the strings of the member leaf of each unit it reads, once for each distinct string of a dictionary, and tests the word), the pairs of those members are listed by their word and their rows are read by value from the entity segments (a string leaf has no point directory and no zone to prune by, so the leaf of every entity unit is read: a patch of a string member costs a scan of the entity rows, where an integer member of a segment with a point directory costs its lookups), the rows of the write buffer are copied by the pairs of their member, and a member of several keys counted once is dropped from the units of its other keys. Members are identified by their bytes: a read gives each value of the pairs a code for the length of the statement, and the rows of the segments and the buffers find their code by the string, so two members that share a journal word are both left out and both read again, which only widens the patch (`sift.test_member_hash_bits = 3` forces it: `col_scope_textmember_collide` runs every form and every state of `col_scope_textmember` under it, and the fuzz runs a seed set under it). The statement runs the join as written, and says why in `Scope Fallback`, in the cases an integer member does (`top-n, changed pairs`, more changed members than the read bound, a write buffer over `work_mem`).

What it costs, against the integer member on the same data (300,000 facts, 3,000 members, 200 keys, an integer key, about 2 pairs a member, one `sift_col` index with four columns, PostgreSQL 18, shared buffers 128 MB, one backend; user-space instructions per statement, mean of 20 runs):

| | int | text | uuid |
|---|---|---|---|
| build (`CREATE INDEX`), s | 0.62 | 0.71 | 0.58 |
| WAL of the build, MB | 3.4 | 3.7 | 3.3 |
| index file, MB | 3.47 | 3.84 | 3.52 |
| one key, thousand instructions (warm blocks hit / read; cold hit / read) | 1,105 (38 / 0; 403 / 23) | 1,384 (30 / 0; 415 / 24) | 938 (31 / 0; 488 / 24) |
| one key and a month, thousand instructions | 1,379 | 1,673 | 1,226 |
| every key grouped by the key, thousand instructions | 3,977 | 4,357 | 4,298 |
| five keys counted once, thousand instructions | 2,102 | 7,459 | 6,143 |
| one key with 375 changed pairs, thousand instructions (ms) | 87,608 (11.0) | 112,579 (10.7) | 112,093 (15.2) |
| five keys counted once with 375 changed pairs | 89,186 (8.5) | 114,360 (11.7) | 113,950 (12.5) |
| one key with 375 changed pairs and 3,000 facts in the write buffer | 88,251 (8.3) | 114,616 (12.6) | 113,961 (14.1) |
| the same, the join as written (`sift.agg_scope = off`) | 5,707 (1.2) | 9,851 (1.4) | 334,728 (33.1) |

The text member's index is 11% larger (each row of a scope unit holds its member string, where an integer member holds the integer). With changed pairs a string member costs 1.3 times the instructions of the integer member (the leave-out hashes the strings of every unit read, and the patch reads the member leaf of every entity unit), with none the read is the same. The units are priced against the join as written: at this size the planner's price of the patch is above the join's, and the figures of the scope rows are those of a forced read (`sift.agg_force`), as for an integer member. The join a node holds runs the lookup join of the sift aggregate node, which costs 311 ms for the five uuid keys here with the scope index or without it (`sift.agg_scope = off`), against 19 ms of the plain hash join that the planner chooses when `sift.agg_force` is not set: the figures of the join rows are those of the forced node.

### Two paths with text, varchar or uuid keys and members

A scope of two paths (a direct grant and a grant on a group) takes a `text`, `varchar` or `uuid` scope key and member too, with the same refusals as one path. The scope key of the direct pairs and of the grants of groups are of one type, and the member of the direct pairs and of the memberships
are of one type (a declaration where they differ is refused with both columns and their types); the group column that joins the second path to the memberships stays an `int2`, `int4` or `int8`. The key column is coded in the value store of the pairs relation (the units are cut by the code, as for one path), the
members of the three relations are coded by one map of the statement, and the three journals record a member as the journal of one path does (the word of its bytes) and a group by its integer. The paths of a read
are the integer scope's: the stored units less the members the journals say changed (a member that was reached by the other path is read again from the entity rows), the entity rows of those members read by value, the write buffer by the pairs of its member, and the dedupe of a member that two keys reach.
A colliding word leaves out and patches more members, never fewer or different ones: `col_scope_texttwohop_collide` and the fuzz leg `--two-paths --hash-bits 3` run it with the words cut to 3 bits. Tests: `col_scope_texttwohop` (a text key and member, a varchar fact column, a uuid key and member, an integer key with a text member, in 18 states of the
three relations and of the facts, then vacuum, rebuild, promotion and merge; the rows of 13 forms are those of the join as written and of the normal plan in every one), `col_scope_textkey_refusals`, and the fuzz (32 seeds, 16 with collisions, in `test/t/col_scope_textkey_fuzz.sh`).

What it costs, against the integer scope of the same data (300,000 facts, 3,000 members, 100 groups, 200 keys, 13,470 pairs, a direct grant to a member, a grant on a group and its memberships, three journal indexes, one `sift_col` index with four columns, PostgreSQL 18, shared buffers 128 MB, one backend; user-space instructions per statement, mean of 20 runs; `sift.scope_max_multiplicity = 10` for the build of all three):

| | int | text | uuid |
|---|---|---|---|
| build (`CREATE INDEX`), s | 1.99 | 1.82 | 2.10 |
| WAL of the build, MB | 6.0 | 6.6 | 5.6 |
| index file, MB | 6.18 | 6.66 | 5.65 |
| one key, thousand instructions (warm blocks hit / read; cold hit / read) | 1,730 (88 / 0; 590 / 37) | 1,445 (59 / 0; 653 / 45) | 1,466 (54 / 0; 732 / 37) |
| one key and a month, thousand instructions | 2,040 | 1,725 | 1,746 |
| every key grouped by the key, thousand instructions | 7,033 | 7,991 | 7,776 |
| five keys counted once, thousand instructions | 4,997 | 8,983 | 8,563 |
| one key with 375 changed pairs, thousand instructions (ms) | 90,299 (21.0) | 118,136 (17.3) | 118,313 (19.4) |
| five keys counted once with 375 changed pairs | 91,281 (19.8) | 116,194 (14.5) | 116,432 (18.2) |
| one key with a changed group grant and 40 changed memberships on top | 101,412 (14.3) | 131,747 (17.2) | 132,058 (20.1) |
| the same and 3,000 facts in the write buffer | 100,461 (15.8) | 128,786 (20.1) | 129,101 (20.5) |
| the same, the join as written (`sift.agg_scope = off`) | 7,818 (2.6) | 11,933 (2.0) | 324,161 (39.5) |

Unchanged, a string key or member reads as the integer one (the five keys are read from the pairs by a scan of their btree, which translates each pair, as for one path); with changed pairs it costs 1.3 times the instructions, the figure of one path. The figures of the reads with changed pairs are those of a forced read
(`sift.agg_force`): the join as written costs less at this size for any type and the planner prices the patch against it, as for an integer scope. The uuid join is the slow one of the aggregate node's lookup join (see [Member types](#member-types)).

### Limits

EXPERIMENTAL; a library before 5.0 refuses the index. What a scope does not do in 5.0:

- **Declaration.** The member key and the columns of the relations are `int2`, `int4` or `int8`, or `text`, `varchar` or `uuid` of a deterministic collation, in a scope of one path or of two (a coded key or member: see
  [Key types](#key-types), [Member types](#member-types) and [Two paths with text or uuid](#two-paths-with-text-varchar-or-uuid-keys-and-members); the group column of a scope of two paths stays an integer; `char(n)`, `citext`, a domain and a nondeterministic collation are refused with their reason, and the member of the pairs and the column of the index are of one kind). A relation is a heap table that is not partitioned, not a view and not the indexed
  table; a scope of one path has both columns NOT NULL. At most 4 scopes and 8 connections on an index, and a scan reads one scope. At most one joined path and no third hop; no deny rows; no condition on a third table and no predicate that depends on time.
- **Forms.** A statement that is not in the table of recognised forms keeps the join (`sift.explain_why`, arm `scope`): among them a join or a read for each pair over the pairs of a non-unique btree, a join to a table of keys that may hold a key twice,
  a key in an expression, a range of keys, `ROLLUP` / `CUBE` / grouping sets over the key. A top-N cannot take the patch or the delta of a changed pair (`top-n, changed pairs`): the join answers.
- **Text, varchar and uuid members.** A patch reads the member leaf of every entity unit (no point directory, no zone), and the leave-out of the units hashes the strings of every unit it reads: with changed pairs a read costs 1.3 times an integer member's (table in [Member types](#member-types)), and the planner may keep the join for it. A column of another type is refused. In a scope of two paths a read of one key with any changed pair leaves out and patches the changed members of every key, not of the key read (the records of a string key carry no tag), which an integer key does not.
- **No parallel workers.** A scope read is decided once, by the leader (test `col_scope_read` launches none); the join the node falls back to runs serial in the leader. The builds do run in parallel.
- **`CREATE INDEX CONCURRENTLY`** starts the journal at the end of its build, so the scope segments carry no stamp and the reads say `unfilled` until a `REINDEX` of the scope index or the rewrite the maintenance makes.
- **Snapshots.** A `SERIALIZABLE` or non-MVCC snapshot always takes the join (`snapshot`).
- **Generic plans.** A condition on the relation that reads a parameter is proved only by a custom plan: a generic plan keeps the join for a relation with a partial btree.
- **Two builds at once.** `REINDEX` of two indexes of one table at the same time is not covered by the build-lock event trigger.
- **Format.** The reserved by-reference unit kind (bits 8 to 15 of the scope stamp, directory flag 0x0010) is never written by 5.0: a library that finds one refuses the segment ("Use a newer pg_sift"), and the entity rows still answer.

## Without a scope index: replicate or reuse

Without a scope index (none declared, `sift.agg_scope = off`, or a declaration refused above the limit) the facts of one key are read from the plain `sift_col` index of the table by the member keys of the key, `fact.member IN (SELECT member FROM pairs WHERE key = k)`: **reuse**. The scan takes the keys the pair table lists as a conjunct on the member column of the index (a join key filter, [AGG](AGG.md#join-key-filters-and-shared-lookups): the member column is an integer or date leaf of the index, any `sift_col` index with one). A unit whose zone of the member column holds none of the keys is not read and the other rows are tested as a set, so reuse needs no declaration, no journal and no storage of its own, is exact at the time of the statement, has no limit on the keys of a member, and is chosen by the planner by cost against the plain join (`sift.agg_join_filter = off` restores the plain join). How many units the zones leave depends on the layout of the index: for facts ordered by time most units hold a member of a large key. **Replicate** is the scope index, whose units hold the rows of each key. Both give the same rows. With a scope index the statement is planned both ways and the estimated costs choose (what the planner recognises, above); where there is none the member keys are read. The table is the measured cost of each.

Measured on a table of `events` (member, time, kind, value, quantity) joined to a table of (member, tenant) pairs, 2.7 entries a row, four tenants of 0.1%, 3.3%, 6.6% and 33% of the events, the six requests of a dashboard
(sum by kind, sum by kind in one month, distinct members by month, top 20 members, a filter on the value, latest 20). Warm: instructions of the backend (geometric mean of the six) and shared buffers hit; cold: blocks read with
every relation of the statement evicted. Reuse is, in each cell, the better of two layouts of the entity index (units cut by time, or by the member and then time). Every pair of cells returned the same rows.

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

Reuse costs about 6 instructions a row of every unit that holds a member of the key, because the member column is decoded to find them (a profile of the 500,000-event run of the 6.6% tenant), and its member list about 1.9 thousand instructions a member; replicate reads the rows of the key
and answers sums and counts by kind and month from stored combinations. At a fixed share of the tenant both grow with the table: from 100,000 to 2,000,000 events (20 times the rows) reuse grew 4.1 times at the 0.1% tenant and 10.1 times at the 33% one, replicate 2.0 and 6.8 times. Reuse comes to about 25 instructions an event at the 33% tenant and about 5 at the 0.1% one (the 2,000,000-event run over its events). Nothing above 2,000,000 events was measured.

Reuse costs nothing to write. A table of the totals by member, kind and month kept by triggers
is not an alternative: its cells are 85% of the facts at 25 facts a member over 10 kinds and 12 months, and its writes cost +125% (insert), +73% (update) and +140% (delete) instructions.

## The scope copy: the alternative

The scope copy is the second way to make a request by a key above the fact table (a tenant, a group) cost what the key holds, and it is kept as the alternative to the index. **To choose: the scope index, unless the pairs are a view, depend on a condition on a third table or on time, or the request must be a plain query on a table of its own (`WHERE tenant_id = k`); then the copy.** Both are exact for the rows and neither needs a change to the query that the planner does not already
make; they differ in what they keep.

| | scope index (this page) | scope copy ([INDEX-RECIPES](INDEX-RECIPES.md#keys-above-the-fact-table)) |
|---|---|---|
| what is stored | the rows replicated by scope key inside the segments of the index of the table | a second table of the facts, once for each scope key of the member, with a `sift_col` index keyed by the scope key |
| size (100,000 events, 5,000 entities, tenants and groups: all tables and indexes of the fixture, 13.7 MB with the plain index) | 17.9 MB with one index of two scopes (+4.2 MB) | 35.0 MB with the two copies (+21.3 MB) |
| WAL of the builds (same fixture; 2.6 MB for the plain index) | 6.7 MB | 24.6 MB |
| writes to the facts | the statements cost what the index costs (the write buffer of the index; the pairs are read through their journal); the scope work is deferred to the promotion (+180 to +246 M instructions and +0.15 to +0.22 MB of WAL for each 10,000 facts at 1.05 entries a row, +396 M and +0.8 MB at 2.7: [What it costs](#what-it-costs)) | a statement trigger applies the change to the copy in the statement |
| writes to the pairs | the journal index of the pairs (a 32-byte record a pair) and a read that patches the changed members, a rewrite when many changed | a statement trigger refreshes the members' rows |
| a pair table that is a view | no: the pairs are a heap table with a btree on its two columns, unique or not | yes, `sift.scope_follow_pairs` keeps it as the view's answer |
| the planner uses it | the join forms of the statement, as the application writes them (the recognition above) | the copy is a table: `WHERE tenant_id = k` on it |
| state | EXPERIMENTAL | new in 5.0, EXPERIMENTAL too (`sift.scope_create`, `sift.scope_verify`, `sift.scope_drop`) |

The copy is for a request that names the key and wants the rows of the key by a plain query on a table. The index is for facts that must stay one table (no second copy to size, to vacuum and to
keep by triggers on the write path). The copy stores a fact once for each key of its member, so it grows with that figure (refused above 1.8
copies a fact by default, `max_copies` raises it); the index's growth is said by the `NOTICE` and bounded by `sift.scope_max_multiplicity`.

## Against tuned stock PostgreSQL

The comparisons above are against sift's own join. `bench/steps/scope_stock_arms.py` adds the arms a database administrator would build instead, on one generated fixture of a real shape: 2.4 million events of
2,648 members (median 388 events, largest 20,538; stored in time order), 32 tenants that reach members directly or through groups (3,612 distinct pairs, 1.45 tenants a member, soft-deleted rows included),
the eight measured tenants holding 49.5, 19.6, 17.4, 6.0, 6.3, 3.9, 1.1 and 0.1% of the events. Nine classes (sum by month, count by score, latest 20, top 20 members, distinct members by month, by member, a
selective filter, a date range, count by kind), eight tenants, every cell warm and cold, every result md5-equal to the plain statement (560 of 560 cells, and 140 more after the writes). All on one
PostgreSQL 18 server on tmpfs, one backend, `max_parallel_workers_per_gather = 0`; ms are the executor time of `EXPLAIN (ANALYZE, TIMING OFF)`. Arms:

- `stock_default`: a b-tree on the member, the semi join as written.
- `semijoin`: a covering `(member, time, id)` b-tree, a partial b-tree for the filter class, a per-member probe for latest 20. Nothing is maintained.
- `denorm_flat`, `denorm_part`: a copy of every event for each tenant that reaches it, as one table with a `(tenant, time)` covering b-tree or as a list partition by tenant, kept by triggers on the events and on the pairs
  (the pairs themselves come from a maintained pair table, since a pair edit must find the events to copy).
- `rollup`: rollups by member (month, score, channel and region, kind) kept by a statement trigger; the classes read them through the semi join.
- `ivm`: pg_ivm 1.17 incremental views of the join of events and the maintained pair table, one for each family of classes.
- `sift_reuse` and `sift_scope`: the entity index alone, and with the scope of two paths.

Rollups and incremental views answer only aggregates (latest 20 is not answerable by them). Warm milliseconds, geometric mean over the eight classes all arms answer (latest 20 left out):

| tenant (share) | stock_default | semijoin | denorm_flat | denorm_part | rollup | ivm | sift_reuse | sift_scope |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 (49%) | 307 | 130 | 79 | 109 | 27.0 | 4.50 | 11.9 | 5.69 |
| 2 (20%) | 220 | 54.8 | 30.1 | 60.3 | 14.3 | 2.96 | 10.0 | 2.54 |
| 4 (6%) | 145 | 18.2 | 7.72 | 17.4 | 13.8 | 1.63 | 6.71 | 1.13 |
| 5 (6%, 17 members) | 126 | 15.6 | 8.62 | 17.5 | 1.74 | 0.35 | 2.46 | 1.31 |
| 7 (1%) | 121 | 5.17 | 1.82 | 2.52 | 5.06 | 0.62 | 6.22 | 0.60 |
| 8 (0.1%) | 9.24 | 0.99 | 0.38 | 0.55 | 0.84 | 0.27 | 1.75 | 0.47 |

Where tuned stock PostgreSQL matches or beats the scope index, in the cells of the table (72 of them; absolute figures are in the bench output):

- **The tuned semi join is 16 to 23 times slower than the scope read (geometric mean) for tenants of 6% or more** (tenant 1: 291 ms against 3.2 for a sum by month), 8 to 12 times slower for the tenants of 1 to 6% with 17 to 44
  members, and within 0.5 ms of it for the tenant of 0.1%. It beats the scope read in 9 of 72 cells: the selective filter class for every tenant (1.5 against 3.1 ms at tenant 2, 1.9 against 19.8 ms at tenant 3, 0.4 against
  0.5 ms at tenant 8) and latest 20 of tenant 5 (0.67 against 0.79 ms).
- **A denormalised copy wins latest 20 everywhere (0.04 to 0.06 ms against 0.3 to 2.2 ms) and the selective filter (0.6 ms against 7.6 ms at tenant 1)**, and is as fast or faster than the scope read for the small tenants
  (20 of 72 cells). It is 14 times slower for aggregates of tenant 1 (78 ms against 5.7 geometric mean), since it still reads 1.2 million copied rows.
- **pg_ivm beats the scope read in 33 of the 64 cells it answers**, every aggregate whose group fits a view (at tenant 1: count by score 0.05 ms against 3.5, the filter 0.06 ms against 7.6, count by kind 0.6 ms
  against 1.3) and is about even for the rest (4.5 against 5.7 ms geometric mean at tenant 1). It answers no row retrieval. **Its writes pay for those reads:** 10,000 inserted events cost 11.8 billion instructions
  and 1.65 s (the scope index 0.15 billion, 0.33 with its promotion), and every pair edit 0.5 to 1.3 billion instructions and 67 to 280 ms (the scope index 0.0002 billion, under 1 ms). Member rollups, which a pair edit does not touch, are 4 to 12 times slower than the scope read for the tenants of 6% or more and 1.3 to 8 times for the small ones.
- **Tenant 1's top 20 members is read faster by the entity index alone (6.6 ms) than by the scope units (13.5 ms)**, and its filter and date-range classes were planned as the join (`join is cheaper for 1 key`): the
  scope read has no advantage for them at 49% of the events, as the break-even above says.
- Four parallel workers did not make the tuned semi join faster in this fixture (tenant 1: 100 ms serial, 116 ms with four).

What each arm costs. Added bytes and the cost of 10,000 inserted events (one statement), of granting a member to a tenant and revoking it (mean of 10 members of about 400 events; the largest member, 20,538 events, in
brackets):

| arm | build s | added MB | WAL of 10,000 inserts | instr of 10,000 inserts (M) | WAL of a pair grant | instr of a pair grant (M) |
|---|---:|---:|---:|---:|---:|---:|
| stock_default | 1.4 | 17 | 2.2 MB | 186 | 323 B | 0.14 |
| semijoin | 3.0 | 97 | 2.6 MB | 211 | 323 B | 0.13 |
| denorm_flat | 17.3 | 454 | 6.3 MB | 573 | 105 kB (7.3 MB) | 11.8 (603) |
| denorm_part | 11.1 | 425 | 6.0 MB | 510 | 101 kB (6.7 MB) | 9.7 (507) |
| rollup | 14.4 | 128 | 7.7 MB | 1,046 | 323 B | 0.13 |
| ivm | 13.5 | 55 | 3.9 MB | 11,765 | 77 kB (99 kB) | 510 (1,263) |
| sift_reuse | 7.6 | 33 | 1.7 MB + 0.10 MB promotion | 146 + 75 | 333 B | 0.14 |
| sift_scope | 19.4 | 65 | 1.7 MB + 0.32 MB promotion | 146 + 179 | 897 B (584 B) | 0.19 (0.15) |

The scope index adds 65 MB (the entity index alone 33 MB) and its writes cost a promotion and a few hundred bytes of WAL for a pair edit; the copies add 425 to 454 MB and write 6 to 7 MB of WAL for a pair grant of
the largest member, and the incremental views need 0.5 to 1.3 billion instructions for any pair edit and 11.8 billion for 10,000 inserts (pg_ivm 1.17 matches the changed groups of a view with a null-safe
`OR` that the unique index of the view cannot use beyond its first column). Updates and deletes of events, concurrent writers and a pair edit of a tenant of a read in flight were not measured for the stock arms; the copies and the
rollups need further triggers and indexes for them, so the figures above are the cheapest case for them.

## Checklist: what a scope needs, and what says so when it is missing
Each line is checked when the index is made (`CREATE INDEX`, `REINDEX`) or altered, in the style of the other messages of the extension: an `ERROR` with a hint that names the fix where the index cannot be made, a `NOTICE` where it is made and some reads run as written. A statement that holds a scope relation and is not read from the units says why in `sift.explain_why` (arm `scope`), for every form, including the ones no site of the planner names (no scan of the table is planned, the scan does not take the join, `sift.agg_scope` or `sift.enable_agg` off, the scope declared after the segments were written).

| What the scope needs | When it is not so | The fix |
|---|---|---|
| The indexed table is a heap table and the index a `sift_col` index; the member key is a column of the index of type `int2`, `int4` or `int8` | `ERROR`: the index must be a `sift_col` index on a heap table; the column is not an integer | index a heap table; map a text or uuid key through a dictionary table |
| The name of the derived key is not a column of the index; `cols(..)` names columns of the index, once each | `ERROR` naming the column | rename the key; name columns of the index |
| The relation of pairs is a plain permanent heap table, not the indexed table | `ERROR`: not a plain heap table (a view, a materialized view, a partitioned or foreign table), the table of the index, a temporary table (the index would be dropped with it when the session ends) | name a permanent table of pairs; a view is read by the statements, not declared |
| Its member and scope key columns are integers of one type on every path, `NOT NULL` on a scope of one path | `ERROR`: not an integer, nullable, the same column, types that differ | `ALTER TABLE .. SET NOT NULL`; change a column or the declaration |
| An immediate btree on exactly `(member, scope_key)`, in either order | `ERROR`: no btree, or only a deferrable or `INCLUDE` one; two partial ones of one kind (name the one that defines the pairs with `using`) | `CREATE UNIQUE INDEX ON pairs (member, scope_key)`; `using <index>` |
| The journal index: a `sift_col` index of the relation with `track_changes = 'ctid'`, the member first, holding the scope key, valid, without a `WHERE` (unless the btree of the pairs implies it) | `ERROR`: none, or the first `sift_col` index of the relation names what it lacks (no `track_changes = 'ctid'`, the member not first, no scope key, not valid, partial) | `CREATE INDEX ON pairs USING sift_col (member, scope_key) WITH (track_changes = 'ctid')` |
| A scope of two paths has the journals of its three relations (as a scope of one path has its journal) | `ERROR` naming the first relation without one and what its sift_col index lacks, the hint has the statement that makes each missing journal | the `CREATE INDEX` of the hint, for each |
| The journal stays a journal | `ALTER INDEX journal SET (track_changes = ..)` to another value, or `RESET`, is refused while a scope index depends on it | `ALTER INDEX scope_index RESET (scope)` first |
| At most `sift.scope_max_multiplicity` entries for each row | `ERROR` with the size and the growth, after the build | raise the setting, or leave the table without the scope |
| `ALTER INDEX .. SET (scope = ..)` or `RESET (scope)` on an index that has segments | `NOTICE`: the segments were written without the declaration, and the index is not scanned (every statement over the table runs on its other plans) until a `REINDEX` writes them | `REINDEX INDEX` |
| The scope of a partition is declared on the partition | `NOTICE`: a statement over the partitioned table reads no scope | read the partition, or leave the statement as it is |
| The relation has no row-level security that applies to the reader | `NOTICE`: a role the policies apply to reads the join as written | none needed, or a role the policies do not apply to |
| The index is made without `CONCURRENTLY` | a `CREATE INDEX CONCURRENTLY` starts the journal at the end of its build, so its segments carry no stamp and the reads say `unfilled` (`Scope Fallback`) | `REINDEX INDEX` without `CONCURRENTLY` |

`DROP INDEX` of the btree or the journal, and `DROP TABLE` of the relation, need `CASCADE` and drop the scope index with them (the dependencies are the record of which btree and journal the segments were written under).
