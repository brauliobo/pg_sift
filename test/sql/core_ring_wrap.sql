-- The caches of a few entries that replace the oldest entry in turn (core/ring.h) count their slots in 32 unsigned bits: a signed count passed INT_MAX into undefined behavior and then
-- indexed the array of a memo before its start. sift._test_ring_walk takes slots of a ring across 2^31 and 2^32 for each size in use; sift._test_ring_start puts the count of every ring three slots
-- from the wrap of 32 bits, and the statements that fill each cache past its size follow: the answers are those of the plain statements, and every ring that took slots crossed the wrap.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

SELECT slots, sift._test_ring_walk(slots, 100) AS walks_across_the_wraps FROM unnest(ARRAY[1, 2, 4, 8, 16, 32]) slots;

SELECT sift._test_ring_start(4294967293);

-- scope: the memos of the catalogs (btree, journal, legs, specs, track, made) and of the prices (rows, units), each over more indexes than a memo holds, with the pairs changed since the build
SELECT format(v.q, g) FROM generate_series(1, 10) g, LATERAL (VALUES
 (1, 'CREATE TABLE pairs%1$s (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)'),
 (2, 'INSERT INTO pairs%1$s SELECT d, 1 + (d + j * 3) %% 8 FROM generate_series(1, 64) d, generate_series(0, 1) j'),
 (3, 'CREATE TABLE events%1$s (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false)'),
 (4, 'INSERT INTO events%1$s SELECT i, 1 + (i * 7) %% 64, timestamptz ''2026-01-01'' + i * interval ''9 minutes'', i %% 10 FROM generate_series(1, 400) i'),
 (5, 'CREATE INDEX events%1$s_col ON events%1$s USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = ''tenant_id: dim_a_id -> pairs%1$s(dim_a_id, tenant_id)'')'),
 (6, 'VACUUM (FREEZE, ANALYZE) events%1$s, pairs%1$s')) v(o, q) ORDER BY g, o \gexec

-- read before and after the pairs change: the price of a read is asked for when the pairs changed since the segments were filled, and the patch is not forced
SET sift.enable_agg = on;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;
SELECT format('SELECT count(*), sum(amount) FROM events%1$s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs%1$s m WHERE m.tenant_id = 3)', g) FROM generate_series(1, 10) g \gexec
SELECT format('INSERT INTO pairs%1$s VALUES (70, 3), (71, 3)', g) FROM generate_series(1, 10) g \gexec
SELECT format('SELECT count(*), sum(amount) FROM events%1$s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs%1$s m WHERE m.tenant_id = 3)', g) FROM generate_series(1, 10) g \gexec
SET sift.test_scope_patch = off;
SELECT format('SELECT count(*), sum(amount) FROM events%1$s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs%1$s m WHERE m.tenant_id = 3)', g) FROM generate_series(1, 10) g \gexec
RESET sift.test_scope_patch;
RESET sift.enable_agg;
RESET sift.agg_force;

CREATE FUNCTION scope_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO b;
	RETURN a = b;
END $$;

SELECT bool_and(scope_same(format(v.q, g))) AS scope_reads_equal_the_joins
FROM generate_series(1, 10) g, (VALUES
 ('SELECT m.tenant_id AS k, count(*), sum(amount) FROM events%1$s f JOIN pairs%1$s m ON m.dim_a_id = f.dim_a_id GROUP BY 1'),
 ('SELECT count(*), sum(amount) FROM events%1$s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs%1$s m WHERE m.tenant_id = 3)'),
 ('SELECT count(*), sum(amount) FROM events%1$s f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs%1$s m WHERE m.tenant_id IN (3, 5))')) v(q);

-- scope of two paths: the groups of the keys, six keys over a memo of four
CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id);
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_by_group ON memberships (group_id, dim_a_id);
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 60) m;
INSERT INTO grants (tenant_id, group_id) SELECT g, g FROM generate_series(1, 6) g;
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10 FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) facts, grants, memberships;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
VACUUM (FREEZE, ANALYZE) facts;
INSERT INTO memberships (group_id, dim_a_id) VALUES (2, 55), (3, 56);
SET sift.test_scope_patch = on;
SELECT bool_and(scope_same(format('SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[%s]::int8[])', 1 + g % 6)))
	AS two_path_reads_equal_the_joins FROM generate_series(1, 12) g;
RESET sift.test_scope_patch;

-- the estimates of a BM25 index for twelve queries
CREATE TABLE bm (id int PRIMARY KEY, tenant int, body text);
INSERT INTO bm SELECT i, i % 40, 'word' || (i % 30) || ' filler' || i FROM generate_series(1, 3000) i;
CREATE INDEX bm_body ON bm USING sift (body);
ANALYZE bm;
SELECT format('EXPLAIN (COSTS OFF) SELECT count(*) FROM bm WHERE body @@> ''word%s'' AND tenant < 20', g) FROM generate_series(1, 12) g \gexec

-- the verdicts of eight transactions in a tombstone log
CREATE TABLE t (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10 FROM generate_series(1, 2000) i;
CREATE INDEX t_c ON t USING sift_col (id, v) WITH (track_changes = 'ctid');
VACUUM (ANALYZE) t;
SELECT format('DELETE FROM t WHERE id = %s', g) FROM generate_series(1, 8) g \gexec
SELECT scope_same('SELECT count(*), sum(v) FROM t') AS tombstones_equal_the_heap;

-- the tests of a scan over twenty segments, and the order codes of their dictionaries
CREATE TABLE s (id int PRIMARY KEY, name text, v int) WITH (autovacuum_enabled = false);
CREATE INDEX s_c ON s USING sift_col (id, name, v);
SELECT q FROM generate_series(1, 20) g, LATERAL (VALUES
 (1, format('INSERT INTO s SELECT %1$s * 1000 + i, ''name'' || (i %% 50) || ''-'' || %1$s, i %% 7 FROM generate_series(1, 600) i', g)),
 (2, 'SELECT sift.col_promote(''s_c'')')) v(o, q) ORDER BY g, o \gexec
SELECT scope_same($$SELECT count(*), sum(v) FROM s WHERE name IN ('name1-3', 'name2-4', 'name5-5', 'name7-6', 'name9-7', 'name11-8', 'name13-9', 'name15-10', 'name17-11', 'name19-12', 'name21-13', 'name23-14', 'name25-15', 'name27-16', 'name29-17', 'name31-18', 'name33-19', 'name35-20')$$)
	AS scan_tests_equal_the_heap;
SELECT scope_same('SELECT name, v FROM s ORDER BY name, v, id LIMIT 5') AS order_codes_equal_the_heap;

-- every ring that handed out slots crossed the wrap of its count
SELECT site, wraps >= 1 AS crossed_the_wrap FROM sift._test_ring_sites() ORDER BY site;
SELECT sift._test_ring_start(0);

DROP FUNCTION scope_same(text);
DROP TABLE s, t, bm, facts, grants, memberships;
DO $$ BEGIN FOR g IN 1..10 LOOP EXECUTE format('DROP TABLE events%1$s, pairs%1$s', g); END LOOP; END $$;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
