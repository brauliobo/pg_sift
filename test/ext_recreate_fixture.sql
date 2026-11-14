-- ext_recreate: one round of the objects a backend that lives across DROP EXTENSION and CREATE EXTENSION must find again: the session keeps the library, and
-- with it whatever it remembered of the catalog of the extension that was dropped.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
DROP TABLE IF EXISTS re_t, re_doc;

CREATE TABLE re_t (id int PRIMARY KEY, g int, uid uuid, url text) WITH (autovacuum_enabled = off);
INSERT INTO re_t SELECT i, i % 10, ('00000000-0000-0000-0000-' || lpad(i::text, 12, '0'))::uuid, md5(i::text) FROM generate_series(1, 5000) i;
CREATE INDEX re_t_a ON re_t USING sift_col (id, g, uid, url);
CREATE INDEX re_t_b ON re_t USING sift_col (id, uid, url);
CREATE TABLE re_doc (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO re_doc SELECT i, 'common word' || i % 10 || ' tail' || i FROM generate_series(1, 2000) i;
CREATE INDEX re_doc_body ON re_doc USING sift (body);
VACUUM (ANALYZE) re_t, re_doc;

-- the scans: the aggregate pushdown, a read of the index, a search; the plan names the node and the rows are those of the heap
SELECT plan_has('SELECT g, count(*), sum(id) FROM re_t GROUP BY g', 'SiftAgg') AS agg_pushdown;
SELECT plan_has('SELECT id FROM re_t WHERE g = 3', 'Index Only Scan using re_t_') AS col_index_read;
SELECT plan_has($$SELECT id FROM re_doc WHERE body @@> 'word3' ORDER BY sift.score(ctid) DESC, id LIMIT 3$$, 'Sift') AS search;
SELECT arm, used, slug FROM sift.explain_why('SELECT id FROM re_t WHERE g = 3');
SELECT arm, used FROM sift.explain_why('SELECT g, count(*) FROM re_t GROUP BY g');
SELECT g, count(*), sum(id) FROM re_t GROUP BY g ORDER BY g LIMIT 3;
SELECT id FROM re_doc WHERE body @@> 'word3' ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- the event triggers: a build of an index, a drop of one, and a DDL that the table triggers check
CREATE INDEX re_t_c ON re_t USING sift_col (id, g);
DROP INDEX re_t_c;
ALTER TABLE re_t ADD COLUMN extra int;
INSERT INTO re_t (id, g, uid, url) VALUES (5001, 1, '00000000-0000-0000-0000-000000005001', 'x');
SELECT plan_has('SELECT g, count(*), sum(id) FROM re_t GROUP BY g', 'SiftAgg') AS agg_after_ddl;
