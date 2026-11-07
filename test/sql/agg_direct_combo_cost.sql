-- A slot that PostgreSQL evaluates (a column minus a column) is paid for each tuple the scan reads (21 ns: the leaves read and the combination looked up) and for each
-- distinct combination of the leaves (69 ns an entry: 3.1 ms for 150,000 rows of 10 pairs of values, 13.5 ms for 150,000 pairs), so the scan of the same rows costs
-- the more the more distinct pairs the two columns have, and 2.8 times the maximum of a column that the index answers from its summaries. The estimates are
-- compared with each other, not with a time. Every statement is also run with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE spans (id int PRIMARY KEY, tenant int NOT NULL, few_s int NOT NULL, few_e int NOT NULL, many_s int NOT NULL, many_e int NOT NULL);
INSERT INTO spans SELECT g, g % 50, g % 10, g % 10 + 5, g, g + 3 FROM generate_series(1, 60000) g;
CREATE INDEX spans_col ON spans USING sift_col (id, tenant, few_s, few_e, many_s, many_e) WITH (rollup_dims = 'off');
VACUUM (FREEZE, ANALYZE) spans;

CREATE FUNCTION scan_cost(q text) RETURNS numeric LANGUAGE sql AS $$ SELECT scan_cost::numeric FROM sift.explain_why(q) WHERE scan_cost IS NOT NULL $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg(q) $$;

-- the tuples of 10 distinct pairs cost 2.8 times the maximum of one column; 60,000 distinct pairs cost 2.3 to 3.5 times the 10
SELECT scan_cost('SELECT tenant, max(few_e - few_s) FROM spans GROUP BY tenant') > 2.8 * scan_cost('SELECT tenant, max(few_e) FROM spans GROUP BY tenant') AS rows_priced,
       scan_cost('SELECT tenant, max(many_e - many_s) FROM spans GROUP BY tenant') / scan_cost('SELECT tenant, max(few_e - few_s) FROM spans GROUP BY tenant') BETWEEN 2.3 AND 3.5 AS entries_priced;

SELECT differences('SELECT tenant, max(few_e - few_s) FROM spans GROUP BY tenant') AS few_differences,
       differences('SELECT tenant, max(many_e - many_s) FROM spans GROUP BY tenant') AS many_differences;

DROP FUNCTION differences(text);
DROP FUNCTION scan_cost(text);
DROP TABLE spans;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
