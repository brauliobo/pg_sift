# Scope index

A scope is a key a `sift_col` index cuts its rows by that the rows do not hold: they reach it through a table of `(member, scope key)` pairs. Events of entities, entities in tenants: the question
is "the events of tenant 7", and the tenant is in the pairs of the entity, not in the event. The scope index stores the rows replicated by scope key in segments of their own, cut by `(scope key,
time)`, so a read of one tenant reads the units of that tenant and no others, where the join reads the entity of each member. New in 5.0, column format minor 45 and opt-in. `sift_table` has no scope. Unless a figure says otherwise, the figures on this page are user instructions of one backend, warm, on generated lab fixtures of 100,000 to 2,000,000 rows (blocks hit and read where stated): they say how the arms compare on those shapes, not what a real database will measure.

This page is about the index, in this order: [the expected case](#the-expected-case), [the declaration](#the-declaration), [what a read does](#what-a-read-does) and [what the planner recognises](#what-the-planner-recognises), [EXPLAIN](#explain) and the refusals, [what it costs](#what-it-costs), [the edges](#edges), what happens [without a scope index](#without-a-scope-index-replicate-or-reuse), [a denormalised table](#a-denormalised-table-not-shipped), and [the prior art](#prior-art-what-exists-and-what-differs).

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

The recipe is one statement: the `scope` option on the index of the facts. `CREATE INDEX` makes the journal index of the table of pairs (it records the pairs that change) when the table has none, and says in a `NOTICE` which one it made, which btree of the pairs it recorded, how many entries the scope holds and what the index grows by. The queries stay as the application writes them: the planner reads the join forms against the units (below).

    CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
      WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

`key_name: fk -> [schema.]relation(member, scope_key)`: the name of the derived key, the column of the index that holds the member key (`int2`, `int4`, `int8`, `text`, `varchar` or `uuid`), and the relation of pairs with its
two columns: the member column, of the same kind as the member key of the index ([member types](#member-types)), and the scope key column, an integer, `text`, `varchar` or `uuid` ([key types](#key-types)). Up to 4 scopes, separated by `;`, each with its own name. The relation is a heap table (not a view, not the indexed table), with an immediate btree on exactly `(member,
scope_key)` in either order, unique (a primary key or a unique constraint) or not ([a pair may be several rows](#pairs-in-several-rows-a-multiset)), both columns NOT NULL; an unqualified name is in the schema of the indexed table. The index depends on that btree: `DROP INDEX`
of it, or `DROP TABLE` of the relation, needs `CASCADE`, which drops the index. The dependency is also the record of which btree defines the pairs: `CREATE INDEX`
chooses it (a unique one before a non-unique one, then the lowest oid of those that qualify) and says which in a NOTICE, every read uses that one, and a btree made or rebuilt (`REINDEX .. CONCURRENTLY`) afterwards changes nothing:
`REINDEX` of the scope index (also `REINDEX .. CONCURRENTLY`) and `ALTER INDEX` keep the recorded btree, so what the index means cannot change under a rebuild; a new `CREATE INDEX` chooses again. `ALTER INDEX .. SET (scope = ..)`, `SET (scope = 'off')` and `RESET (scope)` are refused once the index holds a segment (`0A000`, `cannot change the column definitions of index ".." while it holds data`): the scope is part of how the segments were written, so changing a scope or giving it up means building the index again beside the old one, `CREATE INDEX CONCURRENTLY` with the declaration or without it and then `DROP INDEX` the old one (no scan is lost while it is built). An index with no segment takes the change.
A declaration may state the most entries a row the scope accepts, for each scope: `tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(5)`; without it `CREATE INDEX` takes `sift.scope_max_multiplicity` (3) of its session and writes that figure into the option (`max_multiplicity(3)`), so the limit is a property of the index: `REINDEX`, the restore of a dump (`pg_dump` writes the option) and the rebuild after `pg_upgrade` check the recorded limit whatever their session has, and no build is paid for a refusal the index was not made under.
The time cut of the scope units is the first `date`, `timestamp` or `timestamptz` column of the index, else none.
A scope may also be the union of this relation and a relation joined through a second one ([two paths](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant)).

**Privileges.** The build reads every pair of the relations of the scope (the segments hold the rows by scope key, and the `NOTICE` and the size of the index say how many), so the role that runs `CREATE INDEX` or `REINDEX` needs what a
reader of the join needs: `SELECT` on the relation, or on its member and scope key columns and on the columns that the predicate of the btree of the pairs reads, and no row-level security of the relation that applies to it (the segments
would hold the rows the policies hide from it); otherwise the statement is refused with `permission denied`, naming the column and the `GRANT` (or the role without the policies). `ALTER INDEX` reads nothing and checks nothing: the `REINDEX` that writes
the segments does. A statement that reads the scope is checked as the join as written is, because PostgreSQL keeps the relation of the pairs in the range table of the statement: a role without `SELECT` on it, one whose `SELECT` was revoked after
the build and one that has it on a column the statement does not use but not on one it does gets `permission denied` from the scope read and from the join alike, and a role the policies of the relation apply to reads the join as written
(`sift.explain_why`, arm `scope`). The test is `col_scope_privileges`.

**The journal index.** The relation of pairs needs a `sift_col` index with `track_changes = 'ctid'` whose first column is the member, that holds the scope key and that has no `WHERE` (a partial index never hears of a pair inserted
outside its predicate, so it does not qualify). `CREATE INDEX` uses the first one the relation has (a scope of two paths needs one for each of its three relations). When there is none, a plain `CREATE INDEX` makes it, as the statement
you would write, named by the rule of PostgreSQL for an index (`tenant_scope_dim_a_id_tenant_id_sift_journal`), owned by the owner of the table of pairs (a role that does not own it is refused, with the statement for the owner in the hint), and says so in a `NOTICE`. The
index carries a comment that a dump keeps, and goes with the last scope index that depends on it: `DROP INDEX`, `DROP TABLE` or `ALTER TABLE .. DROP COLUMN` of that scope index drops it in the same transaction, unless
another scope index still uses it. A journal made by hand, or one whose comment you change, is yours and stays. The writers of the table of pairs wait until the statement ends, as for any `CREATE INDEX` on it (the statement takes
`SHARE ROW EXCLUSIVE` on the table first, so that two builds that make or use its journal do not deadlock); to name the journal, put it in a tablespace, build it without blocking the writers or keep it, make it first:

    CREATE INDEX CONCURRENTLY tenant_pairs_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

A `CREATE INDEX CONCURRENTLY` of the scope index holds nothing stronger than `ShareUpdateExclusiveLock` on its table while it reads the table and writes its segments, as PostgreSQL's own concurrent builds do, and the writers of the table go on. The first scope index
declared on a relation starts its journal before its build reads anything and stamps its segments with it, and the writers of the relation of pairs wait for the statement, as those of the table do for a `CREATE INDEX`. A concurrent build starts the journal at the end of its build instead, in a window of a few milliseconds in which it takes `ShareLock` on the relation of pairs (after waiting for the writers that hold it, without queuing the ones that come after) and fills the scope segments in its validation pass, under `ShareUpdateExclusiveLock` only, under the stamp
of the journal, as the rewrite of the maintenance does (the scope segments are written twice, and no writer waits for the second pass), so the reads are from the units as soon as the index is there. When the relation of pairs has no journal, the concurrent build makes it at that point as a journal that reads no row (its `NOTICE` says so), in the same few milliseconds: the writers of the relation of pairs wait only until that transaction ends, as for the triggers of any concurrent build. The journal is then ready for writes and not valid, and the validation pass fills it in the phases PostgreSQL gives a concurrent index: it waits for the writers that began before the journal was visible (nothing is held and nothing queues behind it), reads the relation of pairs under a snapshot taken after them and adds the pairs the journal does not have (`validate_index`), waits for the transactions whose snapshots are older, and makes the journal valid, holding `ShareUpdateExclusiveLock` only. Only the statement that made it reads it as a journal meanwhile; a statement that fails or is cancelled leaves an invalid index that `DROP INDEX` removes, with the scope index it was made for, and no other scope index takes it. Building the journal first with `CREATE INDEX CONCURRENTLY` as above names it and keeps it, and the scope index then finds it. A restore brings the journal back as the index it is, before the scope index that reads it.

**`ALTER INDEX` and `REINDEX` without `CONCURRENTLY`.** `ALTER INDEX .. SET (scope = ..)`, `SET (track_changes = ..)` and `RESET (scope)` are DDL that is not concurrent, like `CREATE INDEX` without `CONCURRENTLY` in PostgreSQL: they take `ShareLock` on the table (and the statement that makes a missing journal, `ShareRowExclusiveLock` on the relation of pairs) until the statement ends, the fill of the scope segments runs under it, and the writers of those relations wait for a time that grows with the size of the table. Use `CREATE INDEX CONCURRENTLY` or `REINDEX INDEX CONCURRENTLY` when they must not. A restore brings the journal back as the index it is, before the scope index that reads it.

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
(the recognition below).

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
key). The reader's refusals (`dedupe keys`, `too many changed keys`, the journal's words) are the same choice with the reader's reason: the join answers, which counts a row for each pair of a member in several keys
and the units cannot once the pairs of the member changed, so a read of several keys that counts a row for each pair never raises an error. A rescan decides again; `EXPLAIN` always says which way the node went (the scope lines, or
`Scope Fallback`). The join runs with the workers of the plan, as the scan it replaces would (see **Workers**). The generic plan has the same alternative: the key count it was priced for is the 10 PostgreSQL assumes for an array, the
execution has the real one. No plan holds the alternative where the join as written has no plan of this scan (the keys are the output of an InitPlan array, or the join is to a plan of its own: a table of keys read through a join that is
not a semi join of one key), where the scope index is on a view, or where the join as written has no plan with as few workers as the scan has: those keep the choice of the plan (the estimated costs).
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
Tests `col_scope_changed_pairs` (every state of the pairs, every read shape, every cell md5 equal to the join, the rows read bounded by the rows of the changed keys), `col_scope_groupkey`, `col_scope_delta`, `col_scope_patch`. A top-N takes the patch and the delta as units of its scan too
(`agg_scope_topn_delta`, see Latest-N and top-N and changed pairs).

**The read is priced.** A key that has a pair now has its rows read from the table segments, and an index whose units are cut by time holds some of them in every unit: the read then opens every unit of them beside the scope units.
Each read counts what its two routes would cost and takes the cheaper. The terms are the constants of `src/col/scope.h` (instructions of the backend; the test `col_scope_price_terms` prints each one): 450,000 before any unit (the journal's vouch, the directory, the statement),
36,700 a unit read row by row or 13,300 a unit its pre-aggregates answer, 18 an entry of the keys plus what the planner counts for the aggregation of a row (5 for an entry that the statement's own conditions leave out, by the planner's selectivity of them on the table, when no pair changed: the source tests it and nothing aggregates it), 20 more for each entry when pairs changed (the source leaves out their stored entries) and 15 more for each key whose rows a drop list thins.
A read of several keys counted once (`IN`, `EXISTS`) lists the pairs of the relation whole (39 a pair, whatever the keys) and those of its keys (286 each) to dedupe them (`Scope Dedupe Keys`) and cannot answer a unit from its pre-aggregates.
A change since the stamp costs the journal 150,000 before its first changed member, then for each changed member 3,300 at 5,500 pairs in the relation (grown by 0.45 log2 of the pairs over 5,500) for the probe of its pairs, or the relations' pairs read whole from the leaves of their journal indexes
(138 a pair; 300 a pair of a relation whose pairs only its btree lists), whichever is less. The patch is 11,000 a unit of the table segments that holds a row of the changed members, 23 a row looked at to find them where no point directory of the member leaf names them, and 550 a copy of the delta
(a changed member's row or a buffered row, copied for each pair, made a unit and read: 450 for one key, 700 to 900 for several keys, whose copies are sorted by key). The entity path (the join as written) is 25 a pair of the relation read through its journal index (300 through its btree), 36,700 a unit of the rows that may hold a member of the keys, 25 an entity row of them and 70 an entry the key filter keeps;
the read that costs more says `Scope Fallback: patch dearer than the entity path`. The prices are pure functions of the counts of a `SiftScopeCounts` (`sift_scope_patch_price`, `sift_scope_members_price`, `sift_scope_list_price`, `sift_scope_units_price`, `sift_scope_entity_price` and
`sift_scope_entity_cheaper`: the keys and `once`, the units and entries, the share of the units the pre-aggregates answer, the planner's instructions an entry, the changed members, the units and rows the patch reads, the delta rows,
the pairs and the units and rows of the listed keys), so the executor (from the zones of the units it read) and the planner (from its estimates) decide from the same inputs. Changed members make the units of the keys they are in lose their pre-aggregates: the share of units answered from them falls as `exp(-1.8 changed / units)`, the units of the keys of the read (the largest tenant of 148,000 entries has 36: 10 changed members take 39% of its units where 3% was counted by its keys).
A patch and a delta that cost less than pricing them (1,000,000: listing the keys and counting their rows) are read without it. The changed members one read lists are bounded by the table rows of the index at 21 over the probe of a changed member, and by what the join as written
saves: the node takes the changed pairs its join leaves room for (its price less the clean read, over the probe) as the cap of the reader, which refuses (`too many changed keys`) as soon as it has counted one more, without listing the
pairs, and the read runs the join. The changed members are counted in a hash table, not sorted; there is no setting for the cap. A key cut index reads only the units of the keys it patches.
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
The node chooses by itself: a read grouped by the key answers the units that hold no changed member from their pre-aggregates and reads the units of the changed members row by row (the terms above; the share of the entries in such units is 1 - exp(-1.8 changed / units), one unit a key in this table), and the join as written costs 110 instructions a row of the keys beyond the first more than the model's operators count (`AGG_ALT_JOIN`). Clean, with 20 and with 200 changed pairs of the same table: the node runs the scope read in 1.4 million instructions (the join 53.0), the scope read in 38.9 (the join 56.0), and the join in 58.2 (`Scope Fallback: join is cheaper for every key`; the scope read forced costs 90.8).

## EXPLAIN

A statement that cannot be read from the units is still answered, by the join as written, and says why. `EXPLAIN` shows `Scope Index` when the units were read and `Scope Fallback: <why>` when they were not; `SELECT * FROM sift.explain_why('<query>')` (arm `scope`) returns the reason without running the statement. `SET sift.agg_scope = off` gives the join as written for a comparison.

`Scope Index`, `Scope Keys` (`all` for every key), `Scope Group Key` (a group key of the scan is the derived key), `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows` (the rows of the entity segments read for the changed keys that have a pair now), `Scope Delta Rows` (the copies of the rows of the write buffers and of the patched rows the scan reads, one for each pair), `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled` (the index has no floor of the stamps yet: a concurrent build the maintenance has not filled), `write buffer` (the pairs of the buffered rows are more than work_mem holds),
`dedupe over work_mem`, `dedupe keys`, `patch dearer than the entity path`, `join is cheaper for N keys` (the node ran the join as written), `pairs are a multiset (non-unique btree): a join counts a fact once for each row` (a read for each pair of the pairs of a non-unique btree), and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the maintenance fills the
scope segments again, the reads ask for it), `trimmed`, `snapshot` (a serializable or non-MVCC snapshot), `snapshot older than the stamp`, `stamp aged`, `too many changed keys`. A scope read is decided once, by the
leader (the verdict, the changed members and the patch are made from the snapshot of the statement); the workers of a `Gather` read that decision, not their own (see **Workers**).

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
them again; the rule says `stale scope` when the journal says that many members changed (`sift.col_scope_refresh_pct`, percent of the pairs, 10; reason `stale scope` of `sift.col_maintenance_due`), when the reads that paid for the changed members come to half of what the rewrite costs (1,500 instructions an entity row, 680 a column of an entry and the WAL of the entries, 7 bytes an entry at 600 instructions a byte: 5.0 thousand million for 300,000 rows with 603,000 entries) or an entity segment lost its flag. A table nobody writes is never vacuumed, and the maintenance sees no statement. So a read that finds members changed since the stamps (it patches them, or the join costs less for them) adds what it paid for them, on top of the read of the filled index, to a sum in the metapage of the index, and tells the look of its backend, once for each `sift.maintenance_naptime`, which makes `sift.col_maintenance_due` say `stale scope` when the sum is half of the rewrite, and the status recommend a REINDEX (the server does not start the rewrite) (the tests `col_scope_stale_trigger` and `col_scope_stale_price`). A read that finds nothing changed adds nothing and asks for nothing, and an index nobody reads is not due for pairs nobody pays.

**What a read paid** is what it ran, at the planner's prices: the units it read row by row, those its pre-aggregates answered and the entries it decoded, as the executor counted them, less the units without the changed members, plus the blocks it took from the file instead of the buffers (20,000 instructions each, the weight of a block in the price of a unit of the rewrite, less one for each unit it read; a block found in the buffers is in the instructions already). A read that ran the join is priced as the planner prices the join for the keys it found, less what that price holds for routing every row of the table to a group when the statement groups nothing (5.7 instructions a row of 300,000), plus the rows of the first key, which it leaves out (28.8 instructions of the join's own row and what the units pay for the aggregation), and the routing of the rows beyond them to their groups (18 instructions for each group key). An entry costs what the executor spent on it: a row is routed to its group (18 instructions for each group key), each aggregate costs what the join's operators say beyond the join's own row (28.8), and the changed members add 32 for each entry for every aggregate (measured on 17 of them: 26 to 33; a count of 300,000 entries takes 29 instructions an entry, a sum 33, a sum and a count by 7 values 62). At 0.3 to 5% of the pairs of 300,000 events changed, the price of the extra is 0.92 to 1.22 of the measured, for the join and for the units alike (the measured extra is 2.3 to 21 million instructions over the filled read, which costs 1.1), and the rewrite is due after 100 to 640 such reads: one a minute for an afternoon, or a burst of a dashboard. The rewrite itself is priced with its WAL (7 bytes an entry at 600 instructions a byte), so the sum it is compared with counts the same things on both sides.

**Where the sum lives, and its limits.** The sum is a field of the metapage of each index, logged (WAL): one record of a few hundred bytes (56 bytes of WAL for a read, 720 for the first after a checkpoint) at the end of each statement that paid, up to half the price of the rewrite and not past it, so a rewrite that cannot run does not make every read write. A restart or a crash keeps it; every index has its own, however many there are (no table of the shared memory, no slot limit); a session that ends loses nothing; a standby replays it (`col_scope_paid_durable`, `col_scope_paid_standby`). It is zero again in the switch of the directory that moves the floor of the stamps (a rewrite), so what the reads paid to the segments it replaced is not owed to the new ones. The reads of the open write buffer and of the tombstone log keep their sums the same way (`col_wbuf_reads_durable`). What remains is limits of the platform and of the planner:

- **A standby's reads do not count.** A standby cannot write a page or WAL, and what its reads paid has no way to the primary: the only messages a standby sends are the positions of its replay and the horizon of its snapshots, and a connection of its own would need credentials and a primary that is up. A workload that reads only on a standby does not make the primary rewrite; the standby has the primary's sum and benefits from the rewrite when the primary does it (its own reads, a VACUUM of the entity table, `sift.col_maintain`), replays it and reads the filled units. Until then its reads cost the join as written, 1.7 to 19 times the filled read (measured), the price a read on a primary pays until the sum is half of the rewrite. A deployment that reads only on standbys runs `sift.col_maintain('<index>')` on the primary now and then.
- **The planner's own price of the extra is not the measured one.** It expects most units to keep their pre-aggregate and prices a count by kind of the largest tenant at 0.3% of the pairs changed at 0.43 of the measured (9.1 against 21.0 million instructions): the rows of a changed member lie in every unit of that tenant (73 units, none answered from its pre-aggregate). Its price of an entry is the join's operators, 66 instructions for any aggregate where the executor spends 29 to 90, and the two errors cancel on the stock cells the choice was fitted on, so the choice is not changed; the sum above does not use either. A group by of many values (30,000 groups) costs 416 to 1,450 instructions an entry (cache misses of the group table), which the rule's 18 for each group key and the planner's price both leave out.

The relation of pairs pays the journal index: a 32-byte record for each pair an UPDATE or DELETE ends and each pair inserted.

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

The scope index read fewer instructions than the join in all 243 cells of 100,000, 500,000 and 2,400,000 facts (9 tenants, 9 requests each), from 0.66 (the 36% tenant, count by kind) to 0.05 of the join, and the cold blocks of the two tenants shown fell by 1.7 to 10 times; the saving grows with the table (the 2.2% tenant: 10 M instructions at 100,000 facts, 170 M at 9,600,000), and no tenant share measured, up to 36%, turned it into a loss when the pairs were unchanged. A tenant of 36% saves 3 to 4 times the instructions and reads about as many cold blocks at 9,600,000 facts (1,775 against 1,639: the join reads the entity units that hold the member keys, the scope the units of the key). The requests that gain least are the count by kind, by member and the top 20 members (0.57 to 0.66 of the join at the 36% tenant); the date range and the latest 20 gain most (geometric means 0.09 and 0.13 at 2,400,000 facts). The statements of the 4,800,000 and 9,600,000 rows were read for four tenants.

**After writes, the same table (2,400,000 facts).** 600 facts in the write buffer and 20 pair edits (10 of the 36% tenant's direct pairs ended, 10 of its members granted to the 2.2% tenant, 5 members of a group of the 27% tenant left it and 5 joined it); the maintenance had not rewritten the segments (`sift.col_auto_merge = off` in this lab), the rows are the same in all cells. Instructions of the backend, scope against join:

| tenant | what changed | scope | join | the scope read |
|---|---|---:|---:|---|
| 36% | 10 of its pairs ended | 13 to 224 M | 13 to 223 M | the join, `join is cheaper for 1 key` (ratio 1.00 to 1.04) |
| 27% | 5 members of a group left, 5 joined | 51 to 166 M | 51 to 239 M | patch and delta: 0.43 to 0.73 for sums, distinct, nested array; 1.08 to 1.15 for top 20 members (a latest 20 read the join here, then: see Latest-N and top-N and changed pairs) |
| 2.2% | received 10 members of the 36% tenant | 32 to 150 M | 25 to 107 M | **the scope patch, dearer than the join**: 1.3 to 4.0 times, 97.5 against 27.9 M for the top 20 members, 105.0 against 26.4 M for the filter (the patch reads the 22,262 rows of the 10 members, 3,300 hits against 500) |

A tenant that received members is the case where the planner chose the scope read and the join was the cheaper one: the price of the patch is below its measured cost when the changed members hold a large part of the facts of the tenant (a reproduction is kept with the notes of the item). Until a rewrite (`REINDEX`, or the maintenance when the share of changed members or the cost of the reads calls for it) a tenant with changed pairs reads as the join at best and as 1.3 to 4 times it at worst, and a table where members of large tenants move often gets less of the saving.

**Building at the same time.** The build of a `sift_col` index that tracks changes installs the two triggers of its log on the table when it has read it, which takes `ShareRowExclusiveLock` (for a few milliseconds in a concurrent build, after waiting for the writers that hold the table) while `CREATE INDEX` holds `ShareLock`: two such builds on one table that ran together (`pg_restore -j`) each waited for the other. An event trigger before `CREATE INDEX` of a `sift_col` index
(`sift_col_build_lock`) takes the stronger lock first, so the second build waits at its start; the options a build decides are written under the tuple lock of the catalog row of its index. `REINDEX`
takes no stronger lock than its own `ShareLock` (PostgreSQL has no event trigger for it): it keeps the two triggers of its log when they are what the option made; one that has to make them (disabled, or none yet) asks for `ShareRowExclusiveLock` only when no other statement holds a lock that conflicts with it, since a library that is loaded on first use has no hook before the statement takes `ShareLock`, and two that wait for each other's `ShareLock` deadlock: the one that finds the table held leaves the triggers with a `NOTICE`, the maintenance worker makes missing triggers at the next read that checks the heap (`ALTER INDEX .. SET (track_changes = 'off')`, then the key, makes any other). The rules of a build read the other indexes of the table without a lock on them (a `REINDEX` of one holds `AccessExclusiveLock` on it), so two `REINDEX` of two indexes of one table run together. The event triggers cost a statement of DDL: 10,000 statements (`CREATE TABLE`, `CREATE INDEX`, `DROP INDEX`, `DROP TABLE` in a row, one backend, median of five runs) took 0.94 ms for each four with none, 1.20 with the build lock, 1.26 with the drop sweep and 1.45 with the four event triggers of the library: 0.13 ms for each statement, 1.3 s for the 10,000.

**Workers.** A scope read runs under the `Gather` of a plan that has workers, as an entity read does: the leader decides the read once (the keys, the changed members, the patch and the pairs of the write
buffers, from its snapshot), puts that decision and the directory it read in the shared memory of the statement, and each participant claims units from the one list (a worker opens the same directory and takes the keys
and the changed members from the leader; it asks the journal nothing). The join a scope read falls back to runs in the participants as the plan holds it. The rows of the write buffers and of the changed members
(the delta) are read by the leader after the units, so a read that is ordered or a top-N over a delta stays in one process. The planner prices a scope read from the statistics of its scope segments (their zones, the exact fraction of their pre-aggregates, the leaf statistics, folded and cached beside those of the entity segments), not from those of the entity units, and with the model of an entity read for the workers
(`sift.agg_par_min_rows`, the start cost of a worker: 3 ms and 0.5 ms for each); a read the pre-aggregates of the scope segments answer without reading their rows stays in one process (`Serial Because: the pre-aggregates of the index answer it without reading its rows`). At execution the leader launches none unless the workers
shorten the read by 10%: it prices the read in instructions (6 million a millisecond) and the read with workers as the launch, the start of the workers while the leader scans alone, the rest shared at the slower pace of new processes and the wait for their exit, so a tenant of a few units reads in one process (the tables below: no read of a fixture of 2.4 million events launched a worker, nor did any of the 19.6% tenant at 10 million). `EXPLAIN` says `Workers Planned` and `Workers Launched` as for an entity read; the answers are those of the read in one process
and of the join (tests `col_scope_workers`, `col_scope_par_edits`, and the oracle gate with every setting forced).

Measured on the fixtures of `bench/steps/scope_stock_arms.py` of 2.4 and 10 million events (a tenant of 49.5% and one of 19.6% of the events; the 7 query shapes of the scope arm, and a read with a group for each event) with 0, 2 and 4 workers (`max_parallel_workers_per_gather`), at the default prices and with every price at zero (`parallel_setup_cost`, `parallel_tuple_cost`, the scan sizes and `sift.agg_par_min_rows`), by `bench/steps/scope_parallel.py` (`measure` makes the raw file, `table` the tables; the build is the testing flavour, in which each participant logs its own instructions, buffers, memory and the sizes of the shared memory of the read). Each cell is the median of 7 rounds in which the worker counts and the two prices take turns in a rotating order, with its first and third quartile in brackets for the milliseconds. The CPU was quiet before each cell (pressure `some` avg10 of 0.0 to 9.9%, median 1.0, under the 10% the step waits for); the disks of the host were not always (I/O `some` avg10 of 0 to 72%, median 4.5, and a load average of 16 to 39 on 32 cores, other lanes at work; the data of this one is in memory), so two runs of one plan differ by 10 to 30% and a ratio inside that is a tie. Warm is the executor time of `EXPLAIN (ANALYZE)`; cold is the same after every table of the fixture left the shared buffers (`iolib.cool`; the data directory is in memory, so cold is the buffers, not the disk); instructions are the user instructions of the statement (`perf stat` of the backend and of the postmaster, which counts the workers it forks) and of each participant's scan; blocks are the statement's buffers, hit and read warm and read cold, in total and by participant; memory is the private heap and the nodes' memory of each participant, the shared memory the launch makes and the proportional set size (PSS) of all the processes. Below, x is the value with workers over the value of the read in one process (0.5 is half the time, 1.2 is 20% more); the 49.5% tenant at 10 million events at the default prices, the three shapes that launch:

| shape | workers | ms warm | ms cold | M instr of the statement (leader + workers) | x instr | x blocks warm | blocks read cold, x (leader / each worker) | private heap of a worker, shared memory | x memory (PSS) |
|---|---|---|---|---|---|---|---|---|---|
| by member | 0 | 33.4 (32.4-34.9) | 54 | 255 (252 + 0) | 1.00 | 1.00 (8,443) | 4,278 | - | 1.00 |
| | 2 | 19.3 (18.2-21.2), x 0.58 | 30, x 0.56 | 266 (121 + 136) | 1.05 | 1.08 | 4,339, x 1.01 (1,562 / 1,390) | 3.0 MB, 2.0 MB | 1.09 |
| | 4 | 17.5 (15.6-17.9), x 0.52 | 27, x 0.49 | 277 (73 + 190) | 1.09 | 1.15 | 4,359, x 1.02 (1,050 / 827) | 2.8 MB, 4.1 MB | 1.16 |
| top members | 0 | 26.8 (26.4-29.9) | 54 | 203 (201 + 0) | 1.00 | 1.00 (7,725) | 3,570 | - | 1.00 |
| | 2 | 16.3 (15.1-18.7), x 0.61 | 29, x 0.55 | 215 (97 + 110) | 1.06 | 1.08 | 3,627, x 1.02 (1,299 / 1,164) | 2.8 MB, 2.0 MB | 1.05 |
| | 4 | 15.1 (14.6-18.6), x 0.56 | 25, x 0.46 | 225 (73 + 138) | 1.11 | 1.16 | 3,654, x 1.02 (882 / 693) | 2.7 MB, 4.1 MB | 1.09 |
| filtered count | 0 | 23.7 (21.1-28.0) | 40 | 123 (121 + 0) | 1.00 | 1.00 (2,986) | 2,710 | - | 1.00 |
| | 2 | 17.5 (15.9-21.2), x 0.74 | 33, x 0.85 | 135 (76 + 51) | 1.10 | 1.18 | 2,760, x 1.02 (1,371 / 696) | 3.8 MB, 2.1 MB | 1.08 |
| | 4 | 20.4 (15.8-24.6), x 0.86 | 30, x 0.76 | 147 (70 + 63) | 1.19 | 1.37 | 2,764, x 1.02 (959 / 451) | 3.6 MB, 4.1 MB | 1.13 |

Every shape returned the same rows with 0, 2 and 4 workers. At the default prices nothing else launched a worker: no read of the 2.4 million events, none of the 19.6% tenant, nor the distinct count by month of the 49.5% tenant (its join is priced at 3,570 units, under the 4,000 that a priced parallel plan costs at least), the counts by score, the monthly sums and the latest rows (answered by the pre-aggregates or by the leader's delta), nor the read with a group for each event, whose 4.7 million groups are priced (the table each worker builds and the leader's merge of them) so that its plan has no workers even with `parallel_tuple_cost` at zero. Those reads take the same instructions and buffers with every worker count, and the same time within the noise (the pairs of reads in one process differ by 0.65x to 1.6x in the 2.4 million events' tables, whose reads last 1 to 10 ms).

What the workers cost where they launched, against what they save. Time: 1.7x and 1.9x less warm and 1.8x and 2.0x cold for the by-member read; 1.6x and 1.8x warm, 1.9x and 2.2x cold for the top members; 1.35x and 1.16x warm (the quartiles of the read in one process and of four workers overlap) and 1.2x and 1.3x cold for the filtered count. The first run of these tables, on the tip before the top-N and the patch-price landings, read the filtered count at 14.7 and 15.3 ms against 15.4 in one process (a tie, 1.0x), with the same instructions and buffers (raw file kept with the report): a read of 15 to 25 ms is where the gain of the workers is inside the noise of the host. Instructions: 4 to 19% more for the statement, of which 2 to 10% in the scans (the participants open the directory and the lookup tables again) and the rest the start of the processes (about 3.9 million instructions each). Buffers: 8 to 16% more hits warm for the by-member and top-members reads, 18% and 37% more for the filtered count (each participant pins the pages of the directory and of its units); blocks read from the disk in a cold read are 1 to 2% more in all, split among the participants, so the workers do not read more of the disk, they read it in parallel. Memory: a constant for each worker, 2.7 to 3.8 MB of private heap (1.3 to 1.9 MB the memory of the read: its nodes and a table of groups that is 0.01 to 0.21 MB here), a 1 MB queue in the shared memory, 2.0 and 4.1 MB of shared memory of the read with two and four workers, and 5 to 16% more PSS for the statement (the peak resident size of a worker, 31 to 41 MB, is the shared buffers it touched). The only read whose memory grows with the workers is the one with many groups, because each participant builds a table of the groups it meets: with the prices at zero 392 MB in the leader and 265 MB (two workers) or 230 MB (four) in each worker against 401 MB in one process, 1.85x and 2.49x the PSS of the statement, 1.31x and 1.43x its instructions (4,823 and 5,267 million against 3,675) and 1.62x and 1.45x its time warm (1.53x and 1.15x cold); the 19.6% tenant (tables of 206 MB, then 122 and 106 MB in each worker) took 1.15x and 1.44x the PSS and 0.81x and 1.09x the time (0.72x and 0.86x cold), within a read-to-read spread of 30% for reads of 0.7 to 1.5 s.

Where the workers cost more than they save, and whether the launch price counts it. The price (the 10% rule) is of time: the launch, the start of each worker, the slower pace of the new processes, the wait for their exit and the groups (`AGG_TABLE_GROUP`, `AGG_MERGE_GROUP`); it counts no blocks and no memory, and the numbers say why it does not need to. The warm blocks are buffer hits that the instructions already hold (the statement of the filtered count with four workers: 147 against 123 million instructions for 37% more hits), and the blocks read are 1 to 2% more in all; the memory of a worker is a few megabytes and a queue, 5 to 16% of the statement's, and the memory that would grow, the groups, is priced as time and keeps the read with a group for each event in one process. The launched read that comes closest to costing more than it saves is the filtered count of the 49.5% tenant: 10% and 19% more instructions and 18% and 37% more hits for 1.35x and 1.16x warm (a tie on the earlier tip) and 1.2x and 1.3x cold. I did not change the price for it: the read is above the 83 million instructions (two workers) and 75 (four) from which the price expects a gain of 10%, a threshold above 120 million instructions would keep it in one process and lose the cold gain, and the instruction estimate of the scope reads, 20 to 40% off for these shapes on the earlier tip (151 million priced against 118 measured for the filtered count, 179 against 252 for the by-member read), is the open item of the scope price, whose fixes of the constants changed the plans of other shapes for the worse. With the prices at zero nearly every shape launches (not the counts by score nor, at 10 million events, the monthly sums), and the ones that gain nothing lose time and cost more in every measure: the latest rows take 4.7 to 12.5 times the time (5 to 14 ms against 0.5 to 1.7) with 1.9 to 5.7 times the instructions and 3 to 17 times the hits, the monthly sums of the 2.4 million events 4 to 9.6 times with 2.3 to 4.0 times the instructions, the filtered count of the 19.6% tenant at 10 million events 1.7x and 1.4x the time with 1.4x and 1.8x the instructions, and the by-member, top-members, distinct and filtered reads of the 2.4 million events 0.91x once (inside the noise) and 1.1 to 5.6 times the time in the other 15 cells, with 1.15 to 3.4 times the instructions; the price keeps every one of them in one process. The distinct count of the 49.5% tenant at 10 million events is the one that gains with the prices at zero (29.4 and 25.9 ms, 1.5x and 1.7x less, cold 1.7x and 2.2x, with 7% and 12% more instructions and 29% and 57% more hits; the 19.6% tenant 1.2x and 1.2x): its plan is the join, which the default prices keep in one process (3,570 units) and the launch price of the scope read does not decide.

Where the time goes. A read of 40 to 50 ms in one process is its scan; the participants start 3 ms after the leader launches them (the leader scans meanwhile), and the leader waits 1.3 to 3 ms for the workers to exit after the last item. Phases of the by-member read of the 49.5% tenant at 10 million events with two
workers, one run on a quiet host (ms): open of the directory 1.3, launch (the shared memory and the queues) 1.4, scan 17.4 in the leader and 13.3 in each worker (user CPU 14.2, 11.4 and 11.9), wait for the exit of the workers 2.6, 22.9 in all against 46 in one process. With four workers 21.5 ms (scan 16.5 in the leader, 11 to 12.7 in each worker, the wait 3.0).
The 2.4x of the median is the 4 workers' limit here, not a ceiling of the units: the fixed part (open, launch, exit: 5 to 6 ms) does not shrink, and the participants run the same instructions at an IPC of 1.5 where one process ran at 1.8 (cold caches and TLBs of each new process: 142 M cycles alone, 178 with two workers and 190 to 238 with four), so the scan
of 40 ms of one process takes 40 x 1.3 / 5 = 10.4 ms of the five participants. Fitting the by-member runs (36 ms alone, 20.2 with two workers, 16.3 with four: a fixed 10 ms and 29 ms that share) the least serial time that makes 1.6x with two workers is about 30 ms, and 2.5x with four about 45 ms, so a read below 30 ms gains less than that (27.5 to 16.9 ms for the top members on a quiet host) and a read below about 15 ms gains nothing: the leader's price of a launch
(1.4 ms to launch, 3 ms and 0.5 ms for each worker to start, 3 ms to wait for the exit, the shared part at 1.25 times the time, and 10% to spare) puts the point where it launches at 83 million instructions (about 14 ms of one process) with two workers and 75 with four (test `agg_par_pay`); below it the launched reads of this fixture tied or lost (the 19.6% tenant's by member and top members, forced: 0.88 to 1.01 of the time of one process). The monthly sums, counts by score
and the latest rows stay in one process: the pre-aggregates answer the first two, and the delta of the latest rows is the leader's (with the prices at zero the latest rows launch workers and read in 6 to 12 ms against 1 to 2 ms alone).

The default plan of the distinct count by month of the 49.5% tenant at 10 million events is the join (`Scope Fallback: join is cheaper for 1 key`), and the planner prices it at 3,570 units, under the 4,000 that a priced parallel plan costs at least, for a read that takes 45 to 83 ms: the workers it would launch read it in 25 ms
(on a quiet host, with the workers forced), so that read stays in one process by price, not by the workers.

## Key groups: one summary for the run of units of a key

A key of many units (tenant 1 of the fixture below: 130 to 2,200 rows of each of its 1,149 members, 2,000 rows a unit) used to be summarised by groups of 16 units at absolute positions, so the units of a key that did not
start at a multiple of 16 were read one by one, and an aggregate by member read every unit: a unit holds about two rows of each member of its key, which is too few for a table of the members (a histogram needs 8 rows a group to
pay, `SIFT_PAGG_SCOPE_DIM_ROWS`). A scope segment of column format minor 51 writes its superunit block as **key groups**: a group is a run of two units or more that hold one scope key and no other (at most 256 units and 131,072 rows), and a run
longer than the fan of the index is followed by the groups of a fan of units inside it, for a range of days that cuts the run. The cube of a group is the merge of the cubes of its units (as before) and, apart, the cube of the dims the units
lack, built from the rows of the run: the member (and any other column of the index's cube that a unit has no table of) with the integer measures, within a tenth of the bytes of the units, the smallest tables first, with no minimum and no
maximum for a dim of more than 16 groups, and not for dates (the calendar cells of the units answer a range of days). A read takes the group where it takes a fan of units, and takes the merged part before the part of the rows: an aggregate that
needs a dim the units have reads the same bytes as before. When the index declares `rollup_columns` the group holds the rollup of the rows of its run too (the combinations of those columns with
their measures: 240 cells of (channel, region) for the 297,000 rows of tenant 1; a unit of 2,000 rows keeps at most a tenth of its rows in cells, so the units hold none): a filter or a count on those columns reads one
rollup for the run, and a group whose rollup does not fit (over 4,096 cells or the byte budget) holds none. The scan items end at the end of the group they cut (the next item starts there), so a group is never split between two participants. `sift.col_verify(index, true)` recomputes the page
masks, the sums of every leaf, the merged cube, the dims of the rows and the rollup of the rows of every group from the units.

Tenant 1 of the 600,000-event fixture (events with a tenant a member of 2,648 members, one backend, `perf stat` instructions of the backend, warm blocks hit and cold blocks read after eviction, md5 of the rows equal to the plain statement
in every cell of the nine classes at tenants 1, 4 and 8):

| class (tenant 1) | instructions | blocks hit | blocks read cold |
|---|---:|---:|---:|
| top 20 members | 14.18 M to 4.12 M | 466 to 105 | 277 to 90 |
| count by member | 17.57 M to 5.27 M | 513 to 111 | 311 to 93 |
| count by score | 1.71 M to 1.59 M | 76 to 50 | 58 to 32 |
| count by kind | 2.02 M to 1.94 M | 80 to 64 | 62 to 52 |
| tenant 4: top 20 members / count by member | 3.05 M to 2.02 M / 3.34 M to 2.00 M | 83 to 31 / 89 to 31 | 60 to 20 / 75 to 20 |

The cost: the scope segment of that fixture is 8.63 MB without and 9.30 MB with the groups (+7.7%; its pre-aggregates 2.57 to 3.23 MB: the dims of the rows are bounded by a tenth of the bytes of the units, the merged cubes are those of the groups of 16 that they replace), the index file 16.28 to 16.91 MB (+3.9%), the build 3.6 and 3.3 s (one run each, the difference is noise) and its WAL 50.7 to 52.0 MB (+2.5%); a promotion of 30,000 events writes the same 0.57 MB of WAL (the new segment is small). A segment with no run of units of one key keeps its minor. What the groups do not answer: a count by member over a range of days (a group cut by a bound is read unit by unit), the filter on two columns (`channel = 9 AND region = 'DE'`) unless the index declares them in `rollup_columns` (below; the
summary has a histogram of each, not their combinations, so the units are read one by one; the scan checks the zones of the units of a group against the comparisons that its histograms do not answer before it reads the summary of the group,
so a group that cannot answer costs no block: at tenant 4 the filter reads 46 blocks hit and 45 cold with the groups and without them).

**The rollup of the rows of a group.** Declared as `rollup_columns = 'channel_id, region: amount'` (no option of its own: the groups reuse the declaration of the entity index; a scope index writes none by default,
and `'auto'` asks for the pair of a plain index, see Edges), the filter class goes from the units of the key to one rollup per group. The 2.4-million-event comparison (`bench scope-stock --size standard`, arms
`sift_scope` and `sift_scope_rollup`, the filter on two columns; instructions of the backend, blocks hit warm and read cold, md5 of the rows equal in every cell):

| tenant (share of the events) | `sift_scope` | `sift_scope_rollup` |
|---|---:|---:|
| 1 (49%) | 35.84 M, 790 hit, 787 read | 2.65 M, 76 hit, 56 read |
| 2 (20%) | 8.29 M, 354 hit, 352 read | 2.37 M, 66 hit, 51 read |
| 3 (17%) | 7.53 M, 326 hit, 326 read | 2.32 M, 63 hit, 50 read |
| 4 (6%) | 3.82 M, 147 hit, 135 read | 2.29 M, 67 hit, 54 read |
| 5 (6%) | 3.82 M, 136 hit, 122 read | 2.24 M, 64 hit, 51 read |
| 6 (4%) | 3.26 M, 114 hit, 102 read | 2.33 M, 71 hit, 61 read |
| 7 (1%) | 2.36 M, 73 hit, 61 read | 2.43 M, 93 hit, 81 read |
| 8 (0.1%) | 2.04 M, 63 hit, 51 read | 2.07 M, 69 hit, 57 read |

At tenant 1 the plan without the groups is the join (35.8 M: the units cost 17.5 M forced, and the price of a unit read did not know of the groups); the group term of the price (`SiftScopeCounts.group_frac`, `group_units`: the units that groups
with a rollup cover are priced by the groups) makes the units the cheaper read. The cost: the index of the arm is 70 MB against 68 MB (+3%: the rollups of the groups), the build 8.6 s against 7.5 s, the other eight classes at every tenant within 1.7% of the
arm without it, writes unchanged. The two smallest tenants read 20 and 6 more blocks (+3% and +1.5% instructions): declaring a rollup the units cannot hold costs each unit a look at its rollup (14 blocks at tenant 7 with the groups off), and a key of
a few units reads its group's block besides. Measured and not taken: a rollup only in groups of 4 or more units (the tenants 7 and 8 read the same, the gain of tenants 2 to 6 is kept) or of 8 or more (the gain is lost: the scope segments of a serial build
hold 8 to 55 units of a key), and in groups of a fan of 16 (tenants 2 to 6 lose it, tenant 1 reads 3.0 M).

**The id sets by month of a group.** `count(DISTINCT member)` by `date_trunc('month', date)` of a tenant has no cell to read: a unit holds a few rows of each member and month, so the units hold no table of the
members and the scan read every row of the tenant. A key group holds one run of the key, hundreds of rows of each member, so it keeps what answers it exactly: one bitmap of the values of each 32-bit integer dim of the
cube (`int2`, `int4`; seven leaves at most, in the order of the schema) for each calendar month of the rows of the group, and, to say which months those are and to test them against the comparisons of the statement,
the cells of the days of the first calendar dim of the cube (counts only, in days: a key through the zone of the session is exact at the day of a date only, so cells of months would not answer `date_trunc('month', date)`
in a zone that skips a day; a group of more than 8,192 days holds none). Both are in a section after the cubes of the block, so a read of the dims or of the rollup of a group never crosses them (the word of the group says
which leaves it holds: a statement that wants another reads nothing). The consumer is the one of the ungrouped `count(DISTINCT member)` (the same merge of bitmaps), fed by the bitmap of a month of the group; the units
of a tenant that no group holds are read as before, and the answer is exact: the rows are those of the plain statement in all 72 cells of the nine classes at the eight tenants of the 2.4-million-event fixture and in the
oracle gate (seeds 1 to 6, with a generated date column added to its statements). What is asked of the statement: a scope of one key, `count(DISTINCT leaf)` of a 32-bit integer dim and no other aggregate than an unfiltered
`count(*)`, every test on the date a bound of a month (a group some days of a month of which a test drops is read by units), and the days of a month giving one key through the zone of the session; a group whose bitmaps
would take more than a byte for each row of its run (many far apart values) or with a negative value holds none for that leaf, and a group with a row whose date is NULL is read by units; text and uuid members are not served (their values are codes of a store);
a key whose pairs changed reads its units, the patch and the delta as before (a group is not taken while the pairs of its key are patched). They are derived data of the units like the rest of the block: promotion, merge, rewrite
and VACUUM rebuild them with the group, `sift.col_verify(index, true)` recomputes the sets and the cells of the days of every group from the rows of its units and compares them byte by byte, and the damage test
(`sift._test_col_super_damage`) refuses every flipped bit of the block (the padding included). The price is the one of the groups with the rollup: the fraction of the units that groups with sets cover and the units of a
group (`SiftSrcStats.sets_frac`, `sets_units`, from the same census) replace the preagg term of the units for this shape and for no other.

2.4-million-event fixture (`bench scope-stock` data, one backend, instructions of the backend warm, blocks hit warm, blocks read cold after eviction; md5 of the rows equal to the plain statement in every cell; the index is
rebuilt by both libraries with `REINDEX`, so the layouts are the same):

| tenant (share of the events) | before | with the sets |
|---|---:|---:|
| 1 (49%) | 83.21 M, 602 hit, 757 read | 8.56 M, 85 hit, 81 read |
| 2 (20%) | 33.39 M, 265 hit, 335 read | 8.48 M, 83 hit, 80 read |
| 3 (17%) | 30.02 M, 239 hit, 285 read | 8.46 M, 84 hit, 78 read |
| 4 (6%) | 13.76 M, 107 hit, 149 read | 9.23 M, 78 hit, 68 read |
| 5 (6%) | 13.94 M, 98 hit, 152 read | 8.40 M, 80 hit, 71 read |
| 6 (4%) | 10.76 M, 82 hit, 83 read | 9.03 M, 79 hit, 71 read |
| 7 (1%) | 6.94 M, 55 hit, 53 read | 6.95 M, 56 hit, 54 read |
| 8 (0.1%) | 4.43 M, 45 hit, 34 read | 4.44 M, 46 hit, 35 read |

The gain is 9.7 times the instructions and 7 times the blocks hit at the largest tenant, and it falls where the tenant has no run of units of its own: the tenants of 4 to 6% keep 5 M of the 8 to 9 M in one lone unit that no
group holds, and the two smallest tenants have none. The other 64 cells of the eight tenants read no more blocks hit or read (the sets and the cells of days are a section of their own, and a scan that has no sets to read does not
look at the groups of a lone unit) and at most 1.9% more instructions (the by-member class at tenant 1, 8.57 M to 8.73 M). The cost: the index of that fixture is 67.35 MB without and 68.26 MB with the sets (+0.91 MB, +1.35%: the groups of up to 131,072 rows keep about 6 to 10 kB
each), the scope segments of the 240,000-event fixture 7.27 MB in both (+0.1%). A scan tries the groups for the sets only when its statement is of that shape, and not for a lone unit: the first version read the header of the block of every segment of a small tenant (the tenant of
0.1% read 5 blocks more, the 1% one 5 more, the quick fixture 6 more) and the cold read of the other classes went up by the reads that the order of those headers changed.

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
and the refusal says the entries, the rows, and the size and growth the index would have had; `sift.scope_max_multiplicity` is the default for a new declaration, and the declaration's `max_multiplicity(n)` the override for the members that are in many keys and whose reads are worth it (recorded in the index: a later `REINDEX` keeps it).
A table of 2,048 blocks (16 MB) or more is estimated before its build, so a scope that is clearly over the limit does not spend the build on its segments: 32 runs of 8 consecutive blocks at fixed random
places give the entries of their rows as a ratio of their rows, and the build is refused ("about N entries for about N rows (N per row, from a sample)", without the growth) only when the ratio less 3.5 standard
errors of that ratio of 32 runs (about 99.9% one-sided) is over the limit. Every other scope is built and the exact figure after the build decides, so a scope near the limit is never refused by a sample.
Above the limit the join answers (the entity path), and a count or a sum by key is cheaper from a table of them (docs/INDEX-RECIPES.md). A table of copies kept by triggers flips at 1.84 copies a fact because it carries
the heap rows and their trigger as well; the units live in the index and flip at 3. The reads of the units are not cheaper at a higher multiplicity. A larger table is expected to move the break-even
up (the join is linear in the table; nothing above 2,000,000 events was measured), which this limit does not price: raise it for such a table, from the figures of the NOTICE.

## Verifying a scope index

    SELECT * FROM sift.col_scope_verify('events_col');                  -- 10 random keys; no row: consistent
    SELECT * FROM sift.col_scope_verify('events_col', 50);              -- 50 keys
    SELECT * FROM sift.col_scope_verify('events_col', keys => ARRAY['7']); -- the key named

`sift.col_scope_verify(index, sample, keys)` runs, for each key and each scope of the index, the statement the planner recognises (`f.fk IN (SELECT member FROM rel WHERE key = k AND <predicate>)`, or the union of the two paths) twice in the
snapshot of the caller: through the scope units (`sift.enable_agg` on, `sift.agg_force`) and through the plain plan (`sift.enable_agg` off, the executor of PostgreSQL alone). Each side answers the number of rows and a signature of the first
column of the index for each member, and the function reports the keys whose two answers differ: `problem = 'differs'`, `scope_rows` and `join_rows` (the rows of each side) and `detail` (how many members differ and the first). The keys are
drawn at random among those that have a pair now (`sample`, default 10), or are the ones named by `keys` (text, cast to the type of the key: integer, text, varchar or uuid), which may also name a key with no pair: the check that the units hold nothing for a key whose pairs all went. A key whose answer was not
read from the stored units (an index that has no floor of the stamps yet says `unfilled`, a `Scope Fallback` says its reason) has `problem = 'unverified'` and the reason in `detail`: nothing was compared for it. No row means every
sampled key was read from the units and equals the join. It reads each sampled key twice and a third time to learn whether the units were used, and the plain side is the join of the key's members, so run it on a few keys of a large table;
it changes no setting of the session and no data, and runs with the privileges of the caller. In `REPEATABLE READ` it checks the snapshot of the transaction, on a standby the replayed state (a read-only session). `sift.col_verify(index, true)` checks the structure of the segments against the heap.

**Replication and upgrade.** A physical standby replays the index and reads it (`test/t/col_scope_replica.sh`). The facts and the pairs are heap tables, so a logical subscription carries them: a subscriber that has its own scope index (the facts, the pairs and their journals made there before the subscription copies the rows) keeps it as the apply worker writes the rows, since the journal of the pairs is kept by row triggers that fire for the apply worker, and its reads are the publisher's join; the index itself is not replicated. The oracle gate (`test/t/col_scope_oracle.sh`) compares every read of the primary, a standby, such a subscriber and a restored dump with a node that never loads pg_sift. `pg_upgrade` carries the index with its options and its dependencies on the btrees; the journals and then the scope index are rebuilt after it (`test/t/pg_upgrade.sh`).

## Settings

| Setting | Default | Gain | Impact | Why the default | Off |
|---|---|---|---|---|---|
| `sift.agg_scope` | on | a read of one key from the units: 1.0 to 1.1 million instructions where the entity rows of the members cost 1.9 to 2.0 (the fixture of "What it costs", sum and count by kind of one tenant, prepared statement, warm, one backend); two keys 1.55 million against 2.09 | nothing stored | a scope exists only when the user declares it | `SET sift.agg_scope = off`: the statement is the join as written; no `REINDEX` |
| `sift.scope_max_multiplicity` | 3 | the default of a new declaration (recorded in its option as `max_multiplicity(n)`): `CREATE INDEX` refuses a scope whose members are in more keys than this on average, with the size and growth it would have had; the join answers the reads (see Break-even) | none | the units break even with the join at 2.5 (500,000 events) to 3.0 (2,000,000) entries a row | `SET sift.scope_max_multiplicity = 1000000` (or `max_multiplicity(1000000)` in the declaration) accepts any figure |

## Edges

The expected case needs nothing of what follows. The edges, each with what happens there:

- [where the gain is small](#where-the-gain-is-small): the requests that gain least, latest-N and top-N while pairs changed, small tables, members in many keys;
- the shapes of pair tables the declaration also takes: [soft deleted pairs](#pairs-that-are-soft-deleted-a-partial-unique-btree), [a pair in several rows](#pairs-in-several-rows-a-multiset), [two paths](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant) (two hops: a grant on a member or on a group);
- key types, partitioned tables and the other [limits](#limits).

### Where the gain is small

Tenant size is not a limit: in the table of [the expected case](#the-expected-case) and of [the member keys](#without-a-scope-index-replicate-or-reuse) the scope index read fewer instructions at every tenant share measured, 0.1% to 33% of the events. What differs is the request. At 2,000,000 events and the 33% tenant the member keys cost 13.3 times the instructions of the scope index for sums by kind, 10.2 for the same in one month, 6.9 for the latest 20 and 2.5 for a filter on the value; the gain is smallest for the top 20 members (1.0, a tie) and for distinct members by month (1.4), the requests that read rows of the largest keys. At 0.1% the same two gain 6.6 and 4.6 times.

- **The filter on two columns, and a rollup that no statistic chooses.** The groups answer the filter only from the rollup of the rows the index declares (`rollup_columns = 'channel_id, region: amount'`, [key groups](#key-groups-one-summary-for-the-run-of-units-of-a-key)); without it the filter reads the units of the key (17.8 M instructions, 821 blocks hit and 987 read at the largest tenant of 2.4 million events on the tip of this round; the join of the table above is no longer the plan there). `rollup_columns = 'auto'` chooses like the plain index (the derived key of the scope, which no column of the table holds, is no candidate of the choice any more: it made the list fail, so `auto` and `preagg = 'auto'` chose nothing for a scope index) and writes `auto: a, b: m` with the sums that are worth keeping, the integer columns that are not keys (`amount`, not `id` or `dim_a_id`: a sum of ids says nothing and a measure costs a word in every cell). The pair is the one with the fewest combinations that fits (`kind, score`, 60 of the 240 of `channel_id, region`), and the empty option, the default, writes none for a scope index. Measured on the comparison (2.4 million events, tenants 1, 4, 5 and 8, the nine classes, md5 equal in all 36 cells of each arm), against the arm without a rollup:

| arm | the filter on two columns | count by score | other classes | index of the arm | promotion of 10,000 events |
|---|---|---|---|---|---:|
| `rollup_columns = 'off'`, the default | 17.8 M, 821 hit, 987 read (tenant 1); 3.8 M, 147 hit (tenant 4) | 1.86 M, 84 hit, 69 read (tenant 1) | | 68.43 MB | 193.2 M instructions, 315 kB of WAL |
| `'auto'` (`kind, score: amount`) | unchanged | 2.00 M, 73 hit, 53 read: 7.5% more instructions, 13% fewer hit and 23% fewer read blocks (tenants 4 and 5: 4.1% and 3.6% more instructions, 15 and 21% fewer hit blocks); 3.6% more instructions at the 0.1% tenant for 1% fewer hit and 11% fewer read | within 3% in instructions; the distinct members by month of tenants 4 and 5 read 2 blocks hit and 4 read more | 69.03 MB (+0.9%) | 199.1 M (+3.1%), 328 kB (+4.2%) |
| `'channel_id, region: amount'` | 2.66 M, 76 hit, 56 read (tenant 1); 2.29 M, 67 hit (tenant 4); 2.24 M, 64 hit (tenant 5); 2.07 M, 69 hit, 57 read at the 0.1% tenant (+1.5% instructions, 6 blocks hit) | unchanged | all within 3% | 70.63 MB (+3.2%) | 197.5 M (+2.2%), 321 kB (+1.9%) |

The pair `auto` chooses is asked by no class of the comparison, and on the one it touches it trades instructions for blocks, so the default stays without a rollup: the cost is small and the gain is not one that a statistic of the table can promise. The pair that pays is the one the queries filter on, and a rollup is read cell by cell, so a wider rollup loses what it gains: on the 600,000-event fixture a rollup of `channel_id, region, score` (2,400 combinations) reads the filter at tenant 1 in 2.9 M instructions where the pair reads 1.9 M, and at tenant 4 it reads 2.24 M and 67 blocks hit where the units of the arm without a rollup read 2.17 M and 51; the pairs of the other columns have 60 to 200 combinations and no class asks for them. `sift.index_advice` lists the pairs that fit and what they cost; CREATE INDEX and REINDEX say what the rollup of a scope index takes (`the rollup of the rows (a, b: m) takes 0.2 MB of the index (+0.9%)`, measured on the bytes of every group and a sample of the units, from 1 MB of index). A declared rollup also costs the small tenants a look at the rollup of each unit that has none, and a group of two or three units that lacks it is not tried for a count by one of its columns (the small tenants above, and `count by kind, small key` of the specs).
- **The distinct members by month, and what the sets do not serve.** [The id sets by month](#key-groups-one-summary-for-the-run-of-units-of-a-key) answer `count(DISTINCT member)` by month of one key from its groups (83.2 M instructions to 8.6 M at the largest tenant of 2.4 million events). A statement of another shape reads the units of the tenant, as it did: two keys (`IN (1, 2)`), a text or uuid member, a month cut by a test on the date or a session zone that skips a day, a `sum` or a filtered aggregate next to the distinct count, a tenant whose pairs changed (the patch and the delta), a key whose rows span more than 8,192 days in one run, and a leaf whose values are negative or spread over more than 4 million (a bitmap takes a byte for each row at most). The scan of a tenant of 4 to 6% still reads its lone units, which is what is left of the 9 M of those cells.
- **Latest-N and top-N and changed pairs.** A top-N of the scan (`ORDER BY .. LIMIT n`, which is what a latest-N read is) reads the units less the members whose pairs changed, and takes the patch and the delta (the rows of the changed members and of the write buffers, one copy for each pair) as units of the same scan: its heap holds the rows of both under the sort and the tie rule of the join, and the delta copies carry the leaves the top-N sorts and outputs by, which the units alone give it. The delta cannot be cut by the leading sort key before the heap sees it: other filters and the visibility of the buffered rows decide which of its rows count, so the heap does the merge. A unit whose rows tie on the first sort column (the last day of a tenant of 49%: 10,851 rows on one date, the second key the id) is read from its end when its best rows are there, which takes the latest 20 of that tenant from 10.3 to 5.8 million instructions. An index with `cols(..)` answers a top-N from its units only when they hold every column it reads (`column .. not in the scope units`; a top-N whose output was not among the columns kept returned NULL for it before). Tests `agg_scope_topn_delta` (nine shapes of the sort in ten states of the pairs and the facts, md5 equal to the join, the route each took) and the oracle gate `col_scope_oracle`, which compares latest-N and top-N of every state with stock PostgreSQL. Latest 20 at 2,400,000 events, the units keyed by member and time, backend instructions and blocks hit/read of a cold run, the pairs of the group of a tenant changed (`member`), pairs granted directly (`direct`) and rows in the write buffer, before and after, against the join (the tenant of 0.1% and the one of 5%):

| state | tenant | before | after | join |
|---|---|---:|---:|---:|
| clean | 0.1% | 2.14 M, 18/12 | 2.14 M, 18/12 | 3.11 M, 49/20 |
| 20 members changed | 5% | 79.8 M, 5,644/1,211 (the join) | 12.1 M, 209/75 | 79.1 M, 5,591/1,192 |
| 200 members changed | 5% | 82.3 M, 5,552/1,212 (the join) | 70.2 M, 187/164 | 79.3 M, 5,676/1,193 |
| 2,000 rows buffered | 5% | 21.3 M, 59/56 (the join) | 6.0 M, 42/24 | 18.6 M, 15/42 |
| 20 members changed | 0.1% | 4.07 M, 127/40 (the join) | 9.64 M, 89/40 | 3.12 M, 50/20 |
| 20 pairs granted directly | 0.1% | 3.90 M, 106/37 (the join) | 4.76 M, 50/23 | 3.19 M, 67/29 |
| 2,000 rows buffered | 0.1% | 3.84 M, 76/27 (the join) | 4.07 M, 35/23 | 2.85 M, 34/20 |

A top-N is priced from the units its walk reads (350,000 before any unit, 14,000 for each unit of the keys whose zone is probed, and the units the n rows fall in), and the patch and the delta as everywhere (`SIFT_SCOPE_DELTA_INSTR`); the join as written is priced at the planner's scan of every row, which is the most a top-N join costs: it stops after its n rows, where the rows come first in the order of the sort key, and how soon is not a function of any statistic the planner has (the zones of the entity segments keep 97% of the rows of a tenant in this table, and the join of the tenant of 5% reads 79 M instructions for its latest 20 where that of the tenant of 0.1% reads 3 M). Five cells stay over the bound of 0.35 M instructions and 25 blocks for it (cold run: instructions, blocks hit/read; the cell's other arm is the join):

| state | tenant | request | scope read (the planner's choice) | join |
|---|---|---|---:|---:|
| 200 members changed | 5% | top 20 sums | 87.6 M, 443/557 | 82.0 M, 1,329/1,227 |
| 20 pairs granted directly | 0.1% | latest 20 | 3.52 M, 39/25 | 3.03 M, 49/31 |
| 20 pairs granted directly | 0.1% | top 20 sums | 3.93 M, 43/26 | 2.77 M, 49/30 |
| 20 members changed | 0.1% | latest 20 | 5.15 M, 78/42 | 2.96 M, 32/22 |
| 20 members changed | 0.1% | top 20 sums | 5.57 M, 82/43 | 2.70 M, 32/21 |

The answers are the join's in all of them; the loss is 0.5 to 2.9 M instructions for the tenant of 0.1% and 5.6 M for the one of 5% that changed 200 members (in the cell of the 5% the scope read touches 1,000 blocks against 2,556).
- **Small tables.** At 100,000 events the member keys cost 2.32 to 5.09 M instructions and the scope index 0.90 to 1.93 M (a saving of 1.4 to 3.2 M) on the six requests of the first table; on the nine requests of [the access model](#what-it-costs) at 100,000 facts the join costs 12.8 to 16 M (2 to 5 ms) and the scope index 2.7 to 4.7 M (0.4 to 1.9 ms); [the break-even](#break-even-the-multiplicity-limit) at that size is 2.0 entries a row.
- **Members in many keys.** Above 3 entries a row `CREATE INDEX` refuses the scope and says the size it would have had; the join answers, at the cost of [the member keys](#without-a-scope-index-replicate-or-reuse) and nothing more. Leave a scope undeclared too when the extra size, build and the 0.8 MB of WAL for each 10,000 facts of promotion are not worth the saving.
- **Changed pairs of a tenant that received members.** Until the segments are rewritten the read of a changed key patches the members that changed; when those hold much of the tenant's facts the patch costs 1.3 to 4 times the join and the planner still chooses it ([measured](#what-it-costs)).
- **Soft deleted pairs after the segments were written.** The update that soft deletes a pair is a journal record like an insert, and the journal expects every member whose last record inserted a pair to be patched, so the read of a tenant that lost members is priced with a patch it does not have and the planner may run the join where the units were cheaper: in a 300,000 fact lab, a group of the second largest tenant that lost 5 members, a date range count costs 8.3 M instructions as the join and 3.1 M from the units (the join was run, 5.4 M over the cheaper; the same tenant's latest 20 8.9 against 3.1). The answer is exact in every case; `REINDEX` or the automatic rewrite fills the segments again and ends it. The journal cannot be made to tell it: a soft deleted pair does not say whether the member left the keys or stays through the other path of the scope (a direct grant ended while the member is still in a group of the tenant), which only the listed pairs say, and pricing the soft delete as a removal made the planner list the pairs of the largest tenant and fall back to the join in 10 more cells (2.4 M instructions over), so the price keeps the expectation.
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
  later changes nothing (a partial btree made after a plain one is not taken by a `REINDEX`: a new `CREATE INDEX` chooses again, or `using` names it then). `ALTER INDEX` with another `using` than the recorded btree is refused: the segments were written under the recorded one.
- **Journal index.** A plain one, or a partial one whose predicate the predicate of the pairs' btree implies (`WHERE deleted_at IS NULL` beside the same predicate: a soft delete writes one record). A partial one
  that a pair can be inserted outside of does not qualify: it never hears of that pair.
- **What the predicate can be.** What PostgreSQL accepts in an index predicate: immutable, over the columns of the row (`IS NULL`, comparisons with constants, boolean columns). `now()` is refused by PostgreSQL
  (`functions in index predicate must be marked IMMUTABLE`), so a pair that ends by the clock needs a column the application or a scheduled `UPDATE` sets (`UPDATE grants SET expired = true WHERE expired_at <= now() AND NOT expired`,
  a journaled write: exact as of the sweep, so the staleness is the sweep interval), or the pairs kept from a view ([INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view), which has `sift.scope_expire_pairs`).

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
  across the leaf pages of the btree, in either order of its columns, partial, by each way the pairs are known), `col_scope_pairs_multiset_partial`, `col_scope_pairs_multiset_twohop`.
- **Which forms are exact.** A read with the meaning of `IN` or `EXISTS` counts a fact once whatever the rows of the pair and is read from the units. A `JOIN` to the relation counts a fact once for each row of a pair, and a read
  for each pair (`GROUP BY` the scope key, several keys counted for each pair) once for each row too, which the units (one entry for each distinct pair) cannot: those run as written, the plan says why
  (`sift.explain_why`, arm `scope`: `pairs are a multiset (non-unique btree): a join counts a fact once for each row`), and their rows are the join's.
  The planner learns it from the btree the index records (a dependency of the index, so a btree made later changes nothing, not even for a `REINDEX`). A scope of two paths defines its pairs as the distinct union, so a read for each pair of it counts each pair once whatever the rows hold.
- **The pairs kept by `sift.scope_follow_pairs`** are written by sift as the distinct rows of a view, so they are unique.
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

**The three btrees.** Each relation has its own recorded btree on exactly its two columns, unique or not, with the rule of the pairs of one relation (above): `CREATE INDEX` chooses it (a unique one first,
then the only partial one, else the lowest oid of the plain ones; two partial ones are an error that asks for `using`) and `REINDEX` keeps it, and both say which, one `NOTICE` for each path (`the direct pairs of "grants" (dim_a_id,
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
makes the missing ones (a role that does not own a relation without one is refused, and the hint has the `CREATE INDEX` for its owner). The scope index finds the journals when it is created or rebuilt and depends on them (a journal made later is found by
the next `REINDEX` of the scope index, which starts it before it reads and stamps the segments with it: the reads say `unfilled` until then). A journal rebuilt (`REINDEX` of it) is another lineage: the reads say
`other lineage` (the join answers) until the maintenance has written the scope segments again under the new one: it sees them as stale and the reads of a table that autovacuum covers ask for it, so no `REINDEX` of the scope index is needed (the test `col_scope_journal_reindex`; a table with `autovacuum_enabled = false` is filled by `sift.col_maintain`). Each journal has its own stamp.

**The stamp.** The entries are exact as of a journal position, and so is the absence of an entry (a row whose member had no pair then); a read patches the members whose pairs changed since the oldest of these. The index keeps that position as a floor in its metapage (one stamp for each journal of each scope), so that it does not depend on a scope segment existing: the build sets it from the segments it wrote (a build of an empty table takes the stamp of a fill at its end); a promotion lowers it to its own stamp when that is older, whether or not it wrote a scope segment, and sets it when the index has no entity segment; the rewrite that fills every scope segment again under one stamp (after a rebuilt journal, many changed pairs, or a price) raises it to that stamp in the same switch of the directory, with no scope segment when no member has a pair; a merge, VACUUM and the drop of a segment with no live entry leave it alone. It is only ever older than the truth, which a read pays for in patched members and never in a wrong row. So a table whose pairs are empty or arrive later is read from the units at once (the test `col_scope_stamp`), the first scope segment that appears does not send older entity segments to the join, and the maintenance fills an index with no scope segment when the pairs that changed are many. A segment keeps in its header the stamp it was written under (`sift._test_col_scope_stamps`), which the build reads to set the floor; the reads do not.

**The members that changed.** A segment of a scope of two paths holds a stamp for each of the three journals. The members whose pairs changed since the stamps are the union of the members of J1, the members of J3
(the ones that left a group are there), and the members of every group in J2 as the recorded btree of `vrel` lists them for the reader's snapshot (the active rows, the predicate applied). The result is exact: the pairs of
a member depend on no other row (the three lists above), a row of one of them that changed names the member, or the group whose members are looked up now, in a journal; a member that left a group is in
J3. The read is the one of one path: the units of the keys less the changed members, plus the entity rows of those that have a pair with a key now (the patch), and the write buffer's
members the same way. The reader's cap bounds the changed members (a group with more members than the cap is `too many changed keys`: the entity rows are read; [the price](#what-the-planner-recognises)), and the refresh of the maintenance
counts them against `sift.col_scope_refresh_pct` as it counts those of one path, the members of a changed group included.

**Cost.** 100,000 facts, 2,000 entities, 200 groups, 20 accounts, 3,321 pairs; a read of one key (the account), instructions:u of the backend, cold and warm, in millions:

| | nothing changed | 1 group changed (10 members) | 100 groups changed (1,090 members, 135 patched) | the plain join |
|---|---|---|---|---|
| cold | 6.02 | 12.5 | 26.5 | 68.9 |
| warm | 1.34 | 7.8 | 21.8 | 63.6 |

The changes cost 6.5 M instructions for one group and 20.5 M for 100 groups over the first column, cold and warm alike; the join reads 63.6 M even warm.

**Column format minor 49.** A segment of an index that declares a scope of two paths holds three stamps and is written as column format minor 49 (docs/FORMATS.md); an index whose scopes all have one path
keeps writing minor 45 with one stamp for each scope. A library before 5.0, and a 5.0 build before minor 49, refuse the index (`Use a newer pg_sift`, `REINDEX`): `CREATE INDEX` says so in a `NOTICE`. A scope
declared with another number of paths than its segments were written with (the declaration changed from one to two paths or back, in an index built before; `ALTER INDEX .. SET (scope = ..)` of an index that holds a segment is refused) is not read as stored (`unfilled`, the maintenance says `stale scope`)
until `REINDEX`.

**The planner and the legs.** The scan takes the conjunct from the join as an application writes it (the recognition below), as for one path. The planner reads the paths from `sift_scope_legs(index, scope, legs[2])` (2 legs for this declaration, the second one joined: `via_rel`, `via_join`, `via_member`, `via_btree`, `via_pred`) and recognises the `UNION` of the two
paths as the sub-select of `IN` or `EXISTS` and as the relation of a join (table below): each select reads the relation of its path with the predicate of its btree, both bind the same key by constants or parameters, and no other condition or column
is read. What else a statement says keeps the join as written, and `sift.explain_why` (arm `scope`) says which select breaks the proof.

**What stays impossible.** A relation that is a view, a CTE or a function (the three are heap tables with a btree on their two columns, unique or not); a condition on a third table (a group's own soft delete: the memberships of a deleted group
stay pairs; put the condition on the memberships or keep the pairs from a view with [INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view)); a predicate that depends on time (`now()` is refused in an index predicate: sweep it
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

A read of a string member is exact in every state the integer member is exact in, and reads the units under writes as the integer member does. The stored entries of the members whose journal word changed since the units were written are left out of the units (the source hashes the strings of the member leaf of each unit it reads, once for each distinct string of a dictionary, and tests the word), the pairs of those members are listed by their word and their rows are read by value from the entity segments (the members are looked up once in the dictionary of each segment, and the zone and the leaf of a unit, which hold dictionary codes, say which units hold a row of one: a patch reads the units of its members, as an integer member's does, and `EXPLAIN` counts them in `Scope Patched Units`; only a unit that stores its strings inline, a string past 255 bytes, is read whole), the rows of the write buffer are copied by the pairs of their member, and a member of several keys counted once is dropped from the units of its other keys. Members are identified by their bytes: a read gives each value of the pairs a code for the length of the statement, and the rows of the segments and the buffers find their code by the string, so two members that share a journal word are both left out and both read again, which only widens the patch (the test build can force it: `col_scope_textmember_collide` runs every form and every state of `col_scope_textmember` under it, and the fuzz runs a seed set under it). The statement runs the join as written, and says why in `Scope Fallback`, in the cases an integer member does (more changed members than the read bound, a write buffer over `work_mem`).

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
| one key with 375 changed pairs, thousand instructions (warm blocks hit; cold hit / read) | 21,854 (122; 442 / 111) | 22,493 (121; 461 / 119) | 22,293 (122; 519 / 144) |
| five keys counted once with 375 changed pairs | 28,985 | 30,306 | 30,856 |
| one key with 375 changed pairs and 3,000 facts in the write buffer | 24,964 | 26,560 | 27,097 |
| the same, the join as written (`sift.agg_scope = off`) | 5,722 | 9,935 | 9,733 |

The text member's index is 11% larger (each row of a scope unit holds its member string, where an integer member holds the integer). With changed pairs a string member costs 1.02 to 1.03 times the instructions of the integer member (the leave-out hashes each distinct string of a dictionary once, and the patch reads the member leaf of every entity unit), with none the read is the same. Before the patch was made a leaf at a time the same reads cost 82,430, 77,683 and 77,740 thousand instructions (one key, 375 changed pairs), and 84,066, 79,559 and 79,732 (five keys). The times are not in the table: the host was shared by other runs and the instructions of a statement do not depend on it. The units are priced against the join as written: at this size the planner's price of the patch is above the join's, and the figures of the scope rows are those of a forced read (`sift.agg_force`), as for an integer member. The join a node holds runs the lookup join of the sift aggregate node (the figures of the join rows are those of the forced node); a join by a uuid column reads only the units and rows that hold a key, which cut its 335 million instructions here to 10 million.

### Two paths with text, varchar or uuid keys and members

A scope of two paths (a direct grant and a grant on a group) takes a `text`, `varchar` or `uuid` scope key and member too, with the same refusals as one path. The scope key of the direct pairs and of the grants of groups are of one type, and the member of the direct pairs and of the memberships
are of one type (a declaration where they differ is refused with both columns and their types); the group column that joins the second path to the memberships stays an `int2`, `int4` or `int8`. The key column is coded in the value store of the pairs relation (the units are cut by the code, as for one path), the
members of the three relations are coded by one map of the statement, and the three journals record a member as the journal of one path does (the word of its bytes) and a group by its integer. The paths of a read
are the integer scope's: the stored units less the members the journals say changed (a member that was reached by the other path is read again from the entity rows), the entity rows of those members read by value, the write buffer by the pairs of its member, and the dedupe of a member that two keys reach.
Each record of the journals of the pairs is tagged by the word of its key (cut to 14 bits), as an integer key tags it by its value, so a read of one key lists as changed, leaves out and patches the members of the pairs that changed in that key and not those of other keys (test `col_scope_textkey_tags`); a record with no tag is of every key. A colliding word leaves out and patches more members, never fewer or different ones: `col_scope_texttwohop_collide` and the fuzz leg `--two-paths --hash-bits 3` run it with the words cut to 3 bits. Tests: `col_scope_texttwohop` (a text key and member, a varchar fact column, a uuid key and member, an integer key with a text member, in 18 states of the
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
| one key with 375 changed pairs, thousand instructions (ms; warm blocks hit; cold hit / read) | 24,492 (4.2; 148; 613 / 124) | 26,389 (7.0; 156; 662 / 144) | 26,783 (7.5; 155; 781 / 178) |
| five keys counted once with 375 changed pairs | 91,281 (19.8) | 116,194 (14.5) | 116,432 (18.2) |
| one key with a changed group grant and 40 changed memberships on top | 101,412 (14.3) | 131,747 (17.2) | 132,058 (20.1) |
| the same and 3,000 facts in the write buffer | 100,461 (15.8) | 128,786 (20.1) | 129,101 (20.5) |
| the same, the join as written (`sift.agg_scope = off`) | 7,818 (2.6) | 11,933 (2.0) | 46,388 (4.0) |

Unchanged, a string key or member reads as the integer one (the five keys are read from the pairs by a scan of their btree, which translates each pair, as for one path); with changed pairs a text member costs 1.08 times the instructions of the integer one and a uuid member 1.09 (a member of the units is found by its code in the member leaf of each unit, a code is turned into the member's journal word for each pair it lists). The figures of the reads with changed pairs are those of a forced read
(`sift.agg_force`): the join as written costs less at this size for any type and the planner prices the patch against it, as for an integer scope. (The uuid join is the node's lookup join over the units and rows that hold a key, as in [Member types](#member-types).)

### Limits

A library before 5.0 refuses the index. What a scope does not do in 5.0:

- **Declaration.** The member key and the columns of the relations are `int2`, `int4` or `int8`, or `text`, `varchar` or `uuid` of a deterministic collation, in a scope of one path or of two (a coded key or member: see
  [Key types](#key-types), [Member types](#member-types) and [Two paths with text or uuid](#two-paths-with-text-varchar-or-uuid-keys-and-members); the group column of a scope of two paths stays an integer; `char(n)`, `citext`, a domain and a nondeterministic collation are refused with their reason, and the member of the pairs and the column of the index are of one kind). A relation is a heap table that is not partitioned, not a view and not the indexed
  table; a scope of one path has both columns NOT NULL. At most 4 scopes and 8 connections on an index, and a scan reads one scope. At most one joined path and no third hop; no deny rows; no condition on a third table and no predicate that depends on time.
- **Forms.** A statement that is not in the table of recognised forms keeps the join (`sift.explain_why`, arm `scope`): among them a join or a read for each pair over the pairs of a non-unique btree, a join to a table of keys that may hold a key twice,
  a key in an expression, a range of keys, `ROLLUP` / `CUBE` / grouping sets over the key.
- **Text, varchar and uuid members.** A patch finds the units of its members by their dictionary codes, and its cost in instructions is the integer member's plus 2 to 3% (one path; 8 to 9% for two paths) and its warm blocks the integer member's or below them (table in [Member types](#member-types)). A column of another type is refused: a domain over a supported type too, because a `sift_col` index does not take a domain column at all.
- **The patch costs for each row of the changed members.** A read of one key with changed pairs reads the member leaf of every entity unit the zones leave open (23 instructions an entity row: 5.7 million for 300,000 facts) and decodes, copies, encodes and scans the rows of the changed members for each of their pairs (450 instructions a copy for one key, 700 to 900 for several): 21.9 million for 375 changed pairs of 300,000 facts (37,500 copies; the join 5.6), 124 million for 3,000 (300,000 copies; the join 13). The warm blocks of such a read are about 120 (one path; 150 for two) where a clean read hits 38 (a reader keeps its 8 most recent pages pinned, and the cache of the leaves, the tombstone log and the covered check read the segment list alone: about 40 blocks fewer than before): the member leaf of every unit (45 blocks), the heap addresses of the selected rows (41) and the columns the read uses (8 to 20) are read, and no layout of this table avoids the first two (a point directory of the member leaf, `point_tid_columns`, costs more than it saves here; units cut by the member, `key_order = 'm, t'`, read 145 blocks and 24.4 million instructions). The shape of the changed members decides what a copy costs. On the 2,648-member stock shape (300,000 events, units cut by member and time, a scope of two paths, eight columns, the read grouped by a text column) granting members to a tenant of 11 members costs: the largest member (2,567 copies, one unit) 4.3 million instructions at 74 blocks hit (10.4 million and 78 before); 375 adjacent members (49,533 copies, 2 units) 39.2 million at 108 blocks (138.1 and 117); 375 members spread over all (45,216 copies, 10 units) 45.2 million at 271 blocks (146.1 and 315); the join 4.3, 8.5 and 9.2 million. A read of every key grouped by the key with 375 changed pairs costs 118 million for an integer member (212 before): the copies of every key are sorted by key and address, and the stored entries of the changed members are left out of each unit.
- **The copies of the changed pairs are read by the leader.** The workers of a scope read share the units; the rows of the write buffers and of the changed members (the delta) are read by the leader alone, and a read that is ordered or a top-N over a delta stays in one process. A read of a few units is priced as one that starts no workers.
- **Snapshots.** A `SERIALIZABLE` or non-MVCC snapshot always takes the join (`snapshot`).
- **Generic plans.** A condition on the relation that reads a parameter is proved only by a custom plan: a generic plan keeps the join for a relation with a partial btree.

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

## A denormalised table: not shipped

A copy of the fact table for each scope key of its member, kept by triggers and read by a plain query on a table (`WHERE tenant_id = k`), is the other way to make a request by a key above the fact table cost what the key holds. 5.0 does not ship a function that builds one: [the comparison below](#against-tuned-stock-postgresql) builds it by the benchmark's own SQL (`denorm_flat`, one table with a `(tenant, time)` covering b-tree, and `denorm_part`, a list partition by tenant) and finds it 13 to 28 times slower than the scope index to read for the tenants of 6% or more and 3 to 4 times dearer to write; it wins only the latest 20 rows of a key and a selective filter. Facts that must stay one table, with no second copy to size, to vacuum and to keep by triggers on the write path, are the scope index's case; pairs that are a view or depend on time are [kept by `sift.scope_follow_pairs` and `sift.scope_expire_pairs`](INDEX-RECIPES.md#keeping-the-pairs-from-a-view). A table built that way takes the first column of its `sift_col` index as the key of `key_order` and `key_cut` by itself (the build rule for tables of that shape).

## Prior art: what exists and what differs

Reaching facts by a key that they have through a join, and storing or indexing them that way, is an old idea with many implementations. This section sets the scope index beside the ones whose own documentation or paper was read for it, on 2026-11-13 (the pages and versions are listed after the table). No system was run for this section. The first five rows use the figures [measured above](#against-tuned-stock-postgresql) on the stock-arms fixture (one generated fixture, one backend, warm; instructions of the backend, 10,000 inserted events, one member granted to a tenant); for every other row the table compares mechanisms taken from the documentation, with no figure, and a cell the documentation read does not settle says "not verified".

| system | found by | many-to-many | one pair changes | one fact is inserted | concurrent transactions | the application writes | answers the scope index does not |
|---|---|---|---|---|---|---|---|
| scope index (this page) | the scope key of a table of (member, key) pairs | yes, up to 3 entries a row by default | a 32-byte journal record (0.19 M instr, 899 B of WAL for a grant); a read patches the member | write buffer, one copy per key at promotion: 146 M + 179 M instr for 10,000 | the reader's snapshot: journal, patch and delta; `SERIALIZABLE` reads the join | the plain join | |
| tuned semi join, nothing stored | the member, by a covering b-tree | yes | 0.13 M instr, 323 B of WAL (measured) | 211 M instr for 10,000 (measured) | the statement's snapshot | the plain join | nothing more; it is faster for a selective filter (9 of 72 cells) |
| rollups by member, kept by a statement trigger | the member, read through the semi join | a pair edit does not touch them (0.13 M instr) | none | 1,046 M instr for 10,000, 7.7 MB of WAL (measured) | concurrent writers not measured | the semi join over the rollup table | nothing more: aggregates only, 4 to 12 times slower than the scope read for tenants of 6% or more |
| pg_ivm 1.17 | the columns of the view | yes (inner joins; outer joins restricted) | view updated in the transaction: 0.5 to 1.3 G instr, 67 to 280 ms (measured) | 10,000 inserts: 11.8 G instr, 1.65 s (measured) | immediate; `ExclusiveLock` on the view in `READ COMMITTED`; an error in `REPEATABLE READ` or `SERIALIZABLE` on a conflict | the view, by name; no rewrite in the documentation | a fitted aggregate in 0.05 ms (measured); no `ORDER BY` or `LIMIT` |
| Oracle bitmap join index | a column of a dimension table, to rowids of the fact table | no: the dimension join columns must be a primary key or unique | not verified | not verified | one table of the join can be updated concurrently | a plain join | not verified |
| Oracle materialized view with joins | the columns of the view | guideline: each fact row joins one dimension row; otherwise not verified | `ON COMMIT`: refreshed in the commit, from a log on each table; `ON DEMAND`: stale until refreshed | same | `ON COMMIT`: part of the commit; `ON DEMAND`: stale | the plain query, rewritten by the optimizer | any stored aggregate of the join |
| SQL Server indexed view | the unique clustered key of the view | inner joins; no cardinality rule on the page | view rows updated in the DML statement | same | not verified | the plain query, substituted (specific editions) or the view named with `NOEXPAND` | `SUM` and `COUNT_BIG` aggregates of the join; no `MIN`, `MAX`, `AVG`, outer join or `TOP` |
| PostgreSQL materialized view | the columns of the view | any query | nothing until `REFRESH` recomputes the query | same | the snapshot of the last refresh | the view, by name | any stored result |
| ClickHouse projection | the sort key or aggregate of one table | no join in a projection | not applicable | the projection part is written with the insert | not verified | the plain query, chosen by the optimizer | an order or an aggregate of one table; no lightweight updates or deletes |
| ClickHouse incremental materialized view with a join | the key of the target table | a join is allowed; only the left-most table triggers | nothing: rows already stored are not updated | the view query runs on the inserted rows; right tables are read at that moment | not verified; each fact keeps the pairs it had when inserted | the target table, by name | an aggregate computed at insert time |
| ClickHouse dictionary | the dictionary key, to attributes | not verified | reloaded at the `LIFETIME` interval | none (not in the fact table) | the state of the last load | `dictGet` in the query, in place of the join | an in-memory attribute lookup |
| Zanzibar Leopard index | a set id (user, group), to element ids | yes: nested groups, users in several groups | update tuples in an incremental layer; one tuple "may yield potentially tens of thousands" of index events | not applicable: it indexes membership tuples, not facts | updates up to the query timestamp are merged onto the offline index at query time | the authorization API (check, expand), not SQL | union, intersection and exclusion of sets, across shards, for deeply nested groups |
| SpiceDB with Materialize; OpenFGA | permission sets (SpiceDB); none built in (OpenFGA) | yes: relationships between subjects and resources | Materialize recomputes the affected permission sets (cost not stated); OpenFGA: none | Event Streams give a copy next to the data (cost not stated) | per-request consistency by `ZedToken`; Materialize not verified | the same SpiceDB calls; OpenFGA: a list call, or the application's own index from its changes endpoint | checks and lookups over a permission schema |
| Elasticsearch, OpenSearch | a field of each document, or a terms lookup of another document's values | a terms query matches on any of its terms; a join field has one parent for each child | terms lookup: one document changes; a denormalised field: not verified | the document carries its keys | not verified | the user's search, with the role query of document-level security added | relevance scoring |
| StarRocks, Doris asynchronous materialized view | the columns of the view | joins of several tables; no cardinality rule read | refreshed by hand, at an interval or on a base-table change; Doris refreshes the affected partitions | same | the state of the last refresh; Doris: eventually consistent | the plain query, rewritten (SPJG) | any stored aggregate of the join |
| join index (Valduriez, 1987) | the join attribute, to pairs of tuple surrogates | yes: the join predicate "can be arbitrary" | delete and insert of pairs; about twice the update cost of the joined relations for joins not on a foreign key; may be deferred to the next join | a pair for each joining tuple | not discussed | the join, run with the index | other operations over the pairs; graphs and recursive queries |
| PostgreSQL row-level security | none: a predicate evaluated for each row, a sub-select on the pairs | yes | nothing | nothing | the statement's snapshot; the documentation warns that sub-selects in policies can race | the plain query; the server adds the policy | access checks on writes |

The first five rows are measured; the others are mechanisms. Pages read, with the version or edition each gave: Oracle Database documentation, Data Warehousing Guide, chapter "Data Warehousing Optimizations and Techniques" (bitmap join indexes) and "Basic Materialized Views" ([docs.oracle.com, release 26](https://docs.oracle.com/en/database/oracle/oracle-database/26/dwhsg/data-warehouse-optimizations-techniques.html)); SQL Server, "Create indexed views" ([learn.microsoft.com, SQL Server 2025](https://learn.microsoft.com/en-us/sql/relational-databases/views/create-indexed-views?view=sql-server-ver17)); PostgreSQL 18, [materialized views](https://www.postgresql.org/docs/18/rules-materializedviews.html), [`REFRESH MATERIALIZED VIEW`](https://www.postgresql.org/docs/18/sql-refreshmaterializedview.html), [`CREATE TRIGGER`](https://www.postgresql.org/docs/18/sql-createtrigger.html) and [row security](https://www.postgresql.org/docs/18/ddl-rowsecurity.html); [pg_ivm](https://github.com/sraoss/pg_ivm) README of the main branch (version 1.17, PostgreSQL 13 to 19); ClickHouse, [projections](https://clickhouse.com/docs/data-modeling/projections), [incremental materialized views](https://clickhouse.com/docs/materialized-view/incremental-materialized-view) and [dictionaries](https://clickhouse.com/docs/sql-reference/dictionaries) (current documentation, no release number on the pages); Pang et al., "Zanzibar: Google's Consistent, Global Authorization System", USENIX ATC 2019, section 3.2.4; [AuthZed Materialize](https://authzed.com/docs/authzed/concepts/authzed-materialize) and [SpiceDB consistency](https://authzed.com/docs/spicedb/concepts/consistency), and [OpenFGA, search with permissions](https://openfga.dev/docs/interacting/search-with-permissions) (current documentation); Elasticsearch, [parent-join](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/parent-join), [terms query](https://www.elastic.co/docs/reference/query-languages/query-dsl/query-dsl-terms-query) and [document level security](https://www.elastic.co/docs/deploy-manage/users-roles/cluster-or-deployment-auth/controlling-access-at-document-field-level) (current documentation), and OpenSearch [document-level security](https://docs.opensearch.org/latest/security/access-control/document-level-security/) (latest); StarRocks [asynchronous materialized views](https://docs.starrocks.io/docs/using_starrocks/async_mv/Materialized_view/) (documentation labelled Latest-4.1) and Apache Doris [asynchronous](https://doris.apache.org/docs/query-acceleration/materialized-view/async-materialized-view/overview) and [synchronous](https://doris.apache.org/docs/query-acceleration/materialized-view/sync-materialized-view) materialized views (current documentation; the synchronous ones are single-table and do not join); P. Valduriez, Join Indices, ACM Transactions on Database Systems 12(2), June 1987. Incrementally maintained joins of the pg_ivm kind are also offered by streaming databases (Materialize keeps an indexed view "incrementally updated"); that documentation was read only that far and has no row.

**What is not new.** The idea: store, or index, the facts of a table by a key that they reach through a join, and keep it right as the pairs change. A join index of pairs of tuple surrogates is Valduriez's of 1987, and that paper already lets the update of the index wait until the next join, with a list of the changed tuples and a semi join to reconcile them. Bitmap join indexes, indexed views, materialized views and the copies kept by triggers store the join or an aggregate of it; Leopard flattens group membership into an index and merges the changes made since the index was built at the query's timestamp, which is the same family of mechanism as the patch and the delta here (a base, plus the changes since it, read together under a snapshot). Systems that rewrite the plain query to a stored result are common (Oracle, SQL Server, StarRocks, Doris). A many-to-many relation reached by two paths, direct and through a group, is also the shape of group-based authorization (a direct grant and a grant through a group), the case of the engines derived from Zanzibar.

**What is particular, as far as the documentation read shows.** The combination: the rows are stored by scope key inside the index of the fact table itself, so no second table, view or trigger exists; a pair edit of a many-to-many relation is one small journal record, with no rewrite of the member's facts, and a read reconciles it exactly under the reader's own MVCC snapshot, by reading the entity rows of the members whose pairs changed (the patch) and the rows of the write buffers (the delta); two paths, direct and through a group, are one scope; and the planner recognises the join the application already writes and chooses between the units, the member keys and the join by estimated cost. None of the pages read describes that combination. This is a statement about those pages, not a search of the literature or of closed products, and each part has a counterpart above. The closest in mechanism is Leopard, which indexes membership sets and not the facts that the sets select. The ones that store a join result (the views, the copies, the bitmap join index) pay for a pair change in the maintenance of what they store, as the three measured rows show.

**Where the others are better.** A fitted aggregate: pg_ivm answered 22 of the 64 cells it covers faster than the scope read (count by score 0.05 ms against 3.5 ms at tenant 1), and Oracle, SQL Server, StarRocks and Doris store aggregates of a join of any fitted shape, though only pg_ivm was measured here. A latest-N or a selective filter: a copy kept by triggers answered latest 20 in 0.04 to 0.06 ms against 0.3 to 2.2 ms, and a top-N of the scope index takes the changed pairs and the buffered rows as units of its scan, dearer than the join for a tenant of 0.1% of the events (Latest-N and top-N and changed pairs). The tuned semi join beat the scope read in 5 of 72 cells, and is within 0.5 ms of it for a tenant of 0.1% of the events. Materialized views and indexed views have releases of use and tooling behind them, `ON COMMIT` refresh in Oracle is part of the commit, and ClickHouse computes its aggregates at insert time, with no join at read time (a stored fact keeps the pairs it had when it was inserted, which answers a different question from the join). Leopard serves set algebra over deeply nested groups across shards, which a scope of two paths does not (no third hop, no deny rows); Elasticsearch and OpenSearch add relevance scoring. The scope index refuses a member in more than 3 keys by default, reads the join under `SERIALIZABLE` and for a non-MVCC snapshot, needs integer, text, varchar or uuid keys, and its figures are those of generated fixtures of up to 2,000,000 rows.

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

Rollups and incremental views answer only aggregates (latest 20 is not answerable by them). Every cell is the median of three warm runs, on a host shared with other jobs (the instructions and blocks are
exact, the milliseconds of the cells under 1 ms move by a few tenths between runs). The tables of this section are generated by `bench/bench scope-stock --docs`;
the figures in the prose below were read from the tables of the run before it. The header of the results file names the build of each set of arms; the tables are measured again only when a later commit changes a plan or the work of a read.

<!-- bench:scope-stock-geomean:begin (generated by scripts/gen-docs.py) -->
Warm milliseconds, geometric mean over the 8 classes every arm answers (`sum_by_month`, `count_by_score`, `top_members`, `distinct_members_by_month`, `by_member`, `filtered_count`, `date_range_count`, `count_by_kind`):

| tenant (share of the events) | stock_default | semijoin | denorm_flat | denorm_part | rollup | ivm | sift_reuse | sift_scope |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 (49.5%) | 301 | 119 | 81.7 | 147 | 34.8 | 5.59 | 10.1 | 1.19 |
| 2 (19.6%) | 217 | 65.0 | 38.3 | 90.1 | 17.7 | 3.50 | 7.23 | 1.28 |
| 3 (17.4%) | 197 | 77.4 | 47.6 | 65.8 | 18.4 | 2.32 | 6.78 | 1.51 |
| 4 (6%) | 150 | 24.9 | 10.6 | 12.3 | 15.2 | 1.33 | 5.76 | 1.57 |
| 5 (6.2%) | 126 | 25.1 | 10.7 | 13.0 | 3.96 | 0.57 | 2.37 | 1.01 |
| 6 (3.9%) | 165 | 11.6 | 6.24 | 10.8 | 3.55 | 0.73 | 3.02 | 0.72 |
| 7 (1.1%) | 99.0 | 4.93 | 2.79 | 4.00 | 5.47 | 0.76 | 4.09 | 1.05 |
| 8 (0.1%) | 3.02 | 1.05 | 0.43 | 0.65 | 1.22 | 0.40 | 2.25 | 0.64 |
<!-- bench:scope-stock-geomean:end -->

Per rival, over the cells both answer (a cell is a tenant and a class):

<!-- bench:scope-stock-tally:begin (generated by scripts/gen-docs.py) -->
| rival | cells | warm: sift_scope faster / tie / rival faster | cold: sift_scope faster / tie / rival faster | where the rival is faster (warm) |
|---|---:|---:|---:|---|
| `stock_default` | 72 | 72 / 0 / 0 | 71 / 0 / 1 | - |
| `semijoin` | 72 | 63 / 2 / 7 | 62 / 1 / 9 | `filtered_count` 5, `latest_20_rows` 2 |
| `denorm_flat` | 72 | 51 / 2 / 19 | 48 / 1 / 23 | `latest_20_rows` 8, `filtered_count` 8, `date_range_count` 2, `by_member` 1 |
| `denorm_part` | 72 | 54 / 4 / 14 | 54 / 0 / 18 | `latest_20_rows` 8, `filtered_count` 4, `date_range_count` 2 |
| `rollup` | 64 | 54 / 5 / 5 | 52 / 3 / 9 | `filtered_count` 3, `distinct_members_by_month` 2 |
| `ivm` | 64 | 32 / 2 / 30 | 34 / 3 / 27 | `count_by_score` 8, `filtered_count` 8, `count_by_kind` 6, `distinct_members_by_month` 4, `date_range_count` 3, `sum_by_month` 1 |
| `sift_reuse` | 72 | 70 / 1 / 1 | 68 / 3 / 1 | `filtered_count` 1 |

Cells are a tenant and a class (72 of them for `sift_scope`, in warm and in cold milliseconds); a rival is faster in a cell when it is faster by more than 0.1 ms and by more than 10%, and the cell is a tie within either margin. Every answer is md5-equal to the plain statement (700 of 700 cells). Milliseconds are the executor time of `EXPLAIN (ANALYZE, TIMING OFF)`.
<!-- bench:scope-stock-tally:end -->

Where tuned stock PostgreSQL matches or beats the scope index, in the cells of the table (72 of them; a rival wins a cell when it is faster by more than 0.1 ms and by more than 10%; absolute figures are in the bench output):

- **The tuned semi join is 27 to 74 times slower than the scope read (geometric mean) for the tenants of 6% or more** (tenant 1, sum by month: 285 ms against 1.9), 8 to 12 times
  slower for the tenants of 1 to 4%, and 0.7 ms behind it for the tenant of 0.1% (1.05 against 0.39). It beats the scope read in 2 of 72 cells: the selective filter class at tenant 1 (2.3 against 4.8 ms) and latest 20 at tenant 8 (0.39 against 0.77 ms).
- **A denormalised copy wins latest 20 in all eight tenants (0.04 to 0.08 ms against 0.24 to 2.1 ms) and the selective filter in eight (0.69 ms against 4.8 at tenant 1)**, 16 of 72 cells in all. It is 9 to 27 times slower than the scope read
  for the tenants of 6% or more (geometric mean; tenant 1: 82 ms against 1.4), since it still reads 1.2 million copied rows.
- **pg_ivm beats the scope read in 24 of the 64 cells it answers**: count by score for every tenant (0.04 to 0.22 ms against 0.3 to 0.9), the selective filter for every tenant, count by kind for five tenants, and
  three cells of the distinct class. The scope read beats it for the sum by month and the members classes (tenant 1 sum by month 1.9 ms against 43, top members
  1.5 against 54, date range 0.62 against 14), and is ahead in the geometric mean at tenants 1 to 4 and 7 (1.42 against 5.59 ms at tenant 1, 0.63 against 1.33 at tenant 4), level at tenant 8 (0.39 against 0.40) and behind at tenants 5 and 6 (0.77 against 0.73 ms at tenant 6). It answers no row retrieval. **Its writes pay for those reads:** 10,000 inserted events cost 11.8 billion
  instructions and 2.7 s (the scope index 0.15 billion, 0.33 with its promotion), and every pair edit 0.5 to 1.3 billion instructions and 94 to 335 ms (the scope index 0.0002 to 0.0003 billion, under 1 ms). Member rollups, which a pair edit
  does not touch, are 3 to 25 times slower than the scope read (geometric mean over the tenants).
- **Why the rest are not won, in instructions.** The cheapest scope read of the table costs 1.7 million instructions at 2.4 million events (1.26 on the 240,000-event fixture of `bench scope-stock --size quick`, 1.38 with the
  EXPLAIN ANALYZE that the bench measures), and most of it is not the read: on the small fixture parsing and planning the statement take about 1.07 million (6 buffer accesses) and the execution 0.21, which is what each execution of a prepared statement with
  a generic plan costs (a custom plan, 1.29). About half of the planning is PostgreSQL's own: the statement in a database without the indexes plans in 0.6 million (the pairs view is a union of two selects, one of them a join). The rest is sift's,
  0.45 million: the cost estimates of the index paths of the pair relations and of the facts (and the operator class entries of sift that each equality carries, two of the seven of an `int4` equality, which PostgreSQL 18 asks the access
  method about while it builds pathkeys: PostgreSQL's own lookup of each, 0.05 in all, which no change of the planner hook or the estimate reaches) 0.23, the planner hook and the first pass of the aggregate scan 0.08 (the passes that look for a rewrite are asked
  only where the statement holds the node each needs, found by one walk, and a statement with nothing to rewrite is planned without a copy), the second pass of the scope (the statement with the join taken out, the proofs of the two paths against the declaration,
  the join priced as the node's alternative) 0.14: the estimate of an index scan of sift costs 7.7 thousand instructions a call against the 6.1 of a btree's, so the 8 calls leave 13 thousand to win; what the second pass reads again of the first
  (the source of the scan) is 2 thousand. The vouching of the journals
  (about 20,000 instructions each) is made when the read starts, not in the planning. The cheapest cells of the incremental view cost 0.7 million; a view that holds the answer reads a few blocks. The classes that the read still spends more on are bound by what they decode: the selective filter reads 1.3 million instructions of filter
  kernels over 148,000 entries at tenant 1 (the joint roll-up of the filter's columns, measured with `rollup_columns`, was not cheaper than the scan: 240 tuples for every 4,096 rows), and a date-keyed sum evaluates the expression key for every
  day of a unit that holds more rows than days.
- **Tenant 1's selective filter is read by the scope units in 4.8 ms, slower than the tuned semi join (2.3 ms) and the copy (0.69)**: at 49% of the events the scope read has no advantage for it, as the break-even above says.
- Four parallel workers did not make the tuned semi join faster in this fixture (tenant 1: 100 ms serial, 116 ms with four).
- **What changed since the tables of 2026-11-13, and what the tables mix.** The arms `stock_default`, `sift_reuse` and `sift_scope` were measured again on 2026-11-15 (PostgreSQL 18.7), the other five arms keep their rows of 2026-11-13 (PostgreSQL 18.6): their
  instructions and blocks do not move with the server minor or the host (`stock_default` repeats within 10% in 72 of 72 cells, geometric mean 1.002), their milliseconds do (the CPU pressure recorded with the first run was 28%, with this one about 1%; `stock_default` tenant 2 changed from 181 to 263 ms
  geometric mean with equal instructions), so the tally compares milliseconds of two runs and a margin of 10% is inside that noise for the larger cells. The first measurement of the day found four things worse, each followed to the commit that moved it. *Latest 20* of the entity index alone read 10 to 13% more instructions at tenants 2 to 8
  (tenant 4: 43.1 to 49.2 million): a unit with a heap page for each row was walked page by page to find the page of every selected row; the pages are now found by search, and the read is back at 43.3 million at tenant 4 and 31.0 at tenant 2 (same plan, same blocks). The build and the promotion of 10,000 events cost 3.4 G and 14 million
  instructions more, because the copy of a single row into a batch built a map of the whole dictionary of each string leaf; a map pays for several rows only, and the build of the entity index alone is now 47.5 G instructions (49.6 before), the promotion 187 million with the scope index (182 before, 194 in the first measurement) and 77.4 without it (74.9 before).
  What stays is paid for: building the scope index takes 72.5 G instructions where it took 71.3 (+1.7%), the index is 3.1 MB larger (65 to 68 MB) and its WAL 3.4 MB larger (66 to 70 MB): the superunit summaries of the key groups (+2.2 MB, +1.4 G instructions) and the id sets of the distinct members by month (+0.9 MB, +1.0 G); the rest of the WAL
  (0.4 MB) came in steps of 0.1 to 0.2 MB over the later commits of the scope maintenance. The seconds in the table are those of a host shared with other jobs (13.2 s with the CPU pressure at 1%, 7.6 to 8.0 s on a quiet one for both builds). The selective filter of tenant 1 reads 986 blocks cold where it read 786, with 17.9 million instructions where
  it read 35.6 and 25.7 ms cold where it read 30.7: since the price of the entries a statement leaves out was corrected the planner takes the scope units (events_col 985 blocks, the filter columns of the 1,149 keys) instead of the join (773 blocks). The cold figure of `sift_reuse` by member at tenant 4 was 0 blocks read in the first run (the eviction missed) and is 709 now.

What the scope read does now that it did not: the scope segments carry the superunit summaries of the entity segments (the merged histogram of 16 units answers an aggregate over a whole key from one read), so a count by score of
tenant 1 reads 77 blocks where it read 1,092 (1.9 million instructions where it was 8.6), and an aggregate decodes only the measures it uses; a latest N over the units cut by time reads each unit from its end on several sort keys
(the rows tied with the last on the first key are read as well); the plans below the scope node start when the statement runs them (36,000 instructions of 0.24 million of a read of a small tenant); an expression group key over a wide range (a
month of a date) evaluates the values the unit holds, not every value of its range (tenant 8 sum by month 2.5 to 0.66 million instructions at 300,000 events, 4.6 to 3.7 at 2.4 million where the unit holds more rows than days). The distinct members by month of tenant 1 come from per-month id sets kept in the key groups: 83.4 to 8.5 million instructions (the top 20 members 50.4 to 6.0, the total for every member 62.9 to 8.8); the index is 3.1 MB larger (65 to 68 MB) and its build 1.7% longer in instructions. Each read pays 6 more blocks than before (the header pages of the superunits).

What each arm costs (added bytes, and 10,000 inserted events as one statement; a pair edit is the mean of 10 members of about 400 events, and the largest member apart):

<!-- bench:scope-stock-costs:begin (generated by scripts/gen-docs.py) -->
What each arm costs, as milliseconds · M instructions · WAL written of one statement (a pair edit: the mean of 10 member keys of about 400 events, and the largest member apart). `-`: the arm has no such step.

| arm | build · size added | 10,000 inserts | promotion of those | pair grant | pair revoke | pair grant, largest | pair revoke, largest |
|---|---:|---:|---:|---:|---:|---:|---:|
| `stock_default` | 1.6 s · 17 MB | 48.4 · 188 · 2.2 MB | - | 0.93 · 0.14 · 323 B | 1.34 · 0.19 · 184 B | 2.81 · 0.12 · 312 B | 1.41 · 0.18 · 184 B |
| `semijoin` | 2.9 s · 97 MB | 64.1 · 211 · 2.6 MB | - | 1.72 · 0.13 · 323 B | 1.09 · 0.19 · 186 B | 1.02 · 0.12 · 312 B | 0.74 · 0.18 · 184 B |
| `denorm_flat` | 18.2 s · 453 MB | 120 · 573 · 6.3 MB | - | 6.75 · 11.8 · 105 kB | 1.55 · 1.95 · 23 kB | 135 · 603 · 7.3 MB | 11.8 · 61.0 · 1.2 MB |
| `denorm_part` | 23.2 s · 425 MB | 257 · 510 · 6.0 MB | - | 13.4 · 9.68 · 101 kB | 4.21 · 2.48 · 23 kB | 151 · 507 · 6.7 MB | 10.2 · 66.5 · 1.2 MB |
| `rollup` | 14.8 s · 128 MB | 311 · 1,046 · 7.7 MB | - | 0.67 · 0.13 · 323 B | 0.70 · 0.19 · 186 B | 0.70 · 0.12 · 312 B | 1.67 · 0.18 · 184 B |
| `ivm` | 19.7 s · 55 MB | 2,714 · 11,766 · 3.9 MB | - | 105 · 510 · 77 kB | 93.6 · 494 · 62 kB | 335 · 1,263 · 99 kB | 302 · 1,159 · 103 kB |
| `sift_reuse` | 7.0 s · 33 MB | 47.5 · 150 · 1.7 MB | 26.1 · 77.4 · 105 kB | 1.18 · 0.14 · 335 B | 1.41 · 0.20 · 184 B | 0.61 · 0.12 · 312 B | 1.03 · 0.19 · 184 B |
| `sift_scope` | 13.2 s · 68 MB | 31.9 · 150 · 1.7 MB | 32.5 · 187 · 315 kB | 1.00 · 0.20 · 899 B | 1.05 · 0.34 · 646 B | 0.94 · 0.15 · 584 B | 1.39 · 0.32 · 632 B |
<!-- bench:scope-stock-costs:end -->

The scope index adds 68 MB (the entity index alone 33 MB) and its writes cost a promotion and a few hundred bytes of WAL for a pair edit; the copies add 425 to 454 MB and write 6 to 7 MB of WAL for a pair grant of
the largest member, and the incremental views need 0.5 to 1.3 billion instructions for any pair edit and 11.8 billion for 10,000 inserts (pg_ivm 1.17 matches the changed groups of a view with a null-safe
`OR` that the unique index of the view cannot use beyond its first column). Updates and deletes of events, concurrent writers and a pair edit of a tenant of a read in flight were not measured for the stock arms; the copies and the
rollups need further triggers and indexes for them, so the figures above are the cheapest case for them.

## Checklist: what a scope needs, and what says so when it is missing
Each line is checked when the index is made (`CREATE INDEX`, `REINDEX`) or altered, in the style of the other messages of the extension: an `ERROR` with a hint that names the fix where the index cannot be made, a `NOTICE` where it is made and some reads run as written. A statement that holds a scope relation and is not read from the units says why in `sift.explain_why` (arm `scope`), for every form, including the ones no site of the planner names (no scan of the table is planned, the scan does not take the join, `sift.agg_scope` or `sift.enable_agg` off, the scope declared after the segments were written).

| What the scope needs | When it is not so | The fix |
|---|---|---|
| The indexed table is a heap table and the index a `sift_col` index; the member key is a column of the index of type `int2`, `int4`, `int8`, `text`, `varchar` or `uuid` (a deterministic collation) | `ERROR`: the index must be a `sift_col` index on a heap table; the column is not of one of those types (`char(n)`, `citext`, a domain, a nondeterministic collation) | index a heap table; use a column of a supported type |
| The name of the derived key is not a column of the index; `cols(..)` names columns of the index, once each | `ERROR` naming the column | rename the key; name columns of the index |
| The relation of pairs is a plain permanent heap table, not the indexed table | `ERROR`: not a plain heap table (a view, a materialized view, a partitioned or foreign table), the table of the index, a temporary table (the index would be dropped with it when the session ends) | name a permanent table of pairs; a view is read by the statements, not declared |
| Its member and scope key columns are of one kind on every path (integers, `text` with `varchar`, or `uuid`; the group column of two paths an integer), `NOT NULL` on a scope of one path | `ERROR`: of an unsupported type, nullable, the same column, kinds that differ | `ALTER TABLE .. SET NOT NULL`; change a column or the declaration |
| An immediate btree on exactly `(member, scope_key)`, in either order | `ERROR`: no btree, or only a deferrable or `INCLUDE` one; two partial ones of one kind (name the one that defines the pairs with `using`) | `CREATE UNIQUE INDEX ON pairs (member, scope_key)`; `using <index>` |
| The journal index: a `sift_col` index of the relation with `track_changes = 'ctid'`, the member first, holding the scope key, valid, without a `WHERE` (unless the btree of the pairs implies it) | `NOTICE`: none qualifies, and the statement made it (named by the rule, commented, dropped with the last scope index that uses it). `ERROR`: the role does not own the relation of pairs, and the hint has the statement for its owner | the statement of the hint, run by the owner; `CREATE INDEX CONCURRENTLY ON pairs USING sift_col (member, scope_key) WITH (track_changes = 'ctid')` first, to name it or keep the writers of the pairs running |
| A scope of two paths has the journals of its three relations (as a scope of one path has its journal) | as above, for each relation without one; the `ERROR` names the first and the hint has the statement for each | the same |
| The journal stays a journal | `ALTER INDEX journal SET (track_changes = ..)` to another value, or `RESET`, is refused while a scope index depends on it | `DROP INDEX` the scope index first |
| At most `max_multiplicity` (default `sift.scope_max_multiplicity`) entries for each row | `ERROR` with the size and the growth, after the build; `REINDEX` and the restore check the limit the option records | state `max_multiplicity(n)` in the declaration, raise the setting for a new index, or leave the table without the scope |
| `ALTER INDEX .. SET (scope = ..)`, `SET (scope = 'off')` or `RESET (scope)` on an index that has segments | `ERROR` `0A000` (`cannot change the column definitions of index ".." while it holds data`): the segments were written under the declaration the index had, and every scan of the index would stop until a `REINDEX` writes them | `CREATE INDEX CONCURRENTLY` the index with the scope (or without it, to give the scope up), then `DROP INDEX` the old one: no scan is lost |
| The scope of a partition is declared on the partition | `NOTICE`: a statement over the partitioned table reads no scope | read the partition, or leave the statement as it is |
| The relation has no row-level security that applies to the reader | `NOTICE`: a role the policies apply to reads the join as written | none needed, or a role the policies do not apply to |
| The role that builds (`CREATE INDEX`, `REINDEX`) can read the pairs: `SELECT` on the relation or on its member, scope key and predicate columns, and no row-level security that applies to it | `ERROR`: `permission denied for column .. of table ..` with the `GRANT` in the hint, or `row-level security applies to ..` | the `GRANT` shown; build with the owner of the relation or a role with `BYPASSRLS` |

`DROP INDEX` of the btree or the journal, and `DROP TABLE` of the relation, need `CASCADE` and drop the scope index with them (the dependencies are the record of which btree and journal the segments were written under).
