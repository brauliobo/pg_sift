-- A promotion or merge that fails (injected after a few pages) gives its blocks back, never fails
-- VACUUM or an insert, and backs off instead of being retried at once.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET enable_seqscan = off;
SET client_min_messages = warning;

CREATE TABLE mf (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO mf (body) SELECT 'seed word' || (i % 50) || ' common' FROM generate_series(1, 2000) i;
CREATE INDEX mf_idx ON mf USING sift (body) WITH (write_buffer_limit = 64);
-- one document is about a segment of its own: 12000 distinct terms fill 128 kB of write buffer pages
CREATE FUNCTION big(n int) RETURNS text LANGUAGE sql AS
	$$ SELECT string_agg('t' || n || 'x' || g, ' ') FROM generate_series(1, 12000) g $$;

-- an explicit promotion fails and leaves nothing behind: no orphan block, the index reads as before
INSERT INTO mf (body) SELECT big(i) FROM generate_series(1, 3) i;
SET sift.test_fail_pages = 3;
SELECT sift.promote('mf_idx');
RESET sift.test_fail_pages;
SELECT sift._test_check('mf_idx') AS orphans;
SELECT sift._test_maint_failures('mf_idx') AS failures;
SELECT count(*) FROM mf WHERE body @@> 'common';
SELECT count(*) FROM mf WHERE body @@> 't2x100';
SELECT sift.verify('mf_idx', true);

-- so does a failing merge, and a successful step clears the failure count
SELECT sift.promote('mf_idx') >= 0 AS promoted;
INSERT INTO mf (body) SELECT 'again ' || big(i) FROM generate_series(1, 3) i;
SELECT sift.promote('mf_idx') >= 0 AS promoted;
SET sift.test_fail_pages = 3;
SELECT sift.merge('mf_idx', true);
RESET sift.test_fail_pages;
SELECT sift._test_check('mf_idx') AS orphans;
SELECT sift.merge('mf_idx', true) >= 0 AS merged;
SELECT sift._test_maint_failures('mf_idx') AS failures;
SELECT sift.verify('mf_idx', true);

-- VACUUM does not fail with the step: the warning is the only trace, then the failure is counted
INSERT INTO mf (body) SELECT 'vac ' || big(i) FROM generate_series(1, 3) i;
SET sift.test_fail_pages = 3;
VACUUM mf;
RESET sift.test_fail_pages;
SELECT sift._test_maint_failures('mf_idx') AS failures;
SELECT sift._test_check('mf_idx') AS orphans;
VACUUM mf;
SELECT sift._test_maint_failures('mf_idx') AS failures;
SELECT sift.verify('mf_idx', true);

-- inserts reach the point where they would merge for room (112 of 128 entries) with every step
-- failing: they go on, and the failing step is tried once, not at every seal
CREATE TABLE mr (id serial, body text) WITH (autovacuum_enabled = off);
CREATE INDEX mr_idx ON mr USING sift (body) WITH (write_buffer_limit = 64);
SET sift.test_fail_pages = 1;
SET client_min_messages = error;
DO $$
BEGIN
	FOR i IN 1..118 LOOP
		INSERT INTO mr (body) SELECT 'room ' || big(1000 + i);
	END LOOP;
END $$;
RESET sift.test_fail_pages;
SET client_min_messages = warning;
SELECT count(*) >= 112 AS directory_nearly_full FROM sift.segments('mr_idx');
SELECT sift._test_maint_failures('mr_idx') BETWEEN 1 AND 3 AS backed_off;
SELECT count(*) FROM mr WHERE body @@> 'room';
SELECT sift.promote('mr_idx') >= 0 AS promoted;
SELECT sift._test_maint_failures('mr_idx') AS failures;
SELECT sift._test_check('mr_idx') AS orphans;
SELECT sift.verify('mr_idx', true);

DROP TABLE mf, mr;
DROP FUNCTION big(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
