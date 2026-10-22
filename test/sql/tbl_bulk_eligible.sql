-- Which sift_table qualifies for the bulk load into column groups: a table nothing reads while the load runs.
-- The refusal names why a table keeps the write buffers; a publication is covered by test/t/019_tbl_bulk_groups.pl (wal_level = logical).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE plain (id int, v text) USING sift_table;
CREATE TABLE indexed (id int, v text) USING sift_table;
CREATE INDEX indexed_id ON indexed (id);
CREATE INDEX indexed_expr ON indexed ((id % 7), v);
CREATE TABLE with_bm25 (id int, body text) USING sift_table;
CREATE INDEX with_bm25_idx ON with_bm25 USING sift (body);

CREATE TABLE pk (id int PRIMARY KEY, v text) USING sift_table;
CREATE TABLE uniq (id int, v text) USING sift_table;
CREATE UNIQUE INDEX uniq_v ON uniq (v);
CREATE TABLE uniq_partial (id int, v text) USING sift_table;
CREATE UNIQUE INDEX uniq_partial_id ON uniq_partial (id) WHERE id > 10;
CREATE TABLE excl (id int, v text, EXCLUDE USING btree (id WITH =)) USING sift_table;

CREATE FUNCTION nothing() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
CREATE TABLE trig_row (id int, v text) USING sift_table;
CREATE TRIGGER trig_row_before BEFORE INSERT ON trig_row FOR EACH ROW EXECUTE FUNCTION nothing();
CREATE TABLE trig_stmt (id int, v text) USING sift_table;
CREATE TRIGGER trig_stmt_after AFTER INSERT ON trig_stmt FOR EACH STATEMENT EXECUTE FUNCTION nothing();
CREATE TABLE heap_parent (id int PRIMARY KEY);
CREATE TABLE fk_child (id int, parent int REFERENCES heap_parent (id)) USING sift_table;
CREATE TABLE fk_parent (id int PRIMARY KEY, v text) USING sift_table;
CREATE TABLE heap_child (id int, parent int REFERENCES fk_parent (id));
CREATE TABLE fk_self (id int PRIMARY KEY, up int REFERENCES fk_self (id)) USING sift_table;

SELECT c.relname, sift._test_tbl_bulk_refusal(c.oid) AS refusal
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE a.amname = 'sift_table' AND c.relkind = 'r' AND c.relnamespace = 'public'::regnamespace
ORDER BY (sift._test_tbl_bulk_refusal(c.oid) IS NOT NULL), c.relname;

-- a trigger that is dropped, or a table that is dropped from a foreign key, qualifies again
DROP TRIGGER trig_row_before ON trig_row;
DROP TABLE heap_child;
SELECT relname, sift._test_tbl_bulk_refusal(relname::regclass) AS refusal FROM (VALUES ('trig_row'), ('fk_parent')) t(relname);

DROP TABLE plain, indexed, with_bm25, pk, uniq, uniq_partial, excl, trig_row, trig_stmt, fk_child, fk_parent, fk_self, heap_parent;
DROP FUNCTION nothing();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
