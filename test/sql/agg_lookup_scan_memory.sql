-- The memory of the load of a joined sift_table (scope agg_lookup of core/membudget.h, sift._test_mem_highwater()): the scope holds the lookup table, not the structures the scan of the
-- table reads its units with, so that the rows of a semi or an anti join, 8,000 of 120,000 for the semi join, load within half of work_mem (here 2 MB) and the aggregate scan answers the query.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET work_mem = '4MB';

CREATE TABLE facts (id int PRIMARY KEY, parent_id int NOT NULL, status text NOT NULL) USING sift_table;
INSERT INTO facts SELECT i, 1 + (hashint4(i + 1) & 2147483647) % 2000, (ARRAY['open', 'paid', 'void', 'late'])[1 + i % 4] FROM generate_series(1, 30000) i;
CREATE TABLE links (id int PRIMARY KEY, parent_id int NOT NULL, label text NOT NULL, weight int NOT NULL, status smallint NOT NULL) USING sift_table;
INSERT INTO links SELECT i, 1 + (hashint4(i + 1) & 2147483647) % 2000, 'lbl_' || i % 10, i / 7 % 3, i / 3 % 5 FROM generate_series(1, 120000) i;
VACUUM (FREEZE, ANALYZE) facts, links;
\ir test/agg_helpers.sql

CREATE FUNCTION lookup_peak(q text) RETURNS TABLE (scan bool, same bool, under_budget bool) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM count(*) FROM sift._test_mem_highwater(true);
	RETURN QUERY SELECT runs_in_scan(q), outcome(q, false) = outcome(q, true), (SELECT peak < budget FROM sift._test_mem_highwater() WHERE scope = 'agg_lookup');
END $$;

SELECT * FROM lookup_peak($q$SELECT f.status, count(*) AS n FROM facts f WHERE EXISTS (SELECT 1 FROM links l WHERE l.parent_id = f.parent_id AND l.status = 0 AND l.weight = 1) GROUP BY 1$q$);
SELECT * FROM lookup_peak($q$SELECT f.status, count(*) AS n FROM facts f WHERE NOT EXISTS (SELECT 1 FROM links l WHERE l.parent_id = f.parent_id AND l.status = 0 AND l.weight = 1) GROUP BY 1$q$);

DROP FUNCTION lookup_peak(text), outcome(text, bool), runs_in_scan(text), pushed, runq;
DROP TABLE facts, links;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
