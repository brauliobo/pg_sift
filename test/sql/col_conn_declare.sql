-- The reloption connect of a child sift_col index: 'fk -> [schema.]parent(key)'. CREATE INDEX, REINDEX and ALTER INDEX resolve it: the child index
-- depends on the sift_col index of the parent table that tracks the key, and that index keeps the key journal. The text is the record (pg_dump writes it).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10, 'l' || i, 'x' FROM generate_series(1, 500) i;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, value_num int NOT NULL, label text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 500, i, 'e' FROM generate_series(1, 2000) i;
CREATE FUNCTION edges(child regclass) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce(string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text), 'none') FROM pg_depend
   WHERE classid = 'pg_class'::regclass AND objid = child AND refclassid = 'pg_class'::regclass AND deptype = 'n' $$;
CREATE FUNCTION journal_of(idx regclass) RETURNS boolean LANGUAGE sql AS $$ SELECT lineage <> 0 FROM sift._test_conn_gen(idx) $$;

-- what is wrong with the text, and with the names
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id dim_a(id)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id, x)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id) category_id');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id) carries category_id');
-- the parent table has no sift_col index yet
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id)');
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label);
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'value_num -> dim_a(id)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'label -> dim_a(id)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> nowhere(id)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(nokey)');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> events(id)');
SELECT edges('dim_a_col') AS parent_edges, journal_of('dim_a_col') AS journal;

-- a parent in another schema is named with its schema; an unqualified name is in the schema of the child table
CREATE SCHEMA other;
CREATE TABLE other.dim_o (id int PRIMARY KEY, category_id int NOT NULL);
CREATE INDEX dim_o_col ON other.dim_o USING sift_col (id, category_id);
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_o(id)');
CREATE INDEX events_o ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> other.dim_o(id)');
SELECT edges('events_o') AS other_edges;
DROP INDEX events_o;
DROP SCHEMA other CASCADE;

-- the connection: a dependency, the journal, the text in the options
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, value_num) WITH (connect = 'dim_a_id -> public.dim_a(id)');
SELECT edges('events_col') AS child_edges, journal_of('dim_a_col') AS journal;
SELECT o AS option FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'events_col' AND o LIKE 'connect=%';
SELECT regexp_replace(pg_get_indexdef('events_col'::regclass), '^.*WITH', 'WITH') AS definition;
SELECT child, parent, declaration, lineage IS NOT NULL AS journal, records, stamp FROM sift.col_connections;
SELECT lineage IS NULL AS no_journal, stamp FROM sift.col_connection_state('events_col');

-- REINDEX, and ALTER INDEX of another option, keep one dependency
REINDEX INDEX events_col;
ALTER INDEX events_col SET (superunit = 8);
SELECT count(*) AS edges FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass AND refobjid = 'dim_a_col'::regclass;

-- off removes the connection, the text brings it back, RESET removes it again
ALTER INDEX events_col SET (connect = 'off');
SELECT edges('events_col') AS after_off;
ALTER INDEX events_col SET (connect = 'dim_a_id -> dim_a(id)');
SELECT edges('events_col') AS after_set;
ALTER INDEX events_col SET (connect = 'dim_a_id -> dim_a(nokey)');
ALTER INDEX events_col RESET (connect);
SELECT edges('events_col') AS after_reset;
ALTER INDEX events_col SET (connect = 'dim_a_id -> dim_a(id)');

-- the parent index and table cannot go under the child index; CASCADE takes the child index with them
DROP INDEX dim_a_col;
DROP TABLE dim_a;
DROP TABLE dim_a CASCADE;
SELECT count(*) AS child_indexes FROM pg_class WHERE relname = 'events_col';
DROP TABLE events;
DROP FUNCTION edges(regclass);
DROP FUNCTION journal_of(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
