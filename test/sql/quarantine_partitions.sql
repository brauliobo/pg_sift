-- One partition with a damaged index: the other partitions keep their indexes, the parent answers from the heap of the damaged one and the index of the others
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
SET client_min_messages = error;
SET sift.on_corruption = quarantine;
CREATE TABLE p (id int, v int) PARTITION BY RANGE (id);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10000);
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (10000) TO (20000);
INSERT INTO p SELECT i, i % 7 FROM generate_series(0, 19999) i;
CREATE INDEX p_idx ON p USING sift_col (id, v);
ANALYZE p;
SELECT count(*) AS partition_indexes FROM pg_index WHERE indrelid IN ('p1'::regclass, 'p2'::regclass) AND indexrelid::regclass::text LIKE 'p%_id_v_idx';
SELECT (SELECT count(*) FROM p WHERE v = 3) AS threes \gset
SELECT sift._test_poke('p1_id_v_idx', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM p WHERE v = 3 $$) AS detecting_statement;
SELECT index FROM sift.quarantine;
SELECT count(*) = :threes AS parent_is_exact FROM p WHERE v = 3;
SELECT expect($$ INSERT INTO p VALUES (5, 5), (15000, 5) $$) AS insert_in_both;
SELECT expect($$ UPDATE p SET v = 6 WHERE id IN (6, 16000) $$) AS update_in_both;
VACUUM p;
SELECT tableoid::regclass AS part, count(*) FROM p GROUP BY 1 ORDER BY 1;
REINDEX INDEX p1_id_v_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;

DROP TABLE p;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
