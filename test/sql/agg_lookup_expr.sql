-- An expression of the columns of a lookup table that a sift_col index of the table holds (a presence mask of its text columns) is one column of the table: the load reads
-- its leaf, where it read the columns the expression reads and the expression was evaluated for each distinct tuple of them. Without such an index the expression is
-- evaluated for each row loaded. The answers are those of the normal plan in both (NULL, blank and unmatched values, a left join, which does not read the
-- leaf: the expression of a row nothing matched is not NULL, rows the transaction changed, workers).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET sift.agg_dimmap_kb = 0;

CREATE FUNCTION presence(a text, b text, c int) RETURNS smallint LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT ((nullif(btrim(a), '') IS NOT NULL)::int + ((nullif(btrim(b), '') IS NOT NULL)::int << 1) + ((c IS NOT NULL)::int << 2))::smallint $$;

CREATE TABLE ev (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 3300, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);

-- 3200 keys, the events of keys 3200 to 3299 have no row
CREATE TABLE dm (k int PRIMARY KEY, a text, b text, c int) WITH (autovacuum_enabled = false);
INSERT INTO dm SELECT i, CASE i % 4 WHEN 0 THEN NULL WHEN 1 THEN '  ' ELSE 'a' || i % 7 END, CASE WHEN i % 5 = 0 THEN '' ELSE ' b ' END,
	CASE WHEN i % 3 = 0 THEN NULL ELSE i END FROM generate_series(0, 3199) i;
CREATE TABLE dh (LIKE dm) WITH (autovacuum_enabled = false);
INSERT INTO dh SELECT * FROM dm;
ALTER TABLE dh ADD PRIMARY KEY (k);
-- the same, never analyzed: the classes of the mask are priced from the index
CREATE TABLE df (LIKE dm) WITH (autovacuum_enabled = false);
INSERT INTO df SELECT * FROM dm;
ALTER TABLE df ADD PRIMARY KEY (k);
CREATE INDEX df_col ON df USING sift_col (k, presence(a, b, c));
CREATE INDEX dm_col ON dm USING sift_col (k, presence(a, b, c));
CREATE INDEX dh_col ON dh USING sift_col (k, a);
VACUUM (FREEZE, ANALYZE) ev, dm, dh;
VACUUM (FREEZE) df;

-- the tests of a column that an index holds as booleans, written as the query writes them: on the fact table and on the lookup table, `IS NULL` for the leaf of `IS NOT NULL`
CREATE TABLE fe (id int, k int, t text, u int) WITH (autovacuum_enabled = false);
INSERT INTO fe SELECT i, i % 3300, CASE i % 3 WHEN 0 THEN NULL WHEN 1 THEN '  ' ELSE 'v' END, CASE WHEN i % 7 = 0 THEN NULL ELSE i END FROM generate_series(1, 40000) i;
CREATE INDEX fe_col ON fe USING sift_col (id, k, (nullif(btrim(t), '') IS NOT NULL), ((t IS NOT NULL) AND (u IS NOT NULL)));
CREATE TABLE dq (LIKE dm) WITH (autovacuum_enabled = false);
INSERT INTO dq SELECT * FROM dm;
ALTER TABLE dq ADD PRIMARY KEY (k);
CREATE INDEX dq_col ON dq USING sift_col (k, (nullif(btrim(a), '') IS NOT NULL), (nullif(btrim(b), '') IS NOT NULL), ((a IS NOT NULL) AND (c IS NOT NULL)));
VACUUM (FREEZE, ANALYZE) fe, dq;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT presence(dm.a, dm.b, dm.c) AS m, count(*) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT ev.id % 5, count(DISTINCT ev.k), count(DISTINCT ev.k) FILTER (WHERE (presence(dm.a, dm.b, dm.c) & 1) <> 0),
		count(DISTINCT ev.k) FILTER (WHERE (presence(dm.a, dm.b, dm.c) & 4) <> 0) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT presence(dm.a, dm.b, dm.c) AS m, count(*), sum(ev.v) FROM ev LEFT JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT presence(dh.a, dh.b, dh.c) AS m, count(*) FROM ev JOIN dh ON dh.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (5, $q$SELECT presence(dm.a, dm.b, dm.c) AS m, dm.a, count(*) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (6, $q$SELECT presence(dm.a, dm.b, dm.c) AS m, count(DISTINCT dm.k) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (7, $q$SELECT ev.v, count(*) FROM ev JOIN dm ON dm.k = ev.k WHERE (presence(dm.a, dm.b, dm.c) & 2) <> 0 GROUP BY 1 ORDER BY 1$q$),
 (8, $q$SELECT presence(df.a, df.b, df.c) AS m, count(*) FROM ev JOIN df ON df.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (9, $q$SELECT ev.id % 5, count(DISTINCT ev.k) FILTER (WHERE (presence(df.a, df.b, df.c) & 1) <> 0) FROM ev JOIN df ON df.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (10, $q$SELECT fe.id % 5, count(DISTINCT dq.k), count(DISTINCT dq.k) FILTER (WHERE nullif(btrim(dq.a), '') IS NOT NULL),
		count(DISTINCT dq.k) FILTER (WHERE nullif(btrim(dq.b), '') IS NULL) FROM fe JOIN dq ON dq.k = fe.k GROUP BY 1 ORDER BY 1$q$),
 (11, $q$SELECT fe.id % 4, count(*) FILTER (WHERE nullif(btrim(fe.t), '') IS NULL), count(*) FILTER (WHERE nullif(btrim(fe.t), '') IS NOT NULL) FROM fe GROUP BY 1 ORDER BY 1$q$),
 (12, $q$SELECT dq.k, count(*) FROM fe JOIN dq ON dq.k = fe.k WHERE nullif(btrim(dq.a), '') IS NULL GROUP BY 1 ORDER BY 1$q$),
 (13, $q$SELECT fe.id % 4, count(*) FILTER (WHERE fe.t IS NOT NULL AND fe.u IS NOT NULL), count(*) FILTER (WHERE fe.t IS NULL OR fe.u IS NULL) FROM fe
		GROUP BY 1 ORDER BY 1$q$),
 (14, $q$SELECT fe.id % 5, count(DISTINCT dq.k) FILTER (WHERE dq.a IS NOT NULL AND dq.c IS NOT NULL), count(DISTINCT dq.k) FILTER (WHERE dq.a IS NULL OR dq.c IS NULL)
		FROM fe JOIN dq ON dq.k = fe.k GROUP BY 1 ORDER BY 1$q$);

-- the rows of the answer, and what the load of the table read
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION loads(sql text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; r int := 0; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '(\d+) column loads' THEN r := substring(l from '(\d+) column loads')::int; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION scanned(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		r := r OR l ~ 'Custom Scan \(SiftAgg\)';
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, from_leaves bool, scan bool, not_empty bool) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), loads(sql) > 0, scanned(sql), answer(sql, true) <> '' FROM shapes ORDER BY n
$$;

SELECT * FROM report('settled');

-- rows the heap changed after the index saw them, in the transaction that reads them
BEGIN;
UPDATE dm SET a = 'changed', c = NULL WHERE k % 31 = 0;
DELETE FROM dm WHERE k % 53 = 0;
INSERT INTO dm SELECT i, 'new', '', 1 FROM generate_series(3200, 3249) i;
UPDATE dh SET a = NULL, b = 'x' WHERE k % 29 = 0;
UPDATE df SET a = NULL, b = 'x' WHERE k % 29 = 0;
UPDATE dq SET a = NULL, b = 'x' WHERE k % 29 = 0;
UPDATE fe SET t = NULL, u = NULL WHERE id % 17 = 0;
SELECT * FROM report('in the transaction');
ROLLBACK;

-- committed, not vacuumed
UPDATE dm SET b = NULL WHERE k % 41 = 0;
DELETE FROM dm WHERE k % 61 = 0;
SELECT * FROM report('committed, not vacuumed');
VACUUM dm;
SELECT * FROM report('vacuumed');

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');

SET sift.agg_lookup_columns = off;
SELECT * FROM report('lookup columns off');

DROP FUNCTION report(text), scanned(text), loads(text), answer(text, bool);
DROP TABLE shapes, ev, dm, dh, df, fe, dq;
DROP FUNCTION presence(text, text, int);
DROP EXTENSION pg_sift;
