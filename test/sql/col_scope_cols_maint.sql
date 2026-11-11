-- Maintenance of a scope index that declares cols(..): the scope segments of such an index keep only the listed columns, the member key and the column the units are cut by (the first
-- date or timestamp column), so a leaf the primary key stands for, or any other column left out, has no chunk in their units. VACUUM, a promotion, a rewrite, a refresh, a merge and REINDEX
-- read those units as they are: a leaf with no chunk is NULL in every row and is not a corruption. Each subset of columns (with and without the primary key, the member key, the time
-- column, a text and a float column) takes deletes and updates and then every maintenance step; after each one the scope read has the md5 of the plain join, the scope segments cover the
-- rows, and col_verify is clean. The pair relation has its journal index; REINDEX of the scope index follows its creation (it takes the stamps).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id smallint NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 16 FROM generate_series(1, 300) m, generate_series(0, 3) j ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
VACUUM (FREEZE, ANALYZE) tenant_scope;

-- the md5 of the rows of each statement through the scope read and through the plain join, for three tenants
CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
	(1, 'SELECT count(*) FROM events e WHERE @'),
	(2, 'SELECT count(*), sum(v), min(created_at), max(created_at) FROM events e WHERE @'),
	(3, 'SELECT kind, count(*) FROM events e WHERE @ GROUP BY kind ORDER BY kind'),
	(4, 'SELECT id, v FROM events e WHERE @ ORDER BY id'),
	(5, 'SELECT label, count(*), sum(w) FROM events e WHERE @ GROUP BY label ORDER BY label');
CREATE FUNCTION state(OUT same_md5 boolean, OUT covered boolean, OUT verified boolean) LANGUAGE plpgsql AS $$
DECLARE s record; a text; b text;
BEGIN
	same_md5 := true;
	FOR s IN SELECT * FROM shapes, generate_series(2, 14, 6) k LOOP
		SET LOCAL sift.enable_agg = on;
		EXECUTE 'SELECT md5(coalesce(json_agg(t)::text, '''')) FROM (' ||
				replace(s.q, '@', format('sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, %s)', s.k)) || ') t' INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE 'SELECT md5(coalesce(json_agg(t)::text, '''')) FROM (' ||
				replace(s.q, '@', format('e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s)', s.k)) || ') t' INTO b;
		same_md5 := same_md5 AND a = b;
	END LOOP;
	SET LOCAL sift.enable_agg = on;
	covered := sift._test_col_scope_covered('events_col', 'tenant_id');
	PERFORM sift.col_verify('events_col', true);
	verified := true;
END $$;

-- the subsets: what the declaration names, of the index (id, dim_a_id, created_at, kind, label, w, v)
CREATE TABLE subsets (n serial, cols text);
INSERT INTO subsets (cols) VALUES
	('v'), ('kind, v'), ('created_at'), ('created_at, v'), ('id, v'), ('id, created_at'), ('dim_a_id, v'), ('dim_a_id, created_at, kind'), ('label, w'), ('w, v'),
	('id, dim_a_id, created_at, kind, label, w, v');
CREATE TABLE edits (n serial, name text, stmt text);
INSERT INTO edits (name, stmt) VALUES
	('delete', 'DELETE FROM events WHERE id % 7 = 0'),
	('update', 'UPDATE events SET v = v + 1, dim_a_id = 1 + dim_a_id % 300 WHERE id % 5 = 0');
-- the statements of one case (@cols@ the subset, @edit@ the edit, @name@ its name, @step@ what a check follows); VACUUM cannot run in a function or with other statements
CREATE TABLE steps (n serial, step text, stmt text);
INSERT INTO steps (step, stmt) VALUES
	(NULL, 'DROP TABLE IF EXISTS events; CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamp, kind smallint NOT NULL, label text, w float8, v int NOT NULL, pad text)
		WITH (autovacuum_enabled = off);
	  INSERT INTO events SELECT i, 1 + (i * 7919) % 300, DATE ''2026-01-01'' + (i * 7919) % 300, i % 7, ''l'' || i % 13, i % 100 / 4.0, i % 10, repeat(''x'', 60)
		FROM generate_series(1, 4000) i;
	  CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, label, w, v)
		WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(@cols@)'', track_changes = ''off''); REINDEX INDEX events_col'),
	(NULL, 'VACUUM (FREEZE, ANALYZE) events'),
	('built', NULL),
	(NULL, '@edit@; INSERT INTO events SELECT 4000 + i, 1 + i % 300, DATE ''2026-02-01'' + i % 60, i % 7, ''m'' || i % 5, i % 10, i % 10, ''y'' FROM generate_series(1, 300) i'),
	('edited', NULL),
	(NULL, 'VACUUM events'),
	('vacuumed', NULL),
	(NULL, 'DO $x$ BEGIN PERFORM sift.col_promote(''events_col''); END $x$'),
	('promoted', NULL),
	(NULL, 'DELETE FROM events WHERE id % 3 = 0'),
	(NULL, 'VACUUM events'),
	(NULL, 'DO $x$ BEGIN PERFORM sift.col_maintain(''events_col''); END $x$'),
	('maintained', NULL),
	(NULL, 'DO $x$ BEGIN PERFORM sift.col_refresh(''events_col''); END $x$'),
	('refreshed', NULL),
	(NULL, 'DO $x$ BEGIN PERFORM sift.col_merge(''events_col'', true); END $x$'),
	('merged', NULL),
	(NULL, 'REINDEX INDEX events_col'),
	('reindexed', NULL);
UPDATE steps SET stmt = 'INSERT INTO results SELECT ''@cols@'', ''@name@'', ''@step@'', s.* FROM state() s' WHERE stmt IS NULL;

CREATE TABLE results (subset text, edit text, step text, same_md5 boolean, covered boolean, verified boolean);
\set ECHO none
SELECT replace(replace(replace(replace(s.stmt, '@cols@', c.cols), '@edit@', e.stmt), '@name@', e.name), '@step@', coalesce(s.step, ''))
	FROM subsets c, edits e, steps s ORDER BY c.n, e.n, s.n \gexec
\set ECHO all

-- every subset, edit and step is checked, and none differs from the plain join, loses the cover or fails col_verify
SELECT count(*) AS checks, count(DISTINCT subset) AS subsets, count(DISTINCT edit) AS edits, count(DISTINCT step) AS steps FROM results;
SELECT * FROM results WHERE NOT (same_md5 AND covered AND verified);

DROP TABLE events, shapes, subsets, edits, steps, results, tenant_scope;
DROP FUNCTION state();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
