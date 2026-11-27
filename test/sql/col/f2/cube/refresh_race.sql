-- The refresh of a summary after a compaction, a flush and an edit of the pairs, all at once. VACUUM builds again only what changed: the nodes that hold a retired granule or a changed member and the
-- granules of the new parts, in one pass (the "both" counter of the refreshes), and never the whole summary again when the journal names the members, even for a bulk synchronisation of
-- thousands of members. A summary opened while a compaction commits is read again, and when its parts are gone the read is the join's: no statement fails. Every key's rows are those of the join.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.test_create_format1 = off;

CREATE TABLE members (member int NOT NULL, panel int NOT NULL, PRIMARY KEY (member, panel)) WITH (autovacuum_enabled = false);
INSERT INTO members SELECT m, 1 + m % 20 FROM generate_series(1, 6000) m;
CREATE INDEX members_journal ON members USING sift_col (member, panel) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, member int, at timestamptz, n int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + (i * 37) % 6000, timestamptz '2026-01-01' + i * interval '11 minutes', i % 90 FROM generate_series(1, 30000) i;
CREATE INDEX facts_s ON facts USING sift_col (id, member, at, n) WITH (scope = 'panel: member -> members(member, panel)', track_changes = 'ctid');
ANALYZE members;
ANALYZE facts;

CREATE FUNCTION key_diff(idx regclass) RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM sift._test_f2_scope_keys(idx, 0) k
	FULL JOIN (SELECT s.panel AS scope_key, count(*) AS rows FROM facts f JOIN members s ON s.member = f.member GROUP BY 1) b USING (scope_key)
	WHERE k.rows IS DISTINCT FROM b.rows $$;
CREATE FUNCTION counters(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT substring(sift._test_f2_scope_info(idx, 0) from 'refreshes=[0-9/]+ combined=[0-9]+') $$;
SELECT key_diff('facts_s') AS key_differences, counters('facts_s') AS refreshes_after_build;

-- rows in new parts, a compaction, and a bulk synchronisation of members, in the same cycle
INSERT INTO facts SELECT 40000 + i, 1 + (i * 41) % 6000, timestamptz '2026-04-01' + i * interval '13 minutes', i % 60 FROM generate_series(1, 6000) i;
DELETE FROM facts WHERE id > 40000 AND id % 7 = 0;
DELETE FROM members WHERE member % 6 = 0 AND panel < 10;
INSERT INTO members SELECT m, 1 + (m * 3) % 20 FROM generate_series(1, 5000) m ON CONFLICT DO NOTHING;
UPDATE members SET panel = panel % 20 + 1 WHERE member BETWEEN 2000 AND 2400;
SELECT sift._test_f2_scope_current('facts_s', 0) AS current_before_vacuum;
VACUUM facts;
SELECT sift._test_f2_scope_current('facts_s', 0) AS current_after_vacuum, key_diff('facts_s') AS key_differences;
SELECT (regexp_match(counters('facts_s'), 'refreshes=(\d+)/(\d+)/(\d+) combined=(\d+)'))[1]::int AS full_rebuilds_after_build_is_zero_or_one,
       (regexp_match(counters('facts_s'), 'combined=(\d+)'))[1]::int >= 1 AS combined_refresh_taken;
-- the answers of the node after the refresh
CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP plan := plan || l; END LOOP;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6Scope%' THEN 'scope' ELSE 'normal' END) || ' ' || q;
END $$;
SELECT answer(format('SELECT count(*), sum(f.n), min(f.id) FROM facts f JOIN members s ON s.member = f.member WHERE s.panel = %s', p)) FROM generate_series(1, 20, 4) p;
SELECT answer('SELECT s.panel, count(*), sum(f.n) FROM facts f JOIN members s ON s.member = f.member GROUP BY s.panel');

-- the journal names a handful of members and the rest stays: a refresh that is not a rebuild
UPDATE members SET panel = 1 + (panel % 20) WHERE member IN (10, 11, 12, 13);
INSERT INTO facts SELECT 60000 + i, 1 + i % 6000, timestamptz '2026-05-01', 1 FROM generate_series(1, 20000) i;
VACUUM facts;
SELECT sift._test_f2_scope_current('facts_s', 0) AS current_after_second_vacuum, key_diff('facts_s') AS key_differences, counters('facts_s') AS refreshes;
-- statements run while the summary is stale (rows written, no refresh yet) are the join's or the summary's with the change overlaid, never an error
INSERT INTO facts SELECT 90000 + i, 1 + i % 6000, timestamptz '2026-05-02', 2 FROM generate_series(1, 40) i;
SELECT answer(format('SELECT count(*), sum(f.n) FROM facts f JOIN members s ON s.member = f.member WHERE s.panel = %s', p)) FROM generate_series(1, 20, 7) p;
-- compaction of the entity parts and a read of the old summary: the part list the summary names is gone
SELECT sift._test_f2_scope_current('facts_s', 0) AS current_stale;
SELECT answer('SELECT count(*), sum(f.n) FROM facts f JOIN members s ON s.member = f.member WHERE s.panel = 3');
VACUUM FULL facts;
SELECT answer('SELECT count(*), sum(f.n) FROM facts f JOIN members s ON s.member = f.member WHERE s.panel = 3');
SELECT sift._test_f2_scope_refresh('facts_s') >= 0 AS refresh_ran, key_diff('facts_s') AS key_differences;
-- a read-only transaction reads and writes nothing
BEGIN READ ONLY;
SELECT answer('SELECT count(*), sum(f.n) FROM facts f JOIN members s ON s.member = f.member WHERE s.panel = 5');
COMMIT;
DROP TABLE facts, members;
DROP FUNCTION answer(text), counters(regclass), key_diff(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
