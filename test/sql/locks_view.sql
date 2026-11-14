-- sift.locks names the locks of pg_sift: the advisory locks of the extension, and nothing else (the page locks of an index are read in test/t/col_worker_busy_lock.sh, which holds one)
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE lk (id int PRIMARY KEY, body text, g int);
INSERT INTO lk SELECT g, 'alpha ' || g, g % 3 FROM generate_series(1, 500) g;
CREATE INDEX lk_bm ON lk USING sift (body);
CREATE INDEX lk_col ON lk USING sift_col (id, g);

-- nothing is held
SELECT count(*) AS held FROM sift.locks;

-- the key spaces of the extension that a session can take: a rewrite slot and the orphan sweep (a lock of another key is no concern of the view)
SELECT pg_advisory_lock(20983, 1397948416), pg_advisory_lock(20983, 1397948417), pg_advisory_lock(20984, 1), pg_advisory_lock(20983, 7), pg_advisory_lock(1, 1);
SELECT kind, detail, relation, mode, granted, waiting_since IS NULL AS not_waiting, pid = pg_backend_pid() AS mine FROM sift.locks ORDER BY kind, detail;
SELECT pg_advisory_unlock_all();

-- the view hides the locks of tables the caller cannot look at
CREATE ROLE lk_none;
SELECT pg_advisory_lock(20983, 1397948416);
SET SESSION AUTHORIZATION lk_none;
SELECT kind, detail, relation FROM sift.locks;
RESET SESSION AUTHORIZATION;
SELECT pg_advisory_unlock_all();
DROP ROLE lk_none;

DROP TABLE lk;
DROP EXTENSION pg_sift;
