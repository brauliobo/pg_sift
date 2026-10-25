-- The same statement as agg_cte_inline_rows at 130,000 rows, where the three scans cost 1.47 times less than the shared plan spends on the work they replace:
-- less than the factor of 1.6 this choice had before and more than the 1.15 it has. The shared CTE takes 2.1 seconds here, the scans 0.8 s. At 100,000 rows
-- the ratio is 1.78, at 150,000 1.32 and at 200,000 1.09.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
\set rows 130000
\ir test/agg_cte_inline_fixture.sql
-- the nodes of the scan in the plan, and the rows against the normal plan
CREATE FUNCTION scan_nodes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP IF l ~ 'Custom Scan \(SiftAgg' THEN n := n + 1; END IF; END LOOP;
	RETURN n;
END $$;
-- the shared CTE takes the scan (the arms and the SiftAggRows node above them), and the rows are the normal plan's
SELECT scan_nodes(series_query()) AS scan_nodes, differences(series_query()) AS differences;
DROP FUNCTION differences, scan_nodes, series_query;
DROP TABLE events, dims, maps;
DROP EXTENSION pg_sift;
