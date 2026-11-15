-- A BM25 index that finds its own damage leaves service: the match filter runs on the heap and finds the documents the index found; the functions that exist only inside
-- a sift scan say so
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
	p text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		p := p || r || E'\n';
	END LOOP;
	RETURN CASE WHEN p ~ 'Sift|d_idx' THEN 'sift' ELSE 'heap' END;
END $$;

SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE d (id int, body text);
INSERT INTO d SELECT i, 'alpha beta ' || (i % 10) || ' gamma' || (i % 3) FROM generate_series(1, 5000) i;
CREATE INDEX d_idx ON d USING sift (body);
CREATE TABLE answers AS
	SELECT 'filter' AS q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM d WHERE body @@> 'alpha 3 gamma1') AS a
	UNION ALL SELECT 'count', (SELECT count(*) FROM d WHERE body @@> 'alpha 3')::text;
SELECT plan_of('SELECT count(*) FROM d WHERE body @@> ''alpha 3''') AS plan_before;

SELECT sift._test_poke('d_idx', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM d WHERE body @@> 'alpha 3' $$) AS detecting_statement;
SELECT index, error_code FROM sift.quarantine;
SELECT plan_of('SELECT count(*) FROM d WHERE body @@> ''alpha 3''') AS plan_after;
SELECT 'filter' AS q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM d WHERE body @@> 'alpha 3 gamma1') AS a
UNION ALL SELECT 'count', (SELECT count(*) FROM d WHERE body @@> 'alpha 3')::text
EXCEPT SELECT q, a FROM answers;
SELECT expect($$ SELECT sift.score(ctid) FROM d WHERE body @@> 'alpha 3' $$) AS score_without_the_index;

REINDEX INDEX d_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT plan_of('SELECT count(*) FROM d WHERE body @@> ''alpha 3''') AS plan_after_reindex;

DROP TABLE answers, d;
DROP FUNCTION plan_of(text), expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
