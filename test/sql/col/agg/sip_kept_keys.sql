-- The join filter of a dimension (SiftAggV6Sip) keeps the rows of the fact table whose key one of the dimension's rows has: the share of the keys its conditions keep,
-- not the share of its rows. A condition that keeps a fifth of the rows of every parent keeps every key, filters nothing, and plans no filter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE skk_facts (id int PRIMARY KEY, parent_id int NOT NULL, amount float8 NOT NULL);
INSERT INTO skk_facts SELECT i, 1 + i % 2000, i % 97 FROM generate_series(1, 30000) i;
SET client_min_messages = warning;
CREATE INDEX skk_facts_col ON skk_facts USING sift_col (id, parent_id, amount);
RESET client_min_messages;
CREATE INDEX skk_facts_parent ON skk_facts (parent_id);
CREATE TABLE skk_links (id int PRIMARY KEY, parent_id int NOT NULL, status int NOT NULL, label text NOT NULL);
INSERT INTO skk_links SELECT i, 1 + i % 2000, (i / 2000) % 5, 'label_' || i % 6 FROM generate_series(0, 23999) i;
CREATE INDEX skk_links_parent ON skk_links (parent_id, status);
VACUUM (FREEZE, ANALYZE) skk_facts, skk_links;

-- the estimated rows of the scan of the fact table under the join filter (NULL without one)
CREATE FUNCTION skk_sip_rows(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (SELECT (p ->> 'Plan Rows')::float8 FROM jsonb_path_query(j, 'strict $.**') p
		WHERE jsonb_typeof(p) = 'object' AND p ->> 'Custom Plan Provider' = 'SiftAggV6Sip' LIMIT 1);
END $$;

-- the other scans of the fact table off: the filter is the plan of the fact table whenever it is a path of it
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

-- status = 0 keeps a fifth of the links, and a link of every parent
SELECT skk_sip_rows('SELECT f.id, l.label FROM skk_facts f JOIN skk_links l ON l.parent_id = f.parent_id AND l.status = 0') AS every_key;
-- parent_id < 100 keeps the links of 99 parents: about 99 / 2000 of the fact rows
SELECT skk_sip_rows('SELECT f.id, l.label FROM skk_facts f JOIN skk_links l ON l.parent_id = f.parent_id AND l.parent_id < 100') BETWEEN 1000 AND 3000 AS few_keys;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET enable_bitmapscan;

SELECT l.label, count(*) AS n, sum(f.amount) AS total FROM skk_facts f JOIN skk_links l ON l.parent_id = f.parent_id AND l.status = 0 GROUP BY 1 ORDER BY 1;

DROP FUNCTION skk_sip_rows(text);
DROP TABLE skk_facts, skk_links;
DROP EXTENSION pg_sift;
