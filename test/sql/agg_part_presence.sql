-- The key lists of the integer columns an index keeps in its units (key_columns = auto) follow the data of each partition: one partition keeps the
-- list of a column and its neighbour does not. That is no other layout: a unit without a list is read by its rows, so the table is one source and the
-- scan counts on the lists every partition keeps (and on the measures that the key table of a column adds to its dim in the partitions that keep one). The statements run with enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE pk (id int NOT NULL, g int NOT NULL, h int NOT NULL, v int NOT NULL, m numeric(10,2) NOT NULL, d date NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE pk_a PARTITION OF pk FOR VALUES FROM (0) TO (20000) WITH (autovacuum_enabled = false);
CREATE TABLE pk_b PARTITION OF pk FOR VALUES FROM (20000) TO (40000) WITH (autovacuum_enabled = false);
CREATE TABLE pk_c PARTITION OF pk FOR VALUES FROM (40000) TO (60000) WITH (autovacuum_enabled = false);
INSERT INTO pk SELECT i, ((i::bigint * 7919) % 300)::int, ((i::bigint * 104729) % 40)::int, i % 17, i % 1000 / 10.0, date '2025-01-01' + i % 300 FROM generate_series(0, 59999) i;
CREATE INDEX pk_col ON pk USING sift_col (id, g, h, v, m, d) WITH (key_columns = 'g, h');
VACUUM (FREEZE, ANALYZE) pk;

CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg(q) $$;
CREATE FUNCTION why(q text) RETURNS text LANGUAGE sql AS $$ SELECT CASE WHEN bool_or(used) THEN 'scan' ELSE max(slug) END FROM sift.explain_why(q) $$;

CREATE TABLE queries (name text, q text);
INSERT INTO queries VALUES
	('distinct g', 'SELECT h % 4 AS k, count(DISTINCT g) AS n, sum(v) AS s FROM pk GROUP BY 1'),
	('distinct h', 'SELECT g % 4 AS k, count(DISTINCT h) AS n FROM pk GROUP BY 1'),
	('plain', 'SELECT v, count(*) AS n FROM pk GROUP BY 1'),
	('measure', 'SELECT g, count(*) AS n, sum(m) AS s FROM pk GROUP BY 1');
SELECT name, why(q) AS plan, differences(q) AS differences FROM queries ORDER BY name;

-- one partition keeps no list: the same statements
DO $$
DECLARE ix regclass;
BEGIN
	SELECT i.indexrelid::regclass INTO ix FROM pg_index i WHERE i.indrelid = 'pk_b'::regclass;
	EXECUTE format('ALTER INDEX %s SET (key_columns = ''off'')', ix);
	EXECUTE format('REINDEX INDEX %s', ix);
END $$;
VACUUM (FREEZE, ANALYZE) pk;
SELECT name, why(q) AS plan, differences(q) AS differences FROM queries ORDER BY name;

-- and one that keeps the list of another column
DO $$
DECLARE ix regclass;
BEGIN
	SELECT i.indexrelid::regclass INTO ix FROM pg_index i WHERE i.indrelid = 'pk_c'::regclass;
	EXECUTE format('ALTER INDEX %s SET (key_columns = ''g'')', ix);
	EXECUTE format('REINDEX INDEX %s', ix);
END $$;
VACUUM (FREEZE, ANALYZE) pk;
SELECT name, why(q) AS plan, differences(q) AS differences FROM queries ORDER BY name;

DROP TABLE queries;
DROP FUNCTION why(text), differences(text);
DROP TABLE pk;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
