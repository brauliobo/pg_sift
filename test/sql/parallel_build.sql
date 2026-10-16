-- Parallel CREATE INDEX: every participant scans its share of the heap into sorted runs, the leader
-- merges them into the segments a serial build writes.
CREATE EXTENSION pg_sift;
SET maintenance_work_mem = '128MB';
SET max_parallel_maintenance_workers = 2;
SET enable_seqscan = off;

CREATE TABLE pb (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO pb (body)
SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common word' || (i % 50) || ' ' || repeat('filler ', i % 5)
FROM generate_series(1, 30000) i;
UPDATE pb SET body = NULL WHERE id % 1000 = 0;
DELETE FROM pb WHERE id % 13 = 0;

SET max_parallel_maintenance_workers = 0;
CREATE INDEX pb_ser ON pb USING sift (body);
CREATE TABLE ranked_ser AS
	SELECT id, row_number() OVER () AS pos FROM
	(SELECT id FROM pb WHERE body @@> 'g5 OR g7 OR k3 AND word9' ORDER BY sift.score(ctid) DESC, id LIMIT 50) s;
CREATE TABLE segs_ser AS SELECT gen, ndocs, sum_dl, nterms, bytes FROM sift.segments('pb_ser');
DROP INDEX pb_ser;

SET max_parallel_maintenance_workers = 2;
SET client_min_messages = debug1;
CREATE INDEX pb_par ON pb USING sift (body);
RESET client_min_messages;
SELECT count(*) AS same_segments FROM sift.segments('pb_par') p JOIN segs_ser s USING (gen, ndocs, sum_dl, nterms);
SELECT count(*) AS serial_segments FROM segs_ser;
SELECT sift.verify('pb_par', true);

SELECT (SELECT count(*) FROM pb WHERE body @@> 'k3') = (SELECT count(*) FROM pb WHERE id % 7 = 3 AND id % 1000 <> 0) AS term_count_ok;
SELECT (SELECT count(*) FROM pb WHERE body @@> 'common') = (SELECT count(*) FROM pb WHERE body IS NOT NULL) AS all_docs_ok;
SELECT count(*) AS ranked_diff FROM
	(SELECT id, row_number() OVER () AS pos FROM
	 (SELECT id FROM pb WHERE body @@> 'g5 OR g7 OR k3 AND word9' ORDER BY sift.score(ctid) DESC, id LIMIT 50) s
	 EXCEPT SELECT id, pos FROM ranked_ser) d;

-- the index keeps taking inserts and merges
INSERT INTO pb (body) SELECT 'late word' || i FROM generate_series(1, 500) i;
SELECT count(*) FROM pb WHERE body @@> 'late';
SELECT sift.merge('pb_par') >= 0 AS merged;
SELECT sift.verify('pb_par', true);
SET client_min_messages = debug1;
REINDEX INDEX pb_par;
RESET client_min_messages;
SELECT sift.verify('pb_par', true);

-- MVCC snapshot variant and an expression index
SET client_min_messages = debug1;
CREATE INDEX CONCURRENTLY pb_cic ON pb USING sift (body);
CREATE INDEX pb_expr ON pb USING sift ((body || ' tail'));
RESET client_min_messages;
SELECT sift.verify('pb_cic', true);
SELECT sift.verify('pb_expr', true);

-- build_segments asks for an exact layout, so it builds serially
SET client_min_messages = debug1;
CREATE INDEX pb_one ON pb USING sift (body) WITH (build_segments = 3);
RESET client_min_messages;
SELECT count(*) AS three_segments FROM sift.segments('pb_one');

-- a heap bigger than a quarter of shared_buffers is scanned from where a scan stopped last, but
-- the participants need ascending keys
CREATE TABLE pw (id serial, body text) WITH (parallel_workers = 2);
INSERT INTO pw (body) SELECT 'w' || (i % 100) || ' ' || repeat('x', 600) FROM generate_series(1, 120000) i;
SELECT pg_relation_size('pw') > pg_size_bytes(current_setting('shared_buffers')) / 4 AS syncscan_size;
RESET enable_seqscan;
SET max_parallel_workers_per_gather = 0;
SELECT count(*) FROM (SELECT id FROM pw LIMIT 60000) s;
CREATE INDEX pw_idx ON pw USING sift (body);
SELECT sift.verify('pw_idx', true);
SELECT count(*) FROM pw WHERE body @@> 'w7';

DROP TABLE pb, pw, ranked_ser, segs_ser;
DROP EXTENSION pg_sift;
