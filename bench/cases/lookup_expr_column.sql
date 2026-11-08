-- a count grouped by an expression of the columns of a lookup table (a presence mask) that an index of the table holds as a column: the mask is read from its leaf, where seven text columns were read and the mask evaluated for each distinct tuple of them
-- label: `count(*)` of {rows} rows by their lookup and the presence mask of two text columns of the parent (a table of {rows} / 3 keys, a sift_col index of its key and the mask), the lookup table loaded by the query (`sift.agg_lookup_cache_kb = 0`, `sift.agg_lookup_shared_kb = 0`)
-- ordered: yes
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
DROP TABLE IF EXISTS le_parent;
DROP TABLE IF EXISTS le_child;
DROP FUNCTION IF EXISTS le_presence(text, text);
CREATE FUNCTION le_presence(a text, b text) RETURNS smallint LANGUAGE sql IMMUTABLE PARALLEL SAFE AS
$$ SELECT ((nullif(btrim(a), '') IS NOT NULL)::int + ((nullif(btrim(b), '') IS NOT NULL)::int << 1))::smallint $$;
CREATE TABLE le_parent (id int PRIMARY KEY, a text, b text) WITH (autovacuum_enabled = false);
INSERT INTO le_parent SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE 'name ' || g END, CASE WHEN g % 5 = 0 THEN '' ELSE 'city ' || g % 100 END FROM generate_series(1, {rows} / 3) g;
CREATE TABLE le_child (id int, lookup_id int, parent_id int) WITH (autovacuum_enabled = false);
INSERT INTO le_child SELECT g, g % 200, 1 + (g * 31) % ({rows} / 3) FROM generate_series(1, {rows}) g;
CREATE INDEX le_parent_col ON le_parent USING sift_col (id, le_presence(a, b));
CREATE INDEX le_child_col ON le_child USING sift_col (id, lookup_id, parent_id);
VACUUM (FREEZE, ANALYZE) le_parent, le_child
--@ pg
SELECT c.lookup_id, le_presence(p.a, p.b) AS m, count(*) FROM le_child c JOIN le_parent p ON p.id = c.parent_id GROUP BY 1, 2 ORDER BY 1, 2
