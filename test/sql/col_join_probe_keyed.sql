-- The rows of a thousand timestamps joined to a table whose sift_col index is cut by the timestamp and holds an order array of it: a probe of the index for each timestamp (a nested loop) reads the
-- chunk of the sorted keys that holds it and the rows of the unit (13 to 18 us measured at 300,000 rows, 17 to 18 at 1,000,000), and the hash join reads the table (12 ms at 300,000
-- rows). The seek of a probe was priced as one shared by the probes of the loop (a thousandth of a seek each) and as the chunks of two bounds for an equality, 3.8 units a probe
-- against the 14 measured: the nested loop of 1,000 probes was chosen and ran 17 ms to the hash join's 13. Each probe pays for its seek, which reads one chunk.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE ev AS SELECT i AS id, timestamp '2020-01-01' + ((hashint4(i) & 2147483647) % 157766400) * interval '1 second' + i * interval '1 microsecond' AS t FROM generate_series(1, 300000) i;
CREATE INDEX ev_c ON ev USING sift_col (t, id) WITH (key_cut = 't', order_columns = 't', order_array_columns = 't');
CREATE TABLE few AS SELECT t FROM ev WHERE id % 10000 = 0;
CREATE TABLE many AS SELECT t FROM ev WHERE id % 300 = 0;
VACUUM (FREEZE, ANALYZE) ev, few, many;

CREATE FUNCTION join_path(probes text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'hash join';
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT e.id FROM %I k JOIN ev e ON e.t = k.t', probes) LOOP
		IF l ~ 'Nested Loop' THEN r := 'nested loop'; EXIT; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT 'few' AS probes, count(*) AS rows, join_path('few') AS path FROM few;
SELECT 'many' AS probes, count(*) AS rows, join_path('many') AS path FROM many;

DROP FUNCTION join_path(text);
DROP TABLE ev, few, many;
DROP EXTENSION pg_sift;
