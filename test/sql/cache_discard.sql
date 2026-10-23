-- The index state a relcache entry caches (rd_amcache) is freed whenever the entry is reloaded, and a reload happens wherever a lock or a catalog read
-- accepts an invalidation. A server built with assertions can reload it at every such point (debug_discard_caches, which also overwrites freed memory),
-- so the code that holds a pointer into that state across one dies here, on a few rows, with no concurrent session. The other servers have no such
-- setting and the test runs as it is: the answers are the same. The rows are few, a reload at every catalog read costs about 30 ms per row.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE items (id int, kind int, grp text, val bigint, amt numeric(10,2), doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO items SELECT g, g % 6, 'g' || g % 30, CASE WHEN g % 7 = 0 THEN NULL ELSE g * 977 % 1000 END, g % 200 / 4.0,
	jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('c', 'a', 'score', g))) FROM generate_series(1, 600) g;
CREATE INDEX items_col ON items USING sift_col (id, kind, grp, val, amt, doc)
	WITH (write_buffer_limit = 4096, paths = 'doc.parts[]{c text, score float8}', cube_minmax = false);
CREATE TABLE events (id int, kind int, parent_id int, cat text, big bigint, val int) WITH (autovacuum_enabled = off);
INSERT INTO events SELECT g, (g / 100) % 20, g % 500, 'c' || g % 300, g::bigint * 1000, g % 1000 FROM generate_series(1, 1500) g;
CREATE INDEX events_col ON events USING sift_col (id sift.col_int4_ops, kind sift.col_int4_ops, parent_id sift.col_int4_ops, cat sift.col_text_ops, big sift.col_int8_ops, val sift.col_int4_ops)
	WITH (write_buffer_limit = 4096, key_columns = 'kind,parent_id,cat');
CREATE TABLE dims (id int, name text) WITH (autovacuum_enabled = off);
INSERT INTO dims SELECT g, 'n' || g FROM generate_series(1, 50) g;
ANALYZE events;
ANALYZE dims;
CREATE TABLE blobs (id int PRIMARY KEY, grp int, body text, doc jsonb) WITH (autovacuum_enabled = off);
INSERT INTO blobs SELECT i, i % 5, (SELECT string_agg(md5(i::text || j), ' ') FROM generate_series(1, 200) j),
	jsonb_build_object('v', CASE WHEN i % 6 = 0 THEN to_jsonb('8.5'::text) ELSE to_jsonb((i % 17) + 0.5) END) FROM generate_series(1, 60) i;
CREATE INDEX blobs_col ON blobs USING sift_col (id, grp, body, doc) WITH (paths = 'doc.v float8', write_buffer_limit = 4096);
ANALYZE blobs;
CREATE TABLE notes (id serial, body text, kind int) WITH (autovacuum_enabled = off);
INSERT INTO notes (body, kind) SELECT 'word' || g % 40 || ' item' || g % 7 || ' common', g % 5 FROM generate_series(1, 600) g;
CREATE INDEX notes_sift ON notes USING sift (body);
CREATE TABLE rows_t (id int, store text, total numeric, note text) USING sift_table;
SELECT current_setting('debug_discard_caches', true) IS NOT NULL AS discard \gset
\if :discard
SET debug_discard_caches = 1;
\endif

-- sift_col: rows with a value the index cannot store (a string in a float8 path) and with a JSON null flag the leaf in the directory
INSERT INTO items SELECT g, g % 6, 'g' || g % 30, g, 1.5, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('c', 'b', 'score', CASE WHEN g % 5 = 0 THEN to_jsonb('x'::text) ELSE to_jsonb(g) END))) FROM generate_series(601, 640) g;
SELECT sift.col_promote('items_col') >= 0 AS promoted;
INSERT INTO items SELECT g, g % 6, 'g' || g % 30, g, 2.5, 'null'::jsonb FROM generate_series(641, 660) g;
UPDATE items SET val = val + 1 WHERE id % 50 = 0;
DELETE FROM items WHERE id % 97 = 0;
VACUUM items;
SELECT count(*), sum(val) FROM items WHERE kind = 3;
SELECT kind, count(*), sum(amt) FROM items GROUP BY kind ORDER BY kind;
SELECT id FROM items ORDER BY val DESC NULLS LAST, id LIMIT 5;
SET enable_seqscan = off;
SELECT count(*) FROM items WHERE id BETWEEN 10 AND 400;
RESET enable_seqscan;
ALTER INDEX items_col SET (cube_minmax = true);
SELECT count(*) >= 0 AS refreshed FROM sift.col_refresh('items_col', 0);
SELECT count(*) >= 0 AS refreshed FROM sift.col_refresh('items_col', 2);
REINDEX INDEX items_col;
SELECT sum(rows) = (SELECT count(*) FROM items) AS all_rows FROM sift.col_segments('items_col');

-- planning an index scan of sift_col estimates the units the zones and the small-set summaries leave, with a statistics lookup for each clause
SET sift.enable_col_scan = on;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*) FROM events WHERE kind = 3;
SELECT count(*) FROM events WHERE parent_id IN (3, 7, 11) AND val < 500;
SELECT count(*) FROM events WHERE cat = 'c17' OR id = 5;
SELECT count(*) FROM events WHERE id BETWEEN 100 AND 900 AND kind IN (1, 2);
SELECT count(*) FROM events e JOIN dims d ON d.id = e.parent_id WHERE e.kind = 4;
RESET sift.enable_col_scan;
RESET enable_seqscan;
RESET enable_indexscan;

-- the aggregate scan reads the rows with a value the index cannot store again from the heap (a string in a float8 path), and the toasted text of the
-- row is read while its index row is built
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(body) IS NOT NULL) AS toasted FROM blobs;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.enable_agg = on;
SELECT grp, sum((doc ->> 'v')::float8), count(*) FROM blobs GROUP BY grp ORDER BY grp;
SET sift.enable_agg = off;
SELECT grp, sum((doc ->> 'v')::float8), count(*) FROM blobs GROUP BY grp ORDER BY grp;
RESET sift.enable_agg;
RESET seq_page_cost;
RESET random_page_cost;

-- sift (BM25): insert, promote, delete, update, vacuum, merge, queries
INSERT INTO notes (body, kind) SELECT 'late' || g % 9 || ' common word' || g % 40, g % 5 FROM generate_series(1, 40) g;
SELECT sift.promote('notes_sift') >= 0 AS promoted;
DELETE FROM notes WHERE id % 11 = 0;
UPDATE notes SET body = body || ' edited' WHERE id % 13 = 0;
VACUUM notes;
SELECT sift.merge('notes_sift', true) >= 0 AS merged;
SELECT count(*) FROM notes WHERE body @@> 'common word3';
SELECT id FROM notes WHERE body @@> 'common word3' ORDER BY sift.score(ctid) DESC, id LIMIT 5;
SELECT count(*) FROM notes WHERE body @@> '"common word3"' OR body @@> 'late4';
SELECT count(*) FROM notes WHERE body @@> 'edited' AND kind = 2;
REINDEX INDEX notes_sift;
SELECT count(*) FROM notes WHERE body @@> 'common';

-- sift_table
INSERT INTO rows_t SELECT g, 's' || g % 3, g * 1.5, 'n' || g FROM generate_series(1, 200) g;
UPDATE rows_t SET total = total + 1 WHERE id % 7 = 0;
DELETE FROM rows_t WHERE id % 10 = 0;
CREATE INDEX rows_t_id ON rows_t (id);
VACUUM rows_t;
SELECT count(*), sum(total) FROM rows_t;
SELECT store, count(*), sum(total) FROM rows_t GROUP BY store ORDER BY store;
SELECT id FROM rows_t ORDER BY total DESC, id LIMIT 4;
SELECT note FROM rows_t WHERE id = 77;
TRUNCATE rows_t;
INSERT INTO rows_t SELECT g, 's', g, 'x' FROM generate_series(1, 30) g;
SELECT count(*) FROM rows_t;

-- key_columns: the choice before the build and the census after it read catalogs while they hold the state of the index (the NOTICE is the path under test)
CREATE TABLE keyed (id int, parent_id int, owner_id int, ext_id int, kind int) WITH (autovacuum_enabled = off);
INSERT INTO keyed SELECT g, g % 400, g % 300, g, g % 5 FROM generate_series(1, 1500) g;
SET client_min_messages = notice;
CREATE INDEX keyed_col ON keyed USING sift_col (id, parent_id, owner_id, ext_id, kind);
SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'keyed_col' AND o LIKE 'key_columns=%';
REINDEX INDEX keyed_col;
SET client_min_messages = warning;
SELECT count(DISTINCT parent_id) FROM keyed;

\if :discard
RESET debug_discard_caches;
\endif
DROP TABLE items, events, dims, blobs, notes, rows_t, keyed;
DROP EXTENSION pg_sift;
