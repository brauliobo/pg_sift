-- Tables and indexes of scripts/coverage.sh on top of test/col/fixture.sql: the conventional indexes and the column indexes of the fixture
-- tables (as scripts/coldiff.sh, plus customer_id), one column of every type PostgreSQL has a common use for, a sift_table with them all,
-- a table in every heap state, and the search objects (keyword index, partitions, autocomplete) of the BM25 rows.
--   psql -f coverage_data.sql           (after fixture.sql)
\set ON_ERROR_STOP on

ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_store_id_placed_at_index ON orders USING btree (store_id, placed_at, deleted_at);
CREATE INDEX orders_items_gin_index ON orders USING gin (items jsonb_path_ops);
CREATE INDEX orders_fts ON orders USING sift (memo);
CREATE INDEX orders_col ON orders USING sift_col (id, store_id, customer_id, source, placed_at, country_code, lang, status, total, discount, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes->channel text, attributes->normalized bool, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_role text}');
CREATE INDEX orders_odd_col ON orders_odd USING sift_col (id, store_id, customer_id, source, placed_at, country_code, lang, status, total, discount, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes->channel text, attributes->normalized bool, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_role text}');
CREATE INDEX orders_part_col ON orders_part USING sift_col (id, store_id, customer_id, source, placed_at, country_code, lang, status, total, discount, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes->channel text, attributes->normalized bool, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_role text}');
CREATE INDEX t_int_col ON t_int USING sift_col (id, grp, i4, i8);
CREATE INDEX t_num_col ON t_num USING sift_col (id, grp, n2, n0, n6);
CREATE INDEX t_float_col ON t_float USING sift_col (id, grp, f4, f8);
CREATE INDEX t_text_col ON t_text USING sift_col (id, grp, t, vc);
CREATE INDEX t_jp_col ON t_jp USING sift_col (id, grp, doc, mdoc) WITH (paths = 'doc[]{s text, n int4, b bool}, mdoc[]{s text, n int4, b bool}');

CREATE TYPE cv_color AS ENUM ('red', 'green', 'blue');
CREATE TABLE cv_types (
	id int PRIMARY KEY, grp int NOT NULL, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8,
	n2 numeric(10,2), n15 numeric(15,3), n18 numeric(18,4), n38 numeric(38,6), nfree numeric,
	tx text, vc varchar(20), ch char(5), ba bytea, b boolean, d date, ts timestamp, tstz timestamptz, tm time, iv interval,
	u uuid, m money, e cv_color, ia int[], j jsonb);
INSERT INTO cv_types
SELECT i, i % 7, (i % 100)::int2, i, i * 1000000000::bigint, ((i % 50) / 4.0)::float4, (i % 50) / 4.0,
	((i % 997) / 7.0)::numeric(10,2), ((i % 991) / 3.0)::numeric(15,3), ((i % 983) / 11.0)::numeric(18,4), (i / 13.0)::numeric(38,6), (i / 17.0)::numeric,
	'tx' || (i % 40), 'vc' || (i % 12), 'c' || (i % 9), decode(md5(i::text), 'hex'), i % 3 = 0,
	date '2024-01-01' + i % 90, timestamp '2024-01-01 00:00' + (i % 500) * interval '1 hour', timestamptz '2024-01-01 00:00+00' + (i % 500) * interval '1 hour',
	time '00:00' + (i % 24) * interval '1 hour', (i % 30) * interval '1 day',
	md5((i % 25)::text)::uuid, ((i % 100) / 4.0)::numeric::money, (ARRAY['red', 'green', 'blue'])[1 + i % 3]::cv_color,
	ARRAY[i % 5, i % 3], jsonb_build_object('k', i % 4, 'tags', jsonb_build_array(i % 3, i % 5))
FROM generate_series(1, 3000) i;
CREATE INDEX cv_types_col ON cv_types USING sift_col (id, grp, i2, i4, i8, f4, f8, n2, n15, n18, n38, nfree, tx, vc, b, d, ts, tstz, ch, m, iv);

-- the same rows in a sift_table (every type is kept exactly), sealed into column groups
CREATE TABLE cv_tt USING sift_table AS SELECT * FROM cv_types;
VACUUM cv_tt;
-- rows in the write buffer and tombstones next to the sealed groups, and a BM25 index on a sift_table
INSERT INTO cv_tt SELECT 3000 + i, i % 7, (i % 100)::int2, i, i, i / 4.0, i / 4.0, i / 7.0, i / 3.0, i / 11.0, i, i, 'tx' || (i % 40), 'vc' || (i % 12), 'c' || (i % 9),
	decode(md5(i::text), 'hex'), true, date '2024-06-01', timestamp '2024-06-01', timestamptz '2024-06-01+00', time '12:00', interval '1 day',
	md5(i::text)::uuid, 1::money, 'red', ARRAY[i], '{}' FROM generate_series(1, 200) i;
DELETE FROM cv_tt WHERE id % 50 = 0;
CREATE INDEX cv_tt_tx_fts ON cv_tt USING sift (tx);

-- a numeric NaN, which the column index stores as NULL with an overflow flag
CREATE TABLE cv_nan (g int, n numeric(10,2));
INSERT INTO cv_nan VALUES (1, 1.5), (1, 'NaN'), (2, 2.5);
CREATE INDEX cv_nan_col ON cv_nan USING sift_col (g, n);

-- a variadic user aggregate
CREATE FUNCTION cv_add(acc bigint, VARIADIC xs int[]) RETURNS bigint LANGUAGE sql IMMUTABLE PARALLEL SAFE
	AS $$ SELECT acc + (SELECT coalesce(sum(x), 0) FROM unnest(xs) x) $$;
CREATE AGGREGATE cv_sum_all(VARIADIC int[]) (sfunc = cv_add, stype = bigint, initcond = '0');

-- a table in every heap state: indexed and vacuumed, then rows the visibility map does not cover (inserted, updated, deleted, rolled back)
CREATE TABLE cv_dml (id int PRIMARY KEY, grp int, v int, f float8, tx text);
INSERT INTO cv_dml SELECT i, i % 5, i, i / 3.0, 'x' || (i % 8) FROM generate_series(1, 2000) i;
CREATE INDEX cv_dml_col ON cv_dml USING sift_col (id, grp, v, f, tx);
VACUUM (FREEZE, ANALYZE) cv_dml;
INSERT INTO cv_dml SELECT 2000 + i, i % 5, i, i / 3.0, 'y' || (i % 8) FROM generate_series(1, 300) i;
UPDATE cv_dml SET v = v + 1000, tx = 'u' WHERE id % 20 = 0 AND id <= 2000;
DELETE FROM cv_dml WHERE id % 25 = 1 AND id <= 2000;
BEGIN; INSERT INTO cv_dml SELECT 5000 + i, 9, i, i, 'gone' FROM generate_series(1, 100) i; ROLLBACK;

-- a table with row-level security
CREATE ROLE sift_cov_user NOLOGIN;
CREATE TABLE cv_rls (id int, grp int, v int);
INSERT INTO cv_rls SELECT i, i % 4, i FROM generate_series(1, 1000) i;
CREATE INDEX cv_rls_col ON cv_rls USING sift_col (id, grp, v);
ALTER TABLE cv_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY cv_rls_all ON cv_rls USING (true);
GRANT SELECT ON cv_rls TO sift_cov_user;
GRANT USAGE ON SCHEMA sift, coldiff TO sift_cov_user;

-- search: a keyword index for facets, a btree for filter pushdown, a partitioned table, a multi-field autocomplete table
CREATE INDEX orders_src_kw ON orders USING sift (source) WITH (analyzer = keyword);
CREATE INDEX orders_placed_at ON orders (placed_at);
CREATE TABLE cv_logs (id int, region text, msg text) PARTITION BY LIST (region);
CREATE TABLE cv_logs_eu PARTITION OF cv_logs FOR VALUES IN ('eu');
CREATE TABLE cv_logs_us PARTITION OF cv_logs FOR VALUES IN ('us');
INSERT INTO cv_logs SELECT i, CASE WHEN i % 2 = 0 THEN 'eu' ELSE 'us' END, 'disk full on node ' || i % 10 FROM generate_series(1, 1000) i;
CREATE INDEX cv_logs_msg ON cv_logs USING sift (msg);
CREATE TABLE cv_places (id int PRIMARY KEY, name text, admin1 text, country text);
INSERT INTO cv_places SELECT i, (ARRAY['Greenville', 'Greenwich', 'Hollis', 'Holland', 'Grover', 'Hope'])[1 + i % 6] || ' ' || i,
	(ARRAY['North', 'South'])[1 + i % 2], (ARRAY['US', 'GB', 'NL'])[1 + i % 3] FROM generate_series(1, 600) i;
CREATE INDEX cv_places_sift ON cv_places USING sift ((name || chr(29) || admin1 || ' ' || country)) WITH (fields = 'name', prefix_index = 3);
CREATE TABLE cv_stem (id int PRIMARY KEY, body text);
INSERT INTO cv_stem SELECT i, (ARRAY['runners were running', 'she runs every day', 'a quiet pool', 'cafes near the office'])[1 + i % 4] FROM generate_series(1, 400) i;
CREATE INDEX cv_stem_body ON cv_stem USING sift (body) WITH (stemmer = 'en');
-- tables of the doc-claim rows (D, L, S rows): the probes of test/col/coverage.sql and the `pre` lines of the table run on them
CREATE EXTENSION dblink;
CREATE EXTENSION pageinspect;
CREATE EXTENSION pg_walinspect;
CREATE TABLE cv_snap (id int, grp int, v int);
INSERT INTO cv_snap SELECT i, i % 5, i FROM generate_series(1, 2000) i;
CREATE INDEX cv_snap_col ON cv_snap USING sift_col (id, grp, v);
CREATE TABLE cv_hot (id int PRIMARY KEY, grp int, v int, pad text) WITH (fillfactor = 50);
INSERT INTO cv_hot SELECT i, i % 5, i, 'p' FROM generate_series(1, 2000) i;
CREATE INDEX cv_hot_col ON cv_hot USING sift_col (id, grp, v);
CREATE TABLE cv_maint (id int PRIMARY KEY, grp int, v int);
INSERT INTO cv_maint SELECT i, i % 5, i FROM generate_series(1, 3000) i;
CREATE INDEX cv_maint_col ON cv_maint USING sift_col (id, grp, v);
-- a jsonb column whose paths hold every JSON type, a JSON null and a missing key (the jsonb_typeof rows of coverage.tsv)
CREATE TABLE cv_typeof (id int PRIMARY KEY, grp int NOT NULL, doc jsonb);
INSERT INTO cv_typeof
SELECT i, i % 7, CASE i % 8 WHEN 0 THEN '{"m": "s", "a": "x"}' WHEN 1 THEN '{"m": true, "a": 12}' WHEN 2 THEN '{"m": {"x": 1}, "a": true}'
	WHEN 3 THEN '{"m": [1], "a": 1.50}' WHEN 4 THEN '{"m": null, "a": null}' WHEN 5 THEN '{}'
	ELSE jsonb_build_object('m', i / 4.0, 'a', 'v' || i % 5) END
FROM generate_series(1, 3000) i;
CREATE INDEX cv_typeof_col ON cv_typeof USING sift_col (id, grp, doc) WITH (paths = 'doc->m float8, doc->a astext');
ANALYZE;
