-- A table whose index tracks its changes becomes a partition, or a child of an inheritance parent: a statement on the parent fires none of its statement triggers, so the deletes and key changes it
-- makes are not in the log. The log ends its completeness when the table becomes a child (the event trigger of ALTER TABLE), so that a read does not take it for complete while the table is a child
-- or after DETACH and NO INHERIT; REINDEX gives the index a new baseline.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION same_groups(t regclass) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL sift.enable_agg = on;
  SET LOCAL seq_page_cost = 10000;
  EXECUTE format('CREATE TEMP TABLE by_sift ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
  SET LOCAL sift.enable_agg = off;
  RESET seq_page_cost;
  EXECUTE format('CREATE TEMP TABLE by_heap ON COMMIT DROP AS SELECT g, count(*) AS c, sum(v) AS s FROM %s GROUP BY g', t);
  SELECT count(*) INTO n FROM ((TABLE by_sift EXCEPT TABLE by_heap) UNION ALL (TABLE by_heap EXCEPT TABLE by_sift)) d;
  RETURN n;
END $$;

-- a partition
CREATE TABLE pa (id int, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE pa1 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO pa1 SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX pa1_c ON pa1 USING sift_col (id, g, v);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
ALTER TABLE pa ATTACH PARTITION pa1 FOR VALUES FROM (1) TO (100000);
DELETE FROM pa WHERE id % 2 = 0;
UPDATE pa SET id = id + 500 WHERE id % 5 = 1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT same_groups('pa1') AS groups_differing_attached;
ALTER TABLE pa DETACH PARTITION pa1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT same_groups('pa1') AS groups_differing_detached;
REINDEX INDEX pa1_c;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
DELETE FROM pa1 WHERE id % 3 = 0;
SELECT same_groups('pa1') AS groups_differing_rebuilt;

-- an inheritance child
CREATE TABLE ih (id int, g int NOT NULL, v int);
CREATE TABLE ih1 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO ih1 SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX ih1_c ON ih1 USING sift_col (id, g, v);
ALTER TABLE ih1 INHERIT ih;
DELETE FROM ih WHERE id % 2 = 0;
UPDATE ih SET id = id + 500 WHERE id % 5 = 1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT same_groups('ih1') AS groups_differing_child;
ALTER TABLE ih1 NO INHERIT ih;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT same_groups('ih1') AS groups_differing_after_no_inherit;
REINDEX INDEX ih1_c;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT same_groups('ih1') AS groups_differing_after_reindex;

DROP TABLE pa, pa1, ih, ih1;
DROP FUNCTION same_groups(regclass);
DROP EXTENSION pg_sift;
