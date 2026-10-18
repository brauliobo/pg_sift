-- The column source of sift_colstub: per unit dictionaries (codes in first-seen order, not comparable between units), zone
-- prune, counters, bounded memory and the statistics of the planner.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- about six superblocks of 256 pages; the id grows with the position in the heap
CREATE TABLE u (id int, b int8, d date, f float8, seg text, kind text, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO u
SELECT i,
	   CASE WHEN i <= 6000 THEN NULL ELSE i * 10000000000::int8 END,
	   date '2024-01-01' + i / 100,
	   i / 10.0,
	   -- the first value of every unit differs: 'a' + (the position) and then the three others in an order that depends on it
	   chr(97 + (i / 2500 + i % 3) % 4),
	   'k' || (i % 5),
	   NULL
FROM generate_series(1, 16000) i;
CREATE INDEX u_col ON u USING sift_colstub (id, b, d, f, seg, kind);
ANALYZE u;
SELECT count(*) AS units_total FROM sift._test_aggstub_units('u_col') \gset

-- dictionaries: one per unit and text leaf, with an id of its own; a value has one code in a unit, which differs between units
SELECT count(*) = :units_total AS one_per_unit, count(DISTINCT id) = count(*) AS ids_are_unique
FROM sift._test_aggstub_dicts('u_col', 4);
SELECT count(*) = :units_total * 2 AS two_text_leaves_per_unit, count(DISTINCT id) = count(*) AS ids_unique_over_leaves
FROM sift._test_aggstub_dicts('u_col');
SELECT unit, n, nbytes FROM sift._test_aggstub_dicts('u_col', 4) ORDER BY unit;
SELECT unit, n, nbytes FROM sift._test_aggstub_dicts('u_col', 5) WHERE unit < 2 ORDER BY unit;
-- dense codes, assigned in the order the values are first seen in the unit
SELECT count(*) AS codes_not_first_seen FROM (
	SELECT code, coalesce(max(code) OVER (PARTITION BY unit ORDER BY ord ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), -1) AS before
	FROM sift._test_aggstub_dump('u_col', 4)) x WHERE code > before + 1;
SELECT count(*) AS value_with_two_codes_in_a_unit FROM (
	SELECT unit, val FROM sift._test_aggstub_dump('u_col', 4) GROUP BY unit, val HAVING count(DISTINCT code) > 1) x;
SELECT count(*) AS code_with_two_values_in_a_unit FROM (
	SELECT unit, code FROM sift._test_aggstub_dump('u_col', 4) GROUP BY unit, code HAVING count(DISTINCT val) > 1) x;
-- ... and the same value has different codes in different units
SELECT val, count(DISTINCT code) > 1 AS codes_differ_between_units, array_agg(code ORDER BY unit) AS code_per_unit
FROM (SELECT DISTINCT unit, val, code FROM sift._test_aggstub_dump('u_col', 4) WHERE val IS NOT NULL) d GROUP BY val ORDER BY val;
SELECT unit, (array_agg(val ORDER BY ord))[1] AS first_value, (array_agg(code ORDER BY ord))[1] AS its_code
FROM sift._test_aggstub_dump('u_col', 4) GROUP BY unit ORDER BY unit;

-- zone prune: a unit with no value of the leaf in [lo, hi] is skipped (the ids of a unit are a range)
SELECT array_agg(sb ORDER BY sb) AS all_sbs FROM sift._test_aggstub_units('u_col');
SELECT min(id)::int AS lo2, max(id)::int AS hi2 FROM (SELECT val::int AS id FROM sift._test_aggstub_dump('u_col', 0, 2, 3)) x \gset
SELECT array_agg(sb ORDER BY sb) AS sbs_of_sb2 FROM sift._test_aggstub_units('u_col', 0, NULL, 0, :lo2, :hi2);
SELECT array_agg(sb ORDER BY sb) AS sbs_around_sb2 FROM sift._test_aggstub_units('u_col', 0, NULL, 0, :lo2 - 1, :hi2 + 1);
SELECT array_agg(sb ORDER BY sb) AS sbs_edge_low FROM sift._test_aggstub_units('u_col', 0, NULL, 0, :lo2 - 100, :lo2);
SELECT array_agg(sb ORDER BY sb) AS sbs_edge_high FROM sift._test_aggstub_units('u_col', 0, NULL, 0, :hi2, :hi2 + 100);
SELECT count(*) AS none_in_range FROM sift._test_aggstub_units('u_col', 0, NULL, 0, 100000, 200000);
SELECT count(*) AS empty_range FROM sift._test_aggstub_units('u_col', 0, NULL, 0, 5, 4);
SELECT count(*) AS all_of_them FROM sift._test_aggstub_units('u_col', 0, NULL, 0, -1000, 1000000);
-- an int8 leaf with NULLs: the units with only NULLs hold no value in any range
SELECT count(*) < :units_total AS units_with_values_fewer, count(*) > 0 AS units_with_values FROM sift._test_aggstub_units('u_col', 0, NULL, 1, -9223372036854775808, 9223372036854775807);
SELECT count(*) = (SELECT count(DISTINCT unit) FROM sift._test_aggstub_dump('u_col', 1) WHERE val IS NOT NULL) AS as_the_dump
FROM sift._test_aggstub_units('u_col', 0, NULL, 1, -9223372036854775808, 9223372036854775807);
-- a date leaf (int32 days)
SELECT count(*) AS units_in_the_date_range FROM sift._test_aggstub_units('u_col', 0, NULL, 2, (date '2024-02-01' - date '2000-01-01'), (date '2024-02-15' - date '2000-01-01'));
-- a leaf that is not an integer cannot prune: every unit comes
SELECT count(*) AS float_leaf_no_prune FROM sift._test_aggstub_units('u_col', 0, NULL, 3, 0, 0);
SELECT count(*) AS text_leaf_no_prune FROM sift._test_aggstub_units('u_col', 0, NULL, 4, 0, 0);

-- counters: units returned, units skipped by the zone, rows of the units returned, bytes decoded
SELECT units = :units_total AS units, units_pruned, rows = (SELECT count(*) FROM u) AS rows, bytes > 0 AS bytes
FROM sift._test_aggstub_counters('u_col');
SELECT units, units_pruned, rows = (SELECT count(*) FROM sift._test_aggstub_dump('u_col', 0, 2, 3)) AS rows_of_sb2, bytes > 0 AS bytes
FROM sift._test_aggstub_counters('u_col', 0, NULL, 0, :lo2, :hi2);
SELECT units, units_pruned, rows, bytes > 0 AS bytes FROM sift._test_aggstub_counters('u_col', 0, NULL, 0, 100000, 200000);

-- memory is bounded by one unit: reading all units holds about what reading one holds
SELECT units AS units_one, peak_bytes AS peak_one, max_rows AS rows_one FROM sift._test_aggstub_memory('u_col', 1, 2) \gset
SELECT units AS units_all, peak_bytes AS peak_all FROM sift._test_aggstub_memory('u_col') \gset
SELECT :units_one = 1 AS one_unit, :units_all = :units_total AS all_units, :peak_all < :peak_one * 1.5 AS bounded,
	   :peak_one / :rows_one < 2000 AS bytes_per_row_small;

-- statistics of the planner: pg_class plus a sample of two units
SELECT leaf, nrows::int AS nrows, nunits::int AS nunits, allvisible_frac, ndistinct::int AS ndistinct, min::numeric(30, 0) AS min, max::numeric(30, 0) AS max
FROM sift._test_aggstub_stats('u_col') ORDER BY leaf;
-- before ANALYZE there is no reltuples: estimated from the sample
CREATE TABLE u2 (id int, seg text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO u2 SELECT i, 'x' || i % 3 FROM generate_series(1, 4000) i;
CREATE INDEX u2_col ON u2 USING sift_colstub (id, seg);
SELECT (SELECT reltuples FROM pg_class WHERE relname = 'u2') AS reltuples;
SELECT leaf, abs(nrows - 4000) < 400 AS nrows_close, nunits::int, ndistinct::int, min, max FROM sift._test_aggstub_stats('u2_col') ORDER BY leaf;
VACUUM u2;
SELECT leaf, nrows::int, allvisible_frac FROM sift._test_aggstub_stats('u2_col') ORDER BY leaf;
-- an empty table, a table of NULLs
CREATE TABLE u3 (id int, seg text, js jsonb);
CREATE INDEX u3_col ON u3 USING sift_colstub (id, seg, js) WITH (paths = 'js: k int4, a[]{x text}');
SELECT leaf, nrows, nunits, ndistinct, min, max, elems_per_row FROM sift._test_aggstub_stats('u3_col') ORDER BY leaf;
SELECT count(*) AS units FROM sift._test_aggstub_units('u3_col');
INSERT INTO u3 SELECT NULL, NULL, NULL FROM generate_series(1, 10);
SELECT leaf, nrows, nunits, ndistinct, min, max, elems_per_row FROM sift._test_aggstub_stats('u3_col') ORDER BY leaf;
SELECT count(*) AS units, sum(nrow) AS rows FROM sift._test_aggstub_units('u3_col');

DROP TABLE u, u2, u3;
