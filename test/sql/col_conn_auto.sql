-- CREATE INDEX and REINDEX connect a sift_col index to the sift_col indexes its validated foreign keys reference (docs/COL-CONNECT.md): the choice is written into the option as 'auto: ..',
-- named in a NOTICE, and turned off by connect = 'off' or sift.col_auto_connect.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text, price numeric(10, 2), notes text, tags text[]) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10, 'l' || i, i, repeat('n', 200), ARRAY['x'] FROM generate_series(1, 500) i;
ANALYZE dim_a;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label, price, notes, tags);
CREATE TABLE dim_b (id int PRIMARY KEY, zone text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim_b SELECT i, 'z' || i % 5 FROM generate_series(1, 50) i;
CREATE INDEX dim_b_col ON dim_b USING sift_col (id, zone);
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL REFERENCES dim_a, dim_b_id int NOT NULL REFERENCES dim_b, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 500, 1 + i % 50, i FROM generate_series(1, 3000) i;
\set VERBOSITY default
-- one index, two foreign keys: two connections that carry nothing
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, dim_b_id, v);
SELECT ordinal, fk, parent, declaration, automatic FROM sift.col_connections WHERE child = 'facts_col'::regclass ORDER BY ordinal;
SELECT regexp_replace(c, '^connect=', '') AS option FROM pg_class, unnest(reloptions) c WHERE relname = 'facts_col' AND c LIKE 'connect=%';
SELECT DISTINCT format FROM sift.col_segments('facts_col') WHERE kind = 'immutable';
-- the option wins: off, a named connection, and the setting
CREATE INDEX facts_off ON facts USING sift_col (id, dim_a_id, v) WITH (connect = 'off');
CREATE INDEX facts_named ON facts USING sift_col (id, dim_a_id, v) WITH (connect = 'dim_a_id -> dim_a(id) carries label');
SET sift.col_auto_connect = off;
CREATE INDEX facts_guc ON facts USING sift_col (id, dim_a_id, v);
CREATE INDEX facts_asked ON facts USING sift_col (id, dim_b_id, v) WITH (connect = 'auto');
RESET sift.col_auto_connect;
SELECT child, ordinal, fk, declaration, automatic FROM sift.col_connections WHERE child IN ('facts_off'::regclass, 'facts_named'::regclass, 'facts_guc'::regclass, 'facts_asked'::regclass)
ORDER BY child::text;
-- a foreign key that does not connect: no constraint, not validated, composite, not an integer, a parent without the index, a column outside the index (said in a NOTICE when the parent has a sift_col index)
SET client_min_messages = notice;
CREATE TABLE nofk (id int PRIMARY KEY, dim_a_id int NOT NULL);
CREATE INDEX nofk_col ON nofk USING sift_col (id, dim_a_id);
CREATE TABLE notvalid (id int PRIMARY KEY, dim_a_id int NOT NULL);
ALTER TABLE notvalid ADD CONSTRAINT notvalid_fk FOREIGN KEY (dim_a_id) REFERENCES dim_a NOT VALID;
CREATE INDEX notvalid_col ON notvalid USING sift_col (id, dim_a_id);
CREATE TABLE dim_c (id int, k int, label text, PRIMARY KEY (id, k));
CREATE INDEX dim_c_col ON dim_c USING sift_col (id, k, label);
CREATE TABLE composite (id int PRIMARY KEY, c_id int NOT NULL, c_k int NOT NULL, FOREIGN KEY (c_id, c_k) REFERENCES dim_c);
CREATE INDEX composite_col ON composite USING sift_col (id, c_id, c_k);
CREATE TABLE dim_t (code text PRIMARY KEY, label text);
CREATE INDEX dim_t_col ON dim_t USING sift_col (code, label);
CREATE TABLE bytext (id int PRIMARY KEY, code text NOT NULL REFERENCES dim_t);
CREATE INDEX bytext_col ON bytext USING sift_col (id, code);
CREATE TABLE dim_n (id int PRIMARY KEY, label text);
CREATE TABLE noparent (id int PRIMARY KEY, n_id int NOT NULL REFERENCES dim_n);
CREATE INDEX noparent_col ON noparent USING sift_col (id, n_id);
CREATE TABLE outside (id int PRIMARY KEY, dim_a_id int NOT NULL REFERENCES dim_a, v int);
CREATE INDEX outside_col ON outside USING sift_col (id, v);
RESET client_min_messages;
SELECT count(*) AS connected FROM sift.col_connections WHERE child IN ('nofk_col'::regclass, 'notvalid_col'::regclass, 'composite_col'::regclass, 'bytext_col'::regclass, 'noparent_col'::regclass, 'outside_col'::regclass);
-- validating the constraint and a REINDEX connects it; REINDEX keeps what was chosen, and CONCURRENTLY too
ALTER TABLE notvalid VALIDATE CONSTRAINT notvalid_fk;
REINDEX INDEX notvalid_col;
REINDEX INDEX facts_col;
REINDEX INDEX CONCURRENTLY facts_col;
SELECT child, ordinal, declaration, automatic FROM sift.col_connections WHERE child IN ('notvalid_col'::regclass, 'facts_col'::regclass) ORDER BY child::text, ordinal;
-- the option turned off, then REINDEX: nothing is connected, and the parent can be dropped
ALTER INDEX facts_col SET (connect = 'off');
REINDEX INDEX facts_col;
SELECT count(*) AS connections FROM sift.col_connections WHERE child = 'facts_col'::regclass;
-- an index that was connected by itself behaves as one connected by hand: the parent index is under it
DROP INDEX dim_a_col;
DROP INDEX dim_b_col;
DROP TABLE facts, nofk, notvalid, composite, dim_c, bytext, dim_t, noparent, dim_n, outside, dim_a, dim_b CASCADE;
DROP EXTENSION pg_sift;
