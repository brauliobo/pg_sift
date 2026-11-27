-- A row comparison whose leading value has another temporal type than its column, (ts, id) < (date, int) or (d, id) > (timestamp, int), is no range of the key (a date counts days, a timestamp
-- microseconds, and timestamptz against timestamp depends on the time zone): the ordered scan of a sift_col index of format major 2 gives the rows of the heap (S119-14)
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET timezone = 'America/Sao_Paulo';
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE rx (ts timestamp, tz timestamptz, d date, id int4);
INSERT INTO rx SELECT t, t, t::date, i FROM generate_series(1, 3000) i, LATERAL (SELECT timestamp '2020-01-01' + make_interval(hours => i * 7)) x(t);
CREATE INDEX rx_ts ON rx USING sift_col (ts, id) WITH (key_order = 'ts, id');
CREATE INDEX rx_tz ON rx USING sift_col (tz, id) WITH (key_order = 'tz, id');
CREATE INDEX rx_d ON rx USING sift_col (d, id) WITH (key_order = 'd, id');
VACUUM (ANALYZE) rx;

CREATE FUNCTION rx_page(q text, via_index bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE x text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', (NOT via_index)::text, true);
	PERFORM set_config('enable_sort', (NOT via_index)::text, true);
	PERFORM set_config('enable_indexscan', via_index::text, true);
	PERFORM set_config('enable_indexonlyscan', via_index::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
	EXECUTE format('SELECT count(*) || '':'' || coalesce(md5(string_agg(w::text, ''|'')), '''') FROM (%s) w', q) INTO x;
	RETURN x;
END $$;

CREATE TABLE rx_q (n int, q text);
INSERT INTO rx_q VALUES
 (1, 'SELECT ts, id FROM rx WHERE (ts, id) < (date ''2021-03-01'', 5) ORDER BY ts, id'),
 (2, 'SELECT ts, id FROM rx WHERE (ts, id) > (date ''2021-03-01'', 5) ORDER BY ts, id LIMIT 50'),
 (3, 'SELECT ts, id FROM rx WHERE (ts, id) >= (timestamptz ''2021-03-01 00:00+00'', 5) ORDER BY ts, id LIMIT 50'),
 (4, 'SELECT tz, id FROM rx WHERE (tz, id) < (date ''2021-03-01'', 5) ORDER BY tz, id'),
 (5, 'SELECT tz, id FROM rx WHERE (tz, id) > (timestamp ''2021-03-01 00:00'', 5) ORDER BY tz, id LIMIT 50'),
 (6, 'SELECT d, id FROM rx WHERE (d, id) > (timestamp ''2021-03-01 12:00'', 5) ORDER BY d, id LIMIT 50'),
 (7, 'SELECT d, id FROM rx WHERE (d, id) <= (timestamptz ''2021-03-01 12:00+00'', 5) ORDER BY d, id'),
 (8, 'SELECT ts, id FROM rx WHERE (ts, id) < (timestamp ''2021-03-01'', 5::int8) ORDER BY ts, id');
SELECT n, rx_page(q, true) = rx_page(q, false) AS agree FROM rx_q ORDER BY n;

DROP FUNCTION rx_page(text, bool);
DROP TABLE rx_q, rx;
DROP EXTENSION pg_sift;
