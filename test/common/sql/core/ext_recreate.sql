-- ext_recreate: one round of the objects a backend that lives across DROP EXTENSION and CREATE EXTENSION must find again: the session keeps the library, and
-- with it whatever it remembered of the catalog of the extension that was dropped.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
DROP TABLE IF EXISTS re_t, re_doc, re_ev, re_pairs;

CREATE TABLE re_t (id int PRIMARY KEY, g int, uid uuid, url text) WITH (autovacuum_enabled = off);
INSERT INTO re_t SELECT i, i % 10, ('00000000-0000-0000-0000-' || lpad(i::text, 12, '0'))::uuid, md5(i::text) FROM generate_series(1, 5000) i;
CREATE INDEX re_t_a ON re_t USING sift_col (id, g, uid, url);
CREATE INDEX re_t_b ON re_t USING sift_col (id, uid, url);
CREATE TABLE re_doc (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO re_doc SELECT i, 'common word' || i % 10 || ' tail' || i FROM generate_series(1, 2000) i;
CREATE INDEX re_doc_body ON re_doc USING sift (body);
CREATE TABLE re_pairs (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s)) WITH (autovacuum_enabled = off);
INSERT INTO re_pairs SELECT i, 1 + i % 5 FROM generate_series(1, 300) i;
CREATE INDEX re_pairs_journal ON re_pairs USING sift_col (m, s) WITH (track_changes = 'ctid');
CREATE TABLE re_ev (id int PRIMARY KEY, m int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO re_ev SELECT i, 1 + i % 300, i % 17 FROM generate_series(1, 3000) i;
CREATE INDEX re_ev_col ON re_ev USING sift_col (id, m, v) WITH (scope = 's: m -> re_pairs(m, s)');
SELECT sift.projection_create('re_proj', 're_t', ARRAY['g']);
SELECT sift.projection_index('re_proj_ix', ARRAY['re_proj']);
VACUUM (ANALYZE) re_t, re_doc, re_ev, re_pairs;

-- the scans: the aggregate pushdown, a read of the index, a search, a scope read; the plan names the node and the rows are those of the heap
SELECT plan_has('SELECT g, count(*), sum(id) FROM re_t GROUP BY g', 'SiftAgg') AS agg_pushdown;
SELECT plan_has('SELECT id FROM re_t WHERE g = 3', 'Index Only Scan using re_t_') AS col_index_read;
SELECT plan_has($$SELECT id FROM re_doc WHERE body @@> 'word3' ORDER BY sift.score(ctid) DESC, id LIMIT 3$$, 'Sift') AS search;
SELECT arm, used, slug FROM sift.explain_why('SELECT id FROM re_t WHERE g = 3');
SELECT arm, used FROM sift.explain_why('SELECT g, count(*) FROM re_t GROUP BY g');
SELECT g, count(*), sum(id) FROM re_t GROUP BY g ORDER BY g LIMIT 3;
SELECT id FROM re_doc WHERE body @@> 'word3' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT p.s, count(*), sum(e.v) FROM re_ev e JOIN re_pairs p ON p.m = e.m GROUP BY p.s ORDER BY p.s;

-- the scope leaf, the projection
SELECT path FROM sift.col_leaves('re_ev_col') WHERE path LIKE '%#scope';
SELECT projection, index, state FROM sift.projection_status WHERE projection = 're_proj';

-- the event triggers: a build of an index, a drop of one, and a DDL that the table triggers check
CREATE INDEX re_t_c ON re_t USING sift_col (id, g);
DROP INDEX re_t_c;
ALTER TABLE re_t ADD COLUMN extra int;
INSERT INTO re_t (id, g, uid, url) VALUES (5001, 1, '00000000-0000-0000-0000-000000005001', 'x');
SELECT plan_has('SELECT g, count(*), sum(id) FROM re_t GROUP BY g', 'SiftAgg') AS agg_after_ddl;
