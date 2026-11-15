-- a list of 200 rows of one entity (ORDER BY d DESC, id LIMIT 200 over WHERE entity = k, 700 rows of {rows}) of a table with a b-tree on (entity, d) and a sift_col index with sorted runs: the plan is the b-tree's; the top-N scan priced the heap pages of its rows at a fifth of what the b-tree plan pays and took 1.5 to 2 times its cycles
-- label: `ORDER BY d DESC, id LIMIT 200` over one entity of 500 (zipf-like sizes, the entity of 0.4% of the rows) of {rows} rows with a b-tree on (entity, d); the variant `warm_forced` is the top-N scan of the same statement
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS el;
CREATE TABLE el AS SELECT g AS id, 1 + (500 * power((hashint4(g) & 2147483647) / 2147483648.0, 3))::int AS entity, date '2020-01-01' + (hashint4(g + 1) & 2147483647) % 2000 AS d, (hashint4(g + 2) & 2147483647) % 20 AS kind, g % 1000 AS amount FROM generate_series(1, {rows}) g;
ALTER TABLE el ADD PRIMARY KEY (id);
CREATE INDEX el_bt ON el (entity, d);
CREATE INDEX el_col ON el USING sift_col (id, entity, d, kind, amount) WITH (key_columns = 'entity', key_order = 'entity, d');
VACUUM (FREEZE, ANALYZE) el
--@ pg
SELECT id, d, amount FROM el WHERE entity = 40 ORDER BY d DESC, id LIMIT 200
