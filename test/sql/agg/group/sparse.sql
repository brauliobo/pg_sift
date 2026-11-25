-- A scan that groups by several keys starts with room for four ids of each and doubles a key's room as its ids come; the arrays of the groups are
-- indexed by the keys' ids together, so the room is the product of the keys' rooms, not the groups the data makes. Keys that follow one another
-- (a date and the period and week it belongs to) make a few groups in a product of hundreds of thousands, and every doubling copied and zeroed the
-- arrays of all of it. Past 4,096 slots, when the planner expects fewer than a quarter of them groups, the arrays go to the hash table from the start (seven keys start with 4^7 slots) or as soon as fewer than a sixteenth of the slots hold one. When most rows open a group of their own the table is the hash table from the start, sized for the groups the planner expects. The result is the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE sp_facts (id int, parent_id int, ts date, wk int, dow int, quarter int, amount float8) WITH (autovacuum_enabled = false);
INSERT INTO sp_facts SELECT i, i % 50, d, extract(week FROM d)::int, extract(dow FROM d)::int, extract(month FROM d)::int / 4, i % 17 + 0.5
FROM (SELECT i, date '2024-01-01' + i % 90 AS d FROM generate_series(1, 40000) i) g;
CREATE STATISTICS sp_facts_days (ndistinct) ON ts, wk, dow FROM sp_facts;
CREATE INDEX sp_facts_col ON sp_facts USING sift_col (id, parent_id, ts, wk, dow, quarter, amount);
VACUUM (FREEZE, ANALYZE) sp_facts;

CREATE FUNCTION same_rows(q text) RETURNS TABLE (scan text, rows_pushed bigint, differences bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_scan_kind_rows_and_differences(q) $$;

-- three keys that follow the day: 90 groups (the statistics say so) in 128 * 16 * 8 slots
SELECT * FROM same_rows('SELECT ts, wk, dow, count(*) AS n, sum(amount) AS s, count(DISTINCT parent_id) AS p FROM sp_facts GROUP BY ts, wk, dow');
-- the same with a key that varies on its own: the groups fill the slots they are given
SELECT * FROM same_rows('SELECT ts, parent_id, dow, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY ts, parent_id, dow');
-- seven keys: 16,384 slots at the start, the 90 groups of the days
SELECT * FROM same_rows('SELECT ts, wk, dow, quarter, wk + 1 AS w1, dow + 1 AS d1, quarter + 1 AS q1, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY 1, 2, 3, 4, 5, 6, 7');
-- a key whose room doubles and no other key changes (one key of 40,000 groups, a key of few values before it): the groups keep their numbers
SELECT * FROM same_rows('SELECT id, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY id');
SELECT * FROM same_rows('SELECT dow, id, count(*) AS n, sum(amount) AS s, count(DISTINCT parent_id) AS p FROM sp_facts GROUP BY dow, id');

-- most rows open a group of their own (the planner expects fewer than two rows for each group, more than 4,096 groups): the hash table from the start, with room for them
SELECT * FROM same_rows('SELECT id, parent_id, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY id, parent_id');
SELECT * FROM same_rows('SELECT parent_id, id, ts, wk, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY parent_id, id, ts, wk');
-- NULL in a key, and the groups of a key a filter leaves a few of
CREATE TABLE sp_nulls (a int, b int, c text, v int) WITH (autovacuum_enabled = false);
INSERT INTO sp_nulls SELECT CASE WHEN i % 7 = 0 THEN NULL ELSE i END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 9000 END, CASE WHEN i % 13 = 0 THEN NULL ELSE 'k' || i % 5000 END, i % 100 FROM generate_series(1, 30000) i;
CREATE INDEX sp_nulls_col ON sp_nulls USING sift_col (a, b, c, v);
VACUUM (FREEZE, ANALYZE) sp_nulls;
SELECT * FROM same_rows('SELECT a, b, c, count(*) AS n, sum(v) AS s FROM sp_nulls GROUP BY a, b, c');
SELECT * FROM same_rows('SELECT a, b, count(*) AS n, sum(v) AS s FROM sp_nulls WHERE a > 29000 OR a IS NULL GROUP BY a, b');
-- rows that repeat the groups (more than two rows for each) keep the direct arrays
SELECT * FROM same_rows('SELECT id % 5000 AS a, parent_id, count(*) AS n, sum(amount) AS s FROM sp_facts GROUP BY id % 5000, parent_id');

DROP FUNCTION same_rows(text);
DROP TABLE sp_facts, sp_nulls;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
