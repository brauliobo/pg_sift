-- A text column on the device as the codes of the dictionaries of its parts, and the granules the key marks and the zones leave out: sift.gpu = on must give the rows
-- the CPU gives for text filters, text keys and COUNT(DISTINCT) over a table of several parts, with the constant in some dictionaries and not in others, on a build with or
-- without a device (the test prints only whether the two agree).
CREATE EXTENSION pg_sift;
SET client_min_messages = error;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE dx (id int, k int, d date, s text, u text, v int, w bigint) WITH (autovacuum_enabled = false);
INSERT INTO dx
SELECT i, (i / 20000) % 5, DATE '2020-01-01' + (i / 700) % 400,
       CASE WHEN i % 3 = 0 THEN '' ELSE 'm' || ((i * 7 + i / 50000 * 13) % 90) END,
       CASE WHEN i < 100000 THEN 'only-early' WHEN i % 11 = 0 THEN NULL ELSE 'u' || (i % 40) END,
       (i * 31) % 1000, i::bigint * 3
FROM generate_series(1, 300000) i;
CREATE INDEX dx_col ON dx USING sift_col (k, d, s, u, v, w, id);
VACUUM (FREEZE, ANALYZE) dx;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.gpu = on;
	SET sift.gpu_min_rows = 1;
	EXECUTE format('CREATE TEMP TABLE r_on AS SELECT x::text AS r FROM (%s) x', q);
	SET sift.gpu = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.gpu;
	RESET sift.gpu_min_rows;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	d := d + abs((SELECT count(*) FROM r_on) - (SELECT count(*) FROM r_off));
	DROP TABLE r_on, r_off;
	RETURN d = 0;
END $$;

-- text filters: equality, inequality, a constant that is in no dictionary, one that is in some
SELECT same($$SELECT count(*), sum(v) FROM dx WHERE s <> ''$$) AS ne_empty;
SELECT same($$SELECT count(*), sum(w) FROM dx WHERE s = 'm7'$$) AS eq_some;
SELECT same($$SELECT count(*) FROM dx WHERE s = 'absent'$$) AS eq_absent;
SELECT same($$SELECT count(*) FROM dx WHERE s <> 'absent'$$) AS ne_absent;
SELECT same($$SELECT count(*), min(v) FROM dx WHERE u = 'only-early'$$) AS eq_one_part;
SELECT same($$SELECT k, count(*) FROM dx WHERE s <> '' AND v > 500 GROUP BY k$$) AS filter_and_key;
SELECT same($$SELECT k, sum(v) FROM dx WHERE s = 'm3' AND u <> 'u1' GROUP BY k$$) AS two_texts;

-- text keys, with NULLs, with a filter, with the groups of different parts meeting
SELECT same($$SELECT s, count(*) FROM dx GROUP BY s$$) AS key_text;
SELECT same($$SELECT u, count(*), sum(v) FROM dx GROUP BY u$$) AS key_null_text;
SELECT same($$SELECT s, k, count(*) FROM dx WHERE s <> '' GROUP BY s, k$$) AS key_text_int;
SELECT same($$SELECT s, u, count(*) FROM dx GROUP BY s, u$$) AS key_two_texts;
SELECT same($$SELECT s, count(*) FROM dx WHERE u = 'u5' GROUP BY s$$) AS key_filtered;
SELECT same($$SELECT s, count(*) c FROM dx WHERE s <> '' GROUP BY s ORDER BY c DESC, s LIMIT 7$$) AS key_topn;

-- COUNT(DISTINCT) of a number next to a text, which the device leaves to the CPU or runs the same
SELECT same($$SELECT s, count(DISTINCT v) FROM dx GROUP BY s$$) AS distinct_by_text;
SELECT same($$SELECT count(DISTINCT s) FROM dx$$) AS distinct_text;
SELECT same($$SELECT count(DISTINCT v) FROM dx WHERE s <> ''$$) AS distinct_filtered;

-- the key marks and the zones of the date leave granules out
SELECT same($$SELECT count(*), sum(v) FROM dx WHERE k = 2 AND d BETWEEN DATE '2020-03-01' AND DATE '2020-03-31'$$) AS key_and_date;
SELECT same($$SELECT s, count(*) FROM dx WHERE k = 3 AND d >= DATE '2020-06-01' AND d < DATE '2020-07-01' AND s <> '' GROUP BY s$$) AS key_date_text;
SELECT same($$SELECT k, count(*) FROM dx WHERE k IN (1, 4) GROUP BY k$$) AS key_in;
SELECT same($$SELECT count(*) FROM dx WHERE k = 9$$) AS key_absent;
SELECT same($$SELECT count(*) FROM dx WHERE k < 2 AND d <= DATE '2020-02-01'$$) AS key_range;

DROP FUNCTION same(text);
DROP TABLE dx;
DROP EXTENSION pg_sift;
