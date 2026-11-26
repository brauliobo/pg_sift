-- DISTINCT ON in the aggregate scan with the values it could not keep before: an array column, a column of a joined table as the value or a sort key, and groups
-- that do not fit hash memory (the groups spill to partitions and the best row of each is merged). Each statement runs with the pushdown and with
-- sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SET sift.async_maintenance = off;

CREATE TABLE dk (id int PRIMARY KEY, g int, h int, v numeric(10, 2), t text, ia int[], w int);
INSERT INTO dk
SELECT i, i % 30011, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 37 % 1000) / 10.0 END, 'txt' || (i % 977),
	CASE WHEN i % 11 = 0 THEN NULL ELSE ARRAY[i % 97, i % 5] END, i % 211
FROM generate_series(1, 90000) i;
CREATE TABLE dw (id int PRIMARY KEY, name text, kind text);
INSERT INTO dw SELECT i, 'n' || (i * 7 % 211), (ARRAY['a', 'b', 'c'])[1 + i % 3] FROM generate_series(0, 210) i;
CREATE INDEX dk_col ON dk USING sift_col (id, g, h, v, t, ia, w);
VACUUM (ANALYZE) dk;
ANALYZE dw;

CREATE FUNCTION dk_same(q text) RETURNS TABLE (pushed boolean, spilled boolean, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE plan text := ''; line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	pushed := plan ~ 'Custom Scan \(SiftAgg';
	spilled := plan ~ 'Spilled Rows: [1-9]';
	EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
	RESET sift.enable_agg;
	SELECT count(*) INTO result_rows FROM r_on;
	EXECUTE 'SELECT count(*) FROM ((SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) UNION ALL (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on)) d' INTO vs_normal_plan;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- an array value
SELECT * FROM dk_same($$SELECT DISTINCT ON (h) h, id, ia FROM dk ORDER BY h, v DESC NULLS LAST, id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (w) w, id, ia, t FROM dk WHERE w < 100 ORDER BY w, id DESC$$);
-- a column of a joined table: as a value, as a sort key, both, as the ON column
SELECT * FROM dk_same($$SELECT DISTINCT ON (o.h) o.h, o.id, s.name FROM dk o JOIN dw s ON s.id = o.w ORDER BY o.h, o.v DESC NULLS LAST, o.id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (o.h) o.h, o.id FROM dk o JOIN dw s ON s.id = o.w ORDER BY o.h, s.name DESC, o.id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (o.h) o.h, o.id, s.kind FROM dk o JOIN dw s ON s.id = o.w ORDER BY o.h, s.name, o.id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (s.kind) s.kind, o.id, o.v FROM dk o JOIN dw s ON s.id = o.w ORDER BY s.kind, o.v DESC NULLS LAST, o.id$$);
-- groups past hash memory: 30,011 groups under 64 kB spill, the best row of a group is merged from its partial states
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET sift.agg_force = on;
SELECT * FROM dk_same($$SELECT DISTINCT ON (g) g, id, v, t FROM dk ORDER BY g, v DESC NULLS LAST, id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (g) g, id, v, t FROM dk ORDER BY g, v, t DESC, id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (id) id, g, v, t FROM dk WHERE id % 2 = 0 ORDER BY id$$);
SELECT * FROM dk_same($$SELECT DISTINCT ON (g) g, id, ia FROM dk ORDER BY g, id$$);
RESET sift.agg_force;
RESET hash_mem_multiplier;
RESET work_mem;

DROP TABLE dk;
DROP TABLE dw;
DROP FUNCTION dk_same(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
