-- A table joined by a column that the scan's IN list names is read by one btree scan with the list as its keys, or read whole: the keyed read costs 2,100 instructions
-- an entry (178 ns), the table read whole 1,000 a row (90 ns), so the index wins up to about half the rows of the table and the table read whole beyond it (calibration
-- table in docs/AGG.md). The statements pin both sides of that crossover for a unique key and for twenty rows a key, in a table never analyzed (the rows a key has are
-- counted in the index for a few of the keys), analyzed and vacuumed: the same plan in the three states.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET sift.agg_probe_rewrite = off;

CREATE TABLE ev (id int, fk int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 20000, i % 7 FROM generate_series(1, 100000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, fk, v);
-- a unique key, and twenty rows to each of a thousand keys
CREATE TABLE dm_u (k int PRIMARY KEY, label text, w int) WITH (autovacuum_enabled = false);
INSERT INTO dm_u SELECT i, 'l' || i % 11, i % 5 FROM generate_series(1, 20000) i;
CREATE TABLE dm_n (k int, label text, w int) WITH (autovacuum_enabled = false);
INSERT INTO dm_n SELECT 1 + i % 1000, 'l' || i % 11, i % 5 FROM generate_series(0, 19999) i;
CREATE INDEX dm_n_k ON dm_n (k);

-- the rows read and the index scans of the scan, from its EXPLAIN
CREATE FUNCTION loaded(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := ''; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '^ *Lookup: ' THEN r := r || concat_ws('; ', substring(l from '\d+ rows read'), substring(l from '\d+ index scans')) || '; '; END IF;
	END LOOP;
	RETURN r;
END $$;

-- the join of the table with the keys 1 to n of its key column
CREATE FUNCTION crossover(tab text, n int) RETURNS text LANGUAGE sql AS $$
	SELECT loaded(format('SELECT dm.label, count(*), sum(dm.w) FROM ev JOIN %s dm ON dm.k = ev.fk WHERE ev.fk IN (%s) GROUP BY 1 ORDER BY 1',
		tab, (SELECT string_agg(i::text, ',') FROM generate_series(1, n) i))) $$;

CREATE FUNCTION crossovers() RETURNS TABLE (tab text, keys int, entries int, plan text) LANGUAGE sql AS $$
	SELECT t.tab, t.n, t.entries, crossover(t.tab, t.n)
	FROM (VALUES ('dm_u', 200, 200), ('dm_u', 5000, 5000), ('dm_u', 14000, 14000), ('dm_u', 18000, 18000),
	             ('dm_n', 10, 200), ('dm_n', 250, 5000), ('dm_n', 700, 14000), ('dm_n', 900, 18000)) t (tab, n, entries) $$;

-- never analyzed, never vacuumed
SELECT * FROM crossovers();
-- analyzed, not vacuumed
ANALYZE ev, dm_u, dm_n;
SELECT * FROM crossovers();
-- vacuumed
VACUUM (FREEZE, ANALYZE) ev, dm_u, dm_n;
SELECT * FROM crossovers();

DROP FUNCTION crossovers(), crossover(text, int), loaded(text);
DROP TABLE ev, dm_u, dm_n;
DROP EXTENSION pg_sift;
