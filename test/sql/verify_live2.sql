-- sift.verify raises ERRCODE_INDEX_CORRUPTED, and never crashes, on a segment with vacuumed documents (the liveness rows and the
-- table of dead postings of segment format 2) with random bytes overwritten; and passes on the same index rebuilt
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE vl (id serial PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO vl (body) SELECT 'w' || (i % 7) || ' x' || (i % 101) || ' common doc' || i FROM generate_series(1, 3000) i;
CREATE INDEX vl_idx ON vl USING sift (body);
DELETE FROM vl WHERE id % 3 = 0;
VACUUM vl;
SELECT dirty, dead_docs FROM sift.segments('vl_idx');
SELECT sift.verify('vl_idx', true);

CREATE TABLE vl_res (verdict text);
CREATE FUNCTION vl_verify(heapall bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.verify('vl_idx', heapall);
	INSERT INTO vl_res VALUES ('ok');
EXCEPTION WHEN OTHERS THEN
	INSERT INTO vl_res VALUES (CASE WHEN SQLSTATE = 'XX002' THEN 'corrupt' ELSE 'other: ' || SQLERRM END);
END $$;

-- VACUUM cannot run in a function: the statements of each round are generated and run in order
SELECT setseed(0.42);
\set ECHO none
SELECT stmt FROM generate_series(1, 150) i, LATERAL (VALUES
	(0, 'REINDEX INDEX vl_idx'),
	(1, format('DELETE FROM vl WHERE id %% %s = 0', 3 + i % 4)),
	(2, 'VACUUM vl'),
	(3, 'DO $x$ BEGIN PERFORM sift._test_poke(''vl_idx'', 2 + floor(random() * (pg_relation_size(''vl_idx'') / 8192 - 2))::int,
		floor(random() * 8190)::int, floor(random() * 256)::int, 1); END $x$'),
	(4, format('DO $x$ BEGIN PERFORM vl_verify(%L); END $x$', i % 2 = 0))) t (k, stmt) ORDER BY i, k \gexec
\set ECHO all
SELECT count(*) FILTER (WHERE verdict LIKE 'other%') = 0 AS every_verdict_ok_or_corrupt, count(*) FILTER (WHERE verdict = 'corrupt') > 0 AS some_found FROM vl_res;

REINDEX INDEX vl_idx;
DELETE FROM vl WHERE id % 5 = 0;
VACUUM vl;
SELECT sift.verify('vl_idx', true);
SELECT count(*) = (SELECT count(*) FROM vl WHERE body ~ 'common') AS count_ok FROM vl WHERE body @@> 'common';
DROP TABLE vl, vl_res;
DROP FUNCTION vl_verify(bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
