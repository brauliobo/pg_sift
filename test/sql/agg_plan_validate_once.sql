-- The statistics of a column index are validated against its directory once in a planning, not at every call: the cost estimate of each partition's index asks
-- for them three times and the source another, and each validation reads the metapage and the directory page under share locks (leaf_stats_checked). A group by over
-- 12 partitions validates each index once or twice (the first plan of a backend may build them), and the plan after a write sees it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE pv (id int NOT NULL, kind text NOT NULL, amount float8 NOT NULL, ts date NOT NULL, deleted_at timestamptz) PARTITION BY RANGE (ts);
SELECT format('CREATE TABLE pv_%1$s PARTITION OF pv FOR VALUES FROM (%2$L) TO (%3$L)', i, date '2024-01-01' + i, date '2024-01-02' + i) FROM generate_series(0, 11) i \gexec
INSERT INTO pv SELECT i, 'cat_' || (i % 4), i % 17, date '2024-01-01' + i % 12 FROM generate_series(1, 6000) i;
SELECT format('CREATE INDEX ON pv_%s USING sift_col (id, kind, amount, ts, deleted_at)', i) FROM generate_series(0, 11) i \gexec
VACUUM (FREEZE, ANALYZE) pv;

CREATE FUNCTION checked(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM sift.stats_reset();
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP NULL; END LOOP;
	RETURN (SELECT value FROM sift.stats() WHERE name = 'leaf_stats_checked');
END $$;

-- the first plan builds the statistics
SELECT checked('SELECT kind, count(*), sum(amount) FROM pv WHERE deleted_at IS NULL GROUP BY 1') > 0 AS first_plan;
SELECT checked('SELECT kind, count(*), sum(amount) FROM pv WHERE deleted_at IS NULL GROUP BY 1') <= 12 * 2 AS once_a_planning;
SELECT checked('SELECT count(*) FROM pv WHERE deleted_at IS NULL') <= 12 * 2 AS count_once_a_planning;

-- a write and a VACUUM change the directory: the next planning validates again and sees them
INSERT INTO pv SELECT i, 'cat_new', 1, date '2024-01-01' + i % 12 FROM generate_series(6001, 7200) i;
VACUUM pv;
SELECT count(*) AS new_kind_groups FROM (SELECT kind FROM pv WHERE deleted_at IS NULL GROUP BY 1) g WHERE kind = 'cat_new';
SELECT checked('SELECT kind, count(*), sum(amount) FROM pv WHERE deleted_at IS NULL GROUP BY 1') BETWEEN 12 AND 12 * 2 AS validated_again;

DROP FUNCTION checked(text);
DROP TABLE pv;
DROP EXTENSION pg_sift;
