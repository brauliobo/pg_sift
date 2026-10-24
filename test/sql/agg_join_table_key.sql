-- A key of the joined table that is an expression of its columns (`dims.at::date = facts.ts`, a cast of a column) is a hash key of the lookup, evaluated for
-- each row loaded, as a column is; without it every row of the table was tested for every distinct input of the scan. An expression that can raise or is not
-- immutable stays a condition of the rows. Every query runs as the scan and as the normal plan, and the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE facts (id int PRIMARY KEY, k int, ts date, amount numeric, val float8) WITH (autovacuum_enabled = false);
INSERT INTO facts
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE i % 7 END, CASE WHEN i % 13 = 0 THEN NULL ELSE date '2025-01-01' + (i / 3) % 8 END, i % 5 + 0.5, i % 11
FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, k, ts, amount, val);
VACUUM (FREEZE, ANALYZE) facts;

-- days of the dimension are timestamps: several in one day, a NULL, one no fact has, 'infinity'; the small and wide integers repeat and have a NULL
CREATE TABLE dims (at timestamp, n2 smallint, n4 int, n8 bigint, label text) WITH (autovacuum_enabled = false);
INSERT INTO dims VALUES
  ('2025-01-01 00:00', 1, 1, 1, 'a'), ('2025-01-01 23:59:59', 1, 2, 2, 'b'), ('2025-01-02 12:00', 2, 3, 3, 'c'), ('2025-01-02 12:00', 2, 3, 3, 'd'),
  ('2025-01-04 08:30', 3, 4, 4, 'e'), (NULL, NULL, NULL, NULL, 'f'), ('2030-06-01 00:00', 9, 9, 9, 'g'), ('infinity', 6, 6, 6, 'h'), ('2025-01-08 01:00', 0, 0, 0, 'i');
VACUUM (FREEZE, ANALYZE) dims;

\ir test/agg_helpers.sql

-- the rows of the scan and of the normal plan are the same, and the plan is the scan's
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN d = 0 AND n > 0 AND pushed(q);
END $$;

CREATE FUNCTION report(spine_first bool, jointype text, cond text, keys text DEFAULT 'd.label') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, count(*) AS n, count(f.id) AS facts, count(DISTINCT f.id) AS ids, sum(f.amount) AS amount, sum(f.val) AS val
	                  FROM %s GROUP BY %s$q$, keys,
	              CASE WHEN spine_first THEN format('dims d %s facts f ON %s', jointype, cond) ELSE format('facts f %s dims d ON %s', jointype, cond) END, keys)
$$;

-- each ON clause as the preserved dimension (dims LEFT JOIN facts), as a lookup the facts keep their rows of (facts LEFT JOIN dims) and as an inner join
CREATE FUNCTION each_join(cond text, keys text DEFAULT 'd.label') RETURNS TABLE (shape text, ok bool) LANGUAGE sql AS $$
	SELECT 'preserved', same(report(true, 'LEFT JOIN', cond, keys))
	UNION ALL SELECT 'lookup', same(report(false, 'LEFT JOIN', cond, keys || ', f.ts'))
	UNION ALL SELECT 'inner', same(report(false, 'JOIN', cond, keys))
$$;

-- a timestamp cast to a date: several rows of the table in a day, a NULL, 'infinity' (the date infinity no fact has), a day in the future
SELECT * FROM each_join('d.at::date = f.ts');
SELECT * FROM each_join('f.ts = d.at::date');
SELECT * FROM each_join('(d.at)::date = f.ts AND f.val > 3');
SELECT * FROM each_join('d.at::date = f.ts AND d.label <> ''d''');
-- integers widened to the type of the fact's column, a NULL on both sides
SELECT * FROM each_join('d.n2::int = f.k');
SELECT * FROM each_join('d.n4::bigint = f.k');
SELECT * FROM each_join('d.n8 = f.k AND d.n2::int = f.k');
-- two keys, one of them a column and one a cast
SELECT * FROM each_join('d.n4 = f.k AND d.at::date = f.ts');
-- not keys: an expression that can raise (an addition overflows), a timestamp with a time zone (it depends on the setting), a function of a fact's column too
SELECT * FROM each_join('d.n4 + 1 = f.k');
SELECT * FROM each_join('d.at::timestamptz::date = f.ts');
SELECT * FROM each_join('d.at::date = f.ts + (d.n4 - d.n4)');
-- a plain column key next to them is the reference
SELECT * FROM each_join('d.n4 = f.k');

-- the cast is the key: an inner join tests the fact's date against the keys of the loaded rows before tuples are made (EXPLAIN: Join Key Filters),
-- where a condition of the rows alone has no key to test
CREATE FUNCTION key_filters(cond text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; r bigint := 0;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(f.amount) FROM facts f JOIN dims d ON %s WHERE d.label = ''e''', cond) LOOP
		IF l ~ 'Join Key Filters:' THEN r := substring(l from '[0-9]+')::bigint; END IF;
	END LOOP;
	RETURN r;
END $$;
SELECT key_filters('d.at::date = f.ts') AS cast_key, key_filters('d.n2::int = f.k') AS widened_key, key_filters('d.n4 + 1 = f.k') AS no_key;

DROP FUNCTION key_filters(text), each_join(text, text), report(bool, text, text, text), same(text), pushed(text), outcome, runq, runs_in_scan;
DROP TABLE facts, dims;
DROP EXTENSION pg_sift;
