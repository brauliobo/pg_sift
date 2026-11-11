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
A scope may also be the union of this relation and a relation joined through a second one ([two paths](#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant)).

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

## Pairs reached by two paths: a direct grant and a group grant

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

**The three unique btrees.** Each relation has its own recorded unique btree on exactly its two columns, with the rule of the pairs of one relation (above): `CREATE INDEX` and `REINDEX` choose it (the
only partial one, else the lowest oid of the plain ones; two partial ones are an error that asks for `using`) and say which, one `NOTICE` for each path (`the direct pairs of "grants" (dim_a_id,
tenant_id) are the rows of its unique btree "grants_direct" that satisfy (deleted_at IS NULL)`, then `the joined pairs` and `the via members`); `ALTER INDEX` is sticky (another `using` than the
recorded one is refused); `DROP INDEX` of a recorded btree is refused without `CASCADE`, and `REINDEX .. CONCURRENTLY` of it keeps the dependency. The dependencies are told apart by the relation
and its two columns, so one scope index depends on up to three btrees of the same relation or of several, and an index may hold a scope of two paths beside scopes of one. A scope of one path declared as before is unchanged.

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
recorded btree implies (the rule of the journal index above): J1 on `rel` is `(member, key)`, J2 on `rel2` is `(via, key)`, J3 on `vrel` is `(member, via)`. `CREATE INDEX` of a scope of two paths does
not require them: a scope with any of the three missing, or one a pair can be inserted outside of (a partial journal), vouches for nothing, the plan says `Scope Fallback: no journal`, and the read is the
entity rows of the members the three relations list for the keys. The scope index finds the journals when it is created or rebuilt and depends on them: a journal made later is found by the next
`REINDEX` of the scope index, which starts it (the reads say `unfilled` until the `REINDEX` after that stamps the segments). A journal rebuilt (`REINDEX` of it) is another lineage: the reads say
`other lineage` until the scope index is rebuilt, and the maintenance sees the segments as stale. Each journal has its own stamp.

**The members that changed.** A segment of a scope of two paths holds a stamp for each of the three journals. The members whose pairs changed since the stamps are the union of the members of J1, the members of J3
(the ones that left a group are there), and the members of every group in J2 as the recorded btree of `vrel` lists them for the reader's snapshot (the active rows, the predicate applied). The result is exact: the pairs of
a member depend on no other row (the three lists above), a row of one of them that changed names the member, or the group whose members are looked up now, in a journal; a member that left a group is in
J3. The read is the one of one path: the units of the keys less the changed members, plus the entity rows of those that have a pair with a key now (the patch), and the write buffer's
members the same way. `sift.scope_patch_keys` bounds the changed members (a group with more members than it is `too many changed keys`: the entity rows are read), and the refresh of the maintenance
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

**`sift.in_scope` and the planner.** The scan takes the conjunct only from the marker `sift.in_scope(index, key_name, fk, key or keys)` (`sift.in_scope_pairs` for the join meaning), as for one path. The planner's recognition of the forms of the `UNION` (`fk IN (SELECT member FROM <the union> WHERE key = k)`, `EXISTS`, the join) is the work of the
planner, which reads the paths from `sift_scope_legs(index, scope, legs[2])`: it returns 2 legs for this declaration, the second one joined (`via_rel`, `via_join`, `via_member`, `via_btree`, `via_pred`, the
`via` column of `rel2` in `member`). Until it lands a release build has no statement that reads these units except the markers.

**What stays impossible.** A relation that is a view, a CTE or a function (the three are heap tables with a unique btree); a condition on a third table (a group's own soft delete: the memberships of a deleted group
stay pairs; put the condition on the memberships or use the copy path of [INDEX-RECIPES](INDEX-RECIPES.md#keeping-the-pairs-from-a-view)); a predicate that depends on time (`now()` is refused in an index predicate: sweep it
into a column, see above); a partitioned table among the three; text or uuid keys; deny rows (a row that removes a pair another path gives); three or more hops, or more than one joined path.

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
estimated costs of the two plans choose (and, at execution, the real keys: below): the scope read leaves the share of the rows that the pairs of the keys own (`pg_stats` of the key column of the relation, the most common keys by their
own frequency) of the distinct members of the table, where the join's key filter on the member column leaves the units that hold a member of the keys, most of them for facts ordered by time. The
rows are those of the join in every case; whether the index may be read at execution is the reader's verdict (`Scope Fallback`).

| form | counts a row | recognised |
|---|---|---|
| `JOIN scope_rel s ON s.member = f.fk WHERE s.scope_key = k` (also `IN (k1, k2)` with one distinct key) | once for each pair | yes |
| `f.fk IN (SELECT member FROM scope_rel WHERE scope_key = k or IN (list))`, `EXISTS`, a CTE of the members, a set-returning SQL function of them | once | yes, any number of constant keys |
| the same through a view, a security barrier view that projects the two columns, an inlined `STABLE` SQL function | as written | yes |
| `JOIN` with several keys (`= ANY ($1)`, `IN (k1, k2)`, a table of keys) | once for each pair | yes, where the plan holds the join as written (below): the node runs it where the units cannot count a row for each pair, or cost more |
| `JOIN scope_rel s ON s.member = f.fk GROUP BY s.scope_key` (every key: with other group columns, aggregates, `HAVING`, `ORDER BY` the key, a top-N over the groups, `count(DISTINCT ..)`; the key of type `int2`, `int4` or `int8` is the group column's type) | once for each pair | yes: every key is read as the derived leaf `<key_name>#scope`, and `WHERE s.scope_key = ANY (..)` with the key grouped by is the same read of those keys (below) |
| the key in an expression, an aggregate of it, `ROLLUP` / `CUBE` / grouping sets or `DISTINCT ON` over it, another column of the relation read, a range of keys, `GROUP BY` the key with a `LEFT JOIN` | | no |
| the scope key bound by a parameter: `= $1`, `= ANY ($1)`, `IN ($1, $2)`, a stable expression (`current_setting(..)::int`), the output of an InitPlan (`= (SELECT ..)`, `= ANY (ARRAY(SELECT ..))`) | as written | yes: the keys are those of the execution (below); a join (a row for each pair) only for a scalar, an array may hold several keys |
| a table of keys: `s.key IN (SELECT key FROM keys WHERE ..)`, `EXISTS`, `JOIN keys k ON k.key = s.key` | as written | yes, read once when the scan starts (below); a join (a row for each pair) only where the table gives one key: a unique index on the key column with the other columns bound, or a `DISTINCT` sub-select |
| the conditions of the relation that are the predicate of its partial unique btree (`AND s.deleted_at IS NULL`, in the `WHERE`, an `ON`, a plain view or a sub-select), stated as the btree has it | as written | yes: the pairs are the rows that satisfy it |
| a statement that does not state the predicate of the btree (the soft deleted rows are rows of the join and no pairs of the units), another condition on the relation, a join to a relation of a scope of two paths (its pairs are the union of the direct grants and those of the groups, the rows of none of them), a view with a condition of its own (`security_barrier`), `DISTINCT` or a join inside the sub-select, `NOT EXISTS`, an outer join, row-level security on the relation | | no, and `sift.explain_why` says which |

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
a user of the few largest tenants of a skewed set gets a plan priced for average ones: on 300,000 events and 50 tenants the scope read of one key runs in 2.1 million instructions against 7 to 20 million, 3 small tenants 5.3
against 7.6, the 10 largest 37.8 against 16.6, and 30 keys keep the join.

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
every pair of every tenant, 30 keys: scope forced 501 M, node 26.8 M (join 17.6; the reader lists the changed members before it refuses). The join's price follows within 8% from 3 keys on and 37% at the first key where the
units of the keys leave some of the table's units out; the units' price is the weaker half (the aggregation of a row and the units' own setup differ with the shape: the node takes the join up to 0.9 M instructions (0.2 ms) late or early in
the middle of the range, where the two cost the same, and 3 M where a key has many members of few rows). A statement that is nearly one keeps the join and says why in `sift.explain_why` (arm `scope`, slug `scope`). `SET sift.agg_scope = off` turns the recognition off; a top-N, a `GROUP BY` with an
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
Each read counts what its two routes would cost and takes the cheaper, from the figures of `src/col/scope.h` (instructions of the backend, 300,000 events, 5,000 keys of the facts and 50 tenants, warm, median of 5): the scope read
before any row (1,400,000: the journal's vouch and changed keys, the directory, the scope units opened), 20 an entry of the keys, 196,000 a unit of the table segments the rows of the changed keys are read from, 10 a row looked at to
find them when no point directory names them, 1,700 a copy of the delta (a changed key's row or a buffered row, copied for each pair, made a unit and read), against the entity path (the keys the relation lists for the tenants, read
from the leaves of its journal index at 25 a pair, 10 a table row the key filter tests and 35 a row it keeps), which says `Scope Fallback: patch dearer than the entity path`. The prices are pure functions of the counts of a
`SiftScopeCounts` (`sift_scope_patch_price`, `sift_scope_units_price`, `sift_scope_entity_price` and `sift_scope_entity_cheaper`: the keys and `once`, the entries of the units, the units and rows the patch reads, the delta rows, the
pairs and the table rows of the listed keys), so the executor (from the zones of the units it read) and the planner (from its estimates) decide from the same inputs. A read of several keys counted once adds 15 instructions an entry for each key whose rows a drop list
thins (`Scope Dedupe Keys`). A patch and a delta that cost less than pricing them (1,000,000: listing the keys and counting their rows) are read without it. The changed keys one read lists are bounded by the table rows of the index at 21 over
5,600 for a changed key's probe: more read the entity path (`too many changed keys`). The setting `sift.scope_patch_keys` is gone; the testing setting `sift.test_scope_patch` takes the patch whatever it costs.
Measured on the fixture above (a read of one tenant; the join 3.24 million clean, 3.63 and 4.16 with 20 and 200 changed pairs): the clean read costs 1.54 million, with 20 changed pairs the read says why and runs the entity path
(5.3 million: the join's 3.6 and 1.7 million of decision, the journal's vouch, the changed keys, the pairs of the changed keys and the listing of the tenant's keys) where the patch forced costs 8.2 million (5.2 with a point directory
of the key column), with 200 changed pairs 7.2 million (the join 4.2; the patch forced 27.0). A key cut index reads only the units of the keys it patches. The delta has a unit of its own for each key: 2,000 buffered rows cost 2.05 million for one tenant (110 blocks hit, none read; join 3.41).
The reader's part of the decision (`sift_scope_read`: the stamps, the vouch of the journal, the changed keys, the pairs of the changed keys, the plan of the table rows) costs 0.94 million at 20 changed pairs; the reads of every key
list the pairs of the relation from the leaves of its journal index (0.37 million for the 7,000 pairs of the fixture, where the relation read through its btree costs 3 and had made that read cost 9.4 million with 2,000 buffered rows).

**The delta is cheap to expand.** The copies of a read are sorted by (key, address) and the pairs by (key of the fact, tenant): a radix sort of the entries (a few passes over the key ranges) replaces the comparison sort (a call of a
function for each of some twenty steps of each entry), which were more than half of the read of 2,000 buffered rows for every tenant: 12.3 million instructions (2,190 copies, 4,730 each over the clean read's 1.94) became 7.1 (2,330 a copy; the decode of the buffers is the same
for the join). The figure of a copy in the price is 1,700, measured on a read of one key whose delta is one unit.

**Every key, grouped by the key.** A read of every key (`Scope Keys: all`, no key list, `Scope Semantics: per pair`) reads all the scope units, a fact once for each pair of its member, and its group key is the derived leaf
`<key_name>#scope`: every entry has its key (`Scope Group Key: <key_name>`), so the scan groups by it as the engine groups by any leaf. The units of a key hold that one key (the units of small keys share a unit, a run of keys), so a
unit with no member to leave out answers from its pre-aggregates, histograms and sums of its entries, with no hash and no row read (`Units From Pre-aggregates`); the gate that kept a read of several keys from them was written for the dedupe of `IN` and `EXISTS`, which a read per pair does not
have. The rest is as for one key: the stored entries of the keys that changed leave at the source and their rows are read once from the table segments and expanded by the pairs they have now (above), and the rows of the write
buffers are the delta, a copy for each pair of their dim_a_id. The planner chooses between the read and the join at the start of the scan from the price of one function (`sift_scope_entity_cheaper`, with the counts of the read: a
read of every key has no entity path of its own, so only the planner can take the join). The planner recognises the join: `JOIN scope_rel s ON s.member = f.fk GROUP BY s.key` (tests `agg_scope_group_key`, `col_scope_groupkey`, `col_scope_changed_pairs`, `agg_scope_choice`) is planned as written and again with the relation taken out, and the node holds the join as written, which it runs where it costs less (below), so the reader's refusal of a read that counts a row for each pair never raises. Measured on 300,000 events,
5,000 dim_a_ids and 50 tenants (instructions of the backend, warm, median of 5; `sum(amount), count(*)` for every tenant): the read costs 1.99 million instructions (86 units from pre-aggregates, 409 blocks hit; the join 53.0 million, 206 hit),
with 2,000 buffered rows 7.1 million (2,190 copies; the join 53.6; 12.3 before the copies were sorted by radix), with 20 changed pairs 39.4 million (a unit with a changed key's entries is read row by row; the join 56.2; the
reader refused it after 0.94 million instructions, which the join added to its 56.7) and with 200 changed pairs 89.5 million (the join 56.6: the delta's 25,140 copies cost more than the join, which the planner will choose).
The node chooses by itself: a read grouped by the key answers the units that hold no changed member from their pre-aggregates and reads the units of the changed members row by row (143 instructions an entry, 8.5 million for the patch and the delta's setup, the share of the entries in such units 1 - exp(-1.8 changed / keys)), and the join as written costs 110 instructions a row of the keys beyond the first more than the model's operators count (`AGG_ALT_JOIN`). Clean, with 20 and with 200 changed pairs of the same table: the node runs the scope read in 1.4 million instructions (the join 53.0), the scope read in 38.9 (the join 56.0), and the join in 58.2 (`Scope Fallback: join is cheaper for every key`; the scope read forced costs 90.8).

## EXPLAIN

`Scope Index`, `Scope Keys` (`all` for every key), `Scope Group Key` (a group key of the scan is the derived key), `Scope Semantics`, `Scope Columns` (kept of all), and when the units were read `Scope Units` (read and skipped), `Scope Entries Read`, `Scope Changed Keys`, `Scope Patched
Keys`, `Scope Patched Rows` (the rows of the entity segments read for the changed keys that have a pair now), `Scope Delta Rows` (the copies of the rows of the write buffers and of the patched rows the scan reads, one for each pair), `Scope Dedupe Keys`; with a fallback `Scope Fallback: <why>` and `Scope Members Listed`. The reasons (the labels are in [COL-PLAN](COL-PLAN.md)): `disabled`,
`no scope index`, `column .. not in the scope units`, `unfilled`, `several stamps`, `scope segments do not cover the rows`, `write buffer` (the pairs of the buffered rows are more than work_mem holds), `top-n, changed pairs` (a top-N cannot take the patch or the delta of a changed pair),
`dedupe over work_mem`, `dedupe keys`, `patch dearer than the entity path`, `join is cheaper for N keys` (the node ran the join as written), and the words of the journal: `no journal`, `log not complete`, `other lineage` (the relation's journal index was rebuilt or truncated: the next `REINDEX` of
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

EXPERIMENTAL; one key or a list of keys (constants or parameters) per conjunct, no scope of a partitioned table of pairs (the relations of a scope of two paths included); at most one joined path, no third hop; text keys through a dictionary table; a top-N over changed members reads the entity
rows; several keys with a per-pair meaning (a join, not `IN` or `EXISTS`) keep the join, whatever an array parameter holds, and so does every key by the key when a changed member has a pair now; a join to a table of keys that may hold a key twice and the planner's recognition of a `GROUP BY` the key are not in this release; at most 4 scopes and 8 connections plus scopes on an index; a library before 5.0 refuses the index. The reserved
by-reference unit kind (bits 8 to 15 of the scope stamp, directory flag 0x0010) is never written by 5.0: a library that finds one refuses the segment ("Use a newer pg_sift"), and the entity
rows still answer.
