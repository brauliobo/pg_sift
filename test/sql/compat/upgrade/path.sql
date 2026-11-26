-- What an upgrade reads from the installed SQL: sift.index_format of the three access methods, the format columns of sift.col_index_status, the preflight
-- sift.upgrade_check() as an installed function, and sift.upgrade_commands() for a text index built by this release (nothing to REINDEX).
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
\set VERBOSITY terse

CREATE TABLE up (id int PRIMARY KEY, body text, g int, n int) WITH (autovacuum_enabled = false);
INSERT INTO up SELECT i, 'alpha' || i % 7 || ' beta' || i % 11, i % 9, i % 100 FROM generate_series(1, 3000) i;
CREATE INDEX up_text ON up USING sift (body);
CREATE INDEX up_keyword ON up USING sift (body) WITH (analyzer = keyword);
CREATE INDEX up_col ON up USING sift_col (id, g, n);
CREATE TABLE up_tbl (id int, g int) USING sift_table;
INSERT INTO up_tbl SELECT i, i % 9 FROM generate_series(1, 3000) i;
VACUUM up_tbl;

-- the metapage format of each access method; long_words only means something for a text index, the column formats read for the other two
SELECT c.relname, f.major, f.minor > 0 AS minor, f.long_words, f.reads ~ '^3 to [0-9]+$' AS reads
FROM pg_class c, LATERAL sift.index_format(c.oid) f
WHERE c.relname IN ('up_text', 'up_keyword', 'up_col', 'up_tbl') ORDER BY c.relname;

-- the format of the index, of its segments and the range this library reads
SELECT format ~ '^2\.[0-9]+$' AS format, min_segment_format <= max_segment_format AS ordered, library_reads ~ '^3 to [0-9]+$' AS reads
FROM sift.col_index_status WHERE index = 'up_col'::regclass;

-- the preflight is part of the extension and finds nothing wrong with a database of this release
SELECT object, kind, status FROM sift.upgrade_check() WHERE object IN ('pg_sift', 'up_text', 'up_keyword', 'up_col', 'up_tbl') ORDER BY object;
SELECT count(*) AS blockers FROM sift.upgrade_check() WHERE status = 'blocker' AND object !~ '^sift[.]';
-- a target release says what it could not read
SELECT object, status FROM sift.upgrade_check('3.0') WHERE object ~ '^up_' ORDER BY object;
-- going back to 4.3 with the SQL of 5.0: its event triggers call functions that 4.3 lacks and would fail every CREATE INDEX and DROP of the database
SELECT object, kind, status, advice ~ '^ALTER EVENT TRIGGER .* DISABLE' AS says_disable FROM sift.upgrade_check('4.3') WHERE kind = 'trigger' ORDER BY object;
SELECT pg_describe_object(classid, objid, objsubid) FROM pg_depend
WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND objid = 'sift.upgrade_check(text)'::regprocedure;

-- an extension older than 4.3.6 (the first 4.x release whose library refuses a 5.0 metapage with the format message) gets a warning that is no blocker;
-- 4.3.6 and later do not (the update to 5.0 sets the version, so this release never has the row)
SELECT count(*) AS rollback_rows_now FROM sift.upgrade_check() WHERE advice ~ '4\.3\.6';
BEGIN;
SELECT s.stmt FROM unnest(ARRAY['3.1', '4.3', '4.3.5', '4.3.6', '4.3.7', '4.4']) WITH ORDINALITY v(ver, n),
	LATERAL (VALUES (1, format('UPDATE pg_extension SET extversion = %L WHERE extname = ''pg_sift''', v.ver)),
					(2, format('SELECT %L AS extversion, object, kind, status, advice ~ ''every node \(standbys first\) before moving to 5\.0'' AS says_install FROM sift.upgrade_check() WHERE advice ~ ''4\.3\.6''', v.ver))) s(step, stmt)
ORDER BY v.n, s.step \gexec
ROLLBACK;

-- an index built by this release has no word cut at 255 bytes: nothing is left to REINDEX
SELECT kind, object, required FROM sift.upgrade_commands() WHERE object::text ~ '^up_';

-- the owner of the database is told how to preload the library, once; nothing is said when it is
SELECT kind, object, required, command ~ '^ALTER DATABASE .* SET session_preload_libraries = ' AS command FROM sift.upgrade_commands() WHERE kind = 'preload';
SET session_preload_libraries = 'pg_sift';
SELECT count(*) AS preload_rows FROM sift.upgrade_commands() WHERE kind = 'preload';
RESET session_preload_libraries;

-- event triggers of the extension that are disabled are listed as required, with the command that enables them (a superuser's row; nothing when all are on)
SELECT count(*) AS hooks_rows_when_enabled FROM sift.upgrade_commands() WHERE kind = 'hooks';
SELECT sift.ddl_hooks_enable(false) > 0 AS disabled;
SELECT kind, object, required, command, reason ~ '^[0-9]+ of [0-9]+ event triggers of pg_sift are disabled' AS says_count FROM sift.upgrade_commands() WHERE kind = 'hooks';
SELECT sift.ddl_hooks_enable(true) > 0 AS enabled;
SELECT count(*) AS hooks_rows_after FROM sift.upgrade_commands() WHERE kind = 'hooks';

DROP TABLE up, up_tbl;
DROP EXTENSION pg_sift CASCADE;
