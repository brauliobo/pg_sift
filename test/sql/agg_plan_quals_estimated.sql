-- The scan reuses the selectivity the planner kept for each condition of the table (its RestrictInfo) and estimates itself only the conditions the planner has
-- none for: those inside an OR. Estimating a condition again takes a pass over the statistics of the column (an IN list over all its elements), and the
-- same conditions are compiled again for each arm of a statement. sift.stats() counts the estimates the scan made (qual_selectivity_estimated).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE qe (id int NOT NULL, kind text NOT NULL, amount float8 NOT NULL, ts date NOT NULL, deleted_at timestamptz);
INSERT INTO qe SELECT i, 'cat_' || (i % 4), i % 17, date '2024-01-01' + i % 200, CASE WHEN i % 37 = 0 THEN now() END FROM generate_series(1, 20000) i;
CREATE INDEX qe_col ON qe USING sift_col (id, kind, amount, ts, deleted_at);
VACUUM (FREEZE, ANALYZE) qe;

CREATE FUNCTION estimated(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text; scan bool := false;
BEGIN
	PERFORM sift.stats_reset();
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP scan := scan OR line ~ 'SiftAgg'; END LOOP;
	RETURN CASE WHEN scan THEN (SELECT value FROM sift.stats() WHERE name = 'qual_selectivity_estimated') ELSE -1 END;
END $$;

SELECT estimated('SELECT kind, count(*) FROM qe WHERE amount > 5 AND ts >= date ''2024-03-01'' AND deleted_at IS NULL GROUP BY 1') AS conjuncts;
SELECT estimated('SELECT kind, count(*) FROM qe WHERE amount > 5 AND kind IN (''cat_0'', ''cat_1'') GROUP BY 1') AS conjunct_and_in;
SELECT estimated('SELECT kind, count(*) FROM qe WHERE amount > 5 OR ts >= date ''2024-03-01'' GROUP BY 1') > 0 AS inside_or_estimated;

DROP FUNCTION estimated(text);
DROP TABLE qe;
DROP EXTENSION pg_sift;
