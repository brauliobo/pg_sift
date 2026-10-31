-- Word positions survive the write path: documents inserted after the build get their positions when the write buffer is promoted,
-- and a merge keeps the positions of every input, so an index after batches and a forced merge has the size of a fresh build of the
-- same rows and decides the same phrases from its positions (the heap checks of a phrase statement are the fresh index's).
CREATE EXTENSION pg_sift;
SELECT setseed(0.5);

CREATE TABLE fresh (id int PRIMARY KEY, body text);
CREATE TABLE churned (id int PRIMARY KEY, body text);
INSERT INTO fresh
SELECT g, (SELECT string_agg('v' || (get_byte(decode(md5(g || ':' || k), 'hex'), 0) % 24), ' ') FROM generate_series(1, 14) k)
FROM generate_series(1, 6000) g;
-- documents whose text is stored out of line (TOAST)
INSERT INTO fresh SELECT 6000 + g, 'v3 v4 ' || (SELECT string_agg(md5(g || ':' || k), ' ') FROM generate_series(1, 300) k) || ' v3 v4' FROM generate_series(1, 20) g;
INSERT INTO churned SELECT * FROM fresh WHERE id <= 3000;
CREATE INDEX fresh_idx ON fresh USING sift (body);
CREATE INDEX churned_idx ON churned USING sift (body);

DO $$
BEGIN
	FOR b IN 0..5 LOOP
		INSERT INTO churned SELECT * FROM fresh WHERE id > 3000 + b * 500 AND id <= 3500 + b * 500;
		PERFORM sift.promote('churned_idx');
	END LOOP;
	INSERT INTO churned SELECT * FROM fresh WHERE id > 6000;
	PERFORM sift.promote('churned_idx');
END $$;
SELECT sift.merge('churned_idx', true) >= 0 AS merged;
SELECT count(*) AS segments FROM sift.segments('churned_idx') WHERE kind = 'immutable';

CREATE FUNCTION idx_bytes(idx regclass) RETURNS numeric LANGUAGE sql AS $$ SELECT sum(bytes) FROM sift.segments(idx) WHERE kind = 'immutable' $$;
SELECT idx_bytes('churned_idx') BETWEEN 0.97 * idx_bytes('fresh_idx') AND 1.03 * idx_bytes('fresh_idx') AS size_of_a_fresh_build;

-- the phrase statements: the same rows, and the same heap checks as the fresh index
CREATE FUNCTION heap_checks(tbl regclass, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT id FROM %s WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10', tbl, q) INTO j;
	RETURN (SELECT (n ->> 'Heap Checks')::bigint FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1);
END $$;
CREATE FUNCTION top_ids(tbl regclass, q text) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE
	r int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM %s WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 10) s', tbl, q) INTO r;
	RETURN r;
END $$;
CREATE TABLE phrases (q text);
INSERT INTO phrases SELECT format('"v%s v%s"', a, b) FROM generate_series(0, 23, 5) a, generate_series(1, 23, 6) b;
INSERT INTO phrases SELECT format('"v%s v%s v%s"', a, b, c) FROM generate_series(0, 23, 7) a, generate_series(1, 23, 9) b, generate_series(2, 23, 11) c;

SELECT count(*) AS phrases, count(*) FILTER (WHERE heap_checks('churned', q) IS DISTINCT FROM heap_checks('fresh', q)) AS other_heap_checks,
	count(*) FILTER (WHERE top_ids('churned', q) IS DISTINCT FROM top_ids('fresh', q)) AS other_rows
FROM phrases;
SELECT sum(heap_checks('fresh', q)) > 0 AS the_test_has_heap_checks FROM phrases;
SELECT sift.verify('churned_idx', true) IS NOT NULL AS verified;

-- the maintenance worker, which has no snapshot of its own, promotes documents whose text is stored out of line
CREATE TABLE worker_docs (id int PRIMARY KEY, body text);
CREATE INDEX worker_idx ON worker_docs USING sift (body);
INSERT INTO worker_docs SELECT g, 'v1 v2 ' || (SELECT string_agg(md5(g || ':' || k), ' ') FROM generate_series(1, 300) k) FROM generate_series(1, 400) g;
DO $$
BEGIN
	FOR i IN 1..600 LOOP
		EXIT WHEN (SELECT coalesce(sum(ndocs), 0) FROM sift.segments('worker_idx') WHERE kind = 'immutable') >= 300;
		PERFORM pg_sleep(0.1);
	END LOOP;
END $$;
SELECT sum(ndocs) >= 300 AS worker_promoted FROM sift.segments('worker_idx') WHERE kind = 'immutable';
SELECT count(*) AS phrase_rows FROM worker_docs WHERE body @@> '"v1 v2"';

DROP TABLE fresh, churned, phrases, worker_docs;
DROP FUNCTION idx_bytes(regclass), heap_checks(regclass, text), top_ids(regclass, text);
DROP EXTENSION pg_sift;
