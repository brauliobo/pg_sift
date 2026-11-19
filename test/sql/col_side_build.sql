-- The scope sidecar of an entity segment (col/side.h): runs of the units that hold the rows of a scope key's members, with the cube of the key's rows by calendar month, made from the
-- finished segments with no copy of any row. The build of the index writes it; a second call writes nothing, is part of the segment (the index is as large as before plus the sidecar, and
-- col_verify is clean), and its cells equal the plain join: for every key, month and kind the rows and the sum are never more than the join's and, for a key whose rows all lie in runs,
-- exactly its.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET sift.enable_agg = off;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL);
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');

SELECT (SELECT count(*) FROM sift._test_col_formats('events_col') s WHERE s.format >= 53) AS segments_with_sidecar_after_create;
SELECT segments AS second_call_writes FROM sift._test_side_fill('events_col', 0, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope');
SELECT sift.col_verify('events_col', true) IS NOT NULL AS verified;

-- the cells against the join
CREATE TEMP TABLE c AS SELECT key, cell, grp AS kind, sum(rows) AS n, sum(sum) AS s FROM sift._test_side_cells('events_col', 0, 4, 5) WHERE cell <> -9223372036854775808 GROUP BY 1, 2, 3;
CREATE TEMP TABLE j AS SELECT t.tenant_id::int8 AS key, (date_trunc('month', e.created_at)::date - date '2000-01-01')::int8 AS cell, e.kind::int8 AS kind, count(*) AS n, sum(e.v)::int8 AS s
  FROM events e JOIN tenant_scope t USING (dim_a_id) GROUP BY 1, 2, 3;
CREATE TEMP TABLE covered AS SELECT a.key FROM (SELECT key, sum(n) n FROM c GROUP BY 1) a JOIN (SELECT key, sum(n) n FROM j GROUP BY 1) b USING (key) WHERE a.n = b.n;
SELECT count(*) AS cells_over_the_join FROM c LEFT JOIN j ON (c.key, c.cell, c.kind) = (j.key, j.cell, j.kind) WHERE c.n > coalesce(j.n, 0);
SELECT (SELECT count(*) FROM covered) > 0 AS some_key_fully_covered;
SELECT count(*) AS mismatched_cells_of_covered_keys
  FROM c FULL JOIN j ON (c.key, c.cell, c.kind) = (j.key, j.cell, j.kind)
 WHERE coalesce(c.key, j.key) IN (SELECT key FROM covered) AND (c.n IS DISTINCT FROM j.n OR c.s IS DISTINCT FROM j.s);

-- the runs as the superunits of a scan: the rows and the sum by kind of every key are the join's whether the runs answer (mode 1) or every unit is read (mode 0); a scan with a filter or
-- for one member reads the units, with the same rows whether or not the runs are offered (modes 2 to 5); a grouping the cubes lack splits the runs and reads their units
CREATE TEMP TABLE jk AS SELECT t.tenant_id::int8 AS key, e.kind::int8 AS grp, count(*) AS rows, sum(e.v)::int8 AS sum FROM events e JOIN tenant_scope t USING (dim_a_id) GROUP BY 1, 2;
CREATE TEMP TABLE jv AS SELECT t.tenant_id::int8 AS key, e.v::int8 AS grp, count(*) AS rows, sum(e.kind)::int8 AS sum FROM events e JOIN tenant_scope t USING (dim_a_id) GROUP BY 1, 2;
SELECT k.key, mode, (SELECT count(*) FROM (SELECT grp, rows, sum FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, mode) WHERE tag = 'g'
                                        EXCEPT ALL SELECT grp, rows, sum FROM jk WHERE key = k.key) d) AS differing_groups_by_kind
  FROM (VALUES (1), (2), (3)) k(key), (VALUES (0), (1)) m(mode) ORDER BY 1, 2;
SELECT k.key, (SELECT count(*) FROM (SELECT grp, rows, sum FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 5, 4, 1) WHERE tag = 'g'
                                    EXCEPT ALL SELECT grp, rows, sum FROM jv WHERE key = k.key) d) AS differing_groups_split
  FROM (VALUES (1), (2), (3)) k(key) ORDER BY 1;
SELECT (SELECT rows FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 1) WHERE tag = 'runs') > 0 AS key_1_answered_by_runs,
       (SELECT rows FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 1) WHERE tag = 'ref_units')
       < (SELECT rows FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 0) WHERE tag = 'ref_units') AS runs_save_units,
       (SELECT rows FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 5, 4, 1) WHERE tag = 'split_runs') > 0 AS runs_split_when_cubes_lack_the_dim;
SELECT mode / 2 AS shape, count(DISTINCT h) AS distinct_results_with_and_without_runs
  FROM (SELECT mode, md5(string_agg(grp || ':' || rows || ':' || sum, ',' ORDER BY grp)) AS h
          FROM (VALUES (2), (3), (4), (5)) m(mode),
               LATERAL (SELECT * FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, mode, 500) WHERE tag = 'g') t GROUP BY mode) r
 GROUP BY 1 ORDER BY 1;
DROP TABLE jk, jv;

-- a range of whole months of the time column is answered from the cubes of those months (mode 9), the rows of every other range from the units: the join's rows in both modes
CREATE TEMP TABLE rng AS SELECT n, a, b, (extract(epoch FROM a) * 1e6 - 946684800e6)::int8 AS lo, (extract(epoch FROM b) * 1e6 - 946684800e6)::int8 AS hi
  FROM (VALUES (1, timestamptz '2025-04-01', timestamptz '2025-05-01'), (2, '2025-03-01', '2025-06-01'), (3, '2025-04-10', '2025-05-20'), (4, '2024-01-01', '2024-02-01'), (5, '2025-01-01', '2026-01-01')) v(n, a, b);
SELECT r.n, k.key, m.mode, (SELECT count(*) FROM (SELECT grp, rows, sum FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, m.mode, 0, r.lo, r.hi) WHERE tag = 'g'
                                                  EXCEPT ALL SELECT e.kind, count(*), sum(e.v) FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key AND e.created_at >= r.a AND e.created_at < r.b GROUP BY 1) d) AS differing_groups
  FROM rng r, (VALUES (1), (2), (3)) k(key), (VALUES (8), (9)) m(mode) ORDER BY 1, 2, 3;
SELECT r.n, (SELECT rows FROM sift._test_side_agg('events_col', 0, 1, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 9, 0, r.lo, r.hi) WHERE tag = 'runs') > 0 AS answered_by_runs
  FROM rng r ORDER BY 1;
-- the ungrouped total of a measure (mode 16: rows with a value, sum, smallest and largest) from the cubes of the runs (mode 17) is the join's, for the measure and for the dim; for a range of months the
-- rows and the sum are, the smallest and largest being bounds of the run (NULL here)
SELECT k.key, a.att, m.mode,
       (SELECT ROW(grp, rows, sum) IS NOT DISTINCT FROM (SELECT ROW(min(x)::int8, count(x)::int8, sum(x)::int8) FROM (SELECT CASE a.att WHEN 5 THEN e.v ELSE e.kind END x FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key) j)
          FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, a.att, m.mode) WHERE tag = 'g') AS total_is_the_joins,
       (SELECT rows IS NOT DISTINCT FROM (SELECT max(x)::int8 FROM (SELECT CASE a.att WHEN 5 THEN e.v ELSE e.kind END x FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key) j)
          FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, a.att, m.mode) WHERE tag = 'max') AS max_is_the_joins,
       (SELECT rows FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, a.att, m.mode) WHERE tag = 'runs') > 0 AS answered_by_runs
  FROM (VALUES (1), (2), (3)) k(key), (VALUES (4), (5)) a(att), (VALUES (16), (17)) m(mode) ORDER BY 1, 2, 3;
SELECT r.n, k.key, m.mode, (SELECT ROW(rows, sum) IS NOT DISTINCT FROM (SELECT ROW(count(e.v)::int8, sum(e.v)::int8) FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key AND e.created_at >= r.a AND e.created_at < r.b)
                              FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, m.mode, 0, r.lo, r.hi) WHERE tag = 'g') AS total_is_the_joins
  FROM rng r, (VALUES (1), (2), (3)) k(key), (VALUES (24), (25)) m(mode) ORDER BY 1, 2, 3;
DROP TABLE rng;

-- pairs edited after the sidecar was written: a run whose span holds a member whose pair changed is not taken, its units are read with the members as they are now, and every answer stays the join's
-- (mode 33: the runs the journal allows, grouped by kind)
CREATE FUNCTION edited_state(label text) RETURNS TABLE (step text, key int8, groups_are_the_joins bool, runs int8) LANGUAGE sql AS $$
  SELECT label, k.key,
         (SELECT count(*) FROM (SELECT grp, rows, sum FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 33) WHERE tag = 'g'
                                EXCEPT ALL SELECT e.kind, count(*), sum(e.v) FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key GROUP BY 1) d) = 0
         AND (SELECT count(*) FROM (SELECT e.kind, count(*), sum(e.v) FROM events e JOIN tenant_scope t USING (dim_a_id) WHERE t.tenant_id = k.key GROUP BY 1
                                    EXCEPT ALL SELECT grp, rows, sum FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 33) WHERE tag = 'g') d) = 0,
         (SELECT rows FROM sift._test_side_agg('events_col', 0, k.key, 'SELECT tenant_id::int8, dim_a_id::int8 FROM tenant_scope', 4, 5, 33) WHERE tag = 'runs')
    FROM (VALUES (1), (2), (3)) k(key) ORDER BY 2
$$;
-- the rule itself: a run is taken exactly when no member whose pair of the key changed since the sidecar was written (inserted or removed) lies within its span
CREATE TEMP TABLE pairs_at_fill AS SELECT tenant_id, dim_a_id FROM tenant_scope;
CREATE FUNCTION rule_state(label text) RETURNS TABLE (step text, key int8, runs int8, valid_runs int8, differing_from_the_rule int8) LANGUAGE sql AS $$
  SELECT label, k.key, count(*), count(*) FILTER (WHERE v.valid),
         count(*) FILTER (WHERE v.valid IS DISTINCT FROM NOT EXISTS (SELECT 1 FROM ((SELECT dim_a_id FROM pairs_at_fill WHERE tenant_id = k.key EXCEPT SELECT dim_a_id FROM tenant_scope WHERE tenant_id = k.key)
                                                                                    UNION (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = k.key EXCEPT SELECT dim_a_id FROM pairs_at_fill WHERE tenant_id = k.key)) c WHERE c.dim_a_id BETWEEN v.lo AND v.hi))
    FROM (VALUES (1), (2), (3)) k(key), LATERAL sift._test_side_valid('events_col', 0, k.key) v GROUP BY k.key ORDER BY 2
$$;
SELECT * FROM rule_state('unedited');
SELECT * FROM edited_state('unedited');
DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (1, 60);
SELECT * FROM edited_state('dim_b at the ends of the spans removed from key 1');
SELECT * FROM rule_state('dim_b at the ends of the spans removed from key 1');
INSERT INTO tenant_scope VALUES (5, 1), (11, 1);
SELECT * FROM edited_state('pairs added to key 1');
SELECT * FROM rule_state('pairs added to key 1');
DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (7, 8, 31);
SELECT * FROM edited_state('pairs removed from key 1');
SELECT * FROM rule_state('pairs removed from key 1');
INSERT INTO tenant_scope VALUES (2, 2);
DELETE FROM tenant_scope WHERE tenant_id = 2 AND dim_a_id = 18;
INSERT INTO tenant_scope VALUES (20, 3);
UPDATE tenant_scope SET tenant_id = 2 WHERE tenant_id = 3 AND dim_a_id = 8;
SELECT * FROM edited_state('pairs of keys 2 and 3 edited');
SELECT * FROM rule_state('pairs of keys 2 and 3 edited');
DELETE FROM tenant_scope WHERE tenant_id = 2;
SELECT * FROM edited_state('key 2 emptied');
SELECT * FROM rule_state('key 2 emptied');
-- a journal made again no longer vouches for the stamp of the sidecar: no run of it is taken, whichever members changed, and the answers are still the join's
REINDEX INDEX tenant_scope_journal;
SELECT * FROM edited_state('journal made again');
SELECT k.key, count(*) AS runs, count(*) FILTER (WHERE v.valid) AS valid_runs FROM (VALUES (1), (2), (3)) k(key), LATERAL sift._test_side_valid('events_col', 0, k.key) v GROUP BY k.key ORDER BY 1;
DROP FUNCTION edited_state(text);
DROP FUNCTION rule_state(text);

DROP TABLE c, j, covered, events, tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
