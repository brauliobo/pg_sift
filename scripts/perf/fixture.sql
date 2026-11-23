-- The template database of the black-box PR gate (scripts/perf/gate.py, scripts/perf/canaries.py). Every draw is a hash of (row number, seed), so the same
-- arguments give the same rows, and ANALYZE reads every row, so the planner sees the same statistics on every run. The gate builds this file once per
-- (library, SQL scripts, this file, rows, seed) and clones the data directory on every later run.
--
--   psql -v rows=64000 -v seed=1 -f fixture.sql
--
-- sf_events carries the 7 subsystems of the gate: a star around 3 dimensions, ClickBench-style text columns, B-tree indexes beside the column index, a shredded
-- jsonb document, a BM25 body. Its rows are padded to cross the stock min_parallel_table_scan_size (8MB), so the stock planner launches workers with no cost override.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 64000
\endif
\if :{?seed}
\else
\set seed 1
\endif
SET default_statistics_target = 1000;
SET synchronous_commit = on;
SET timezone = 'UTC';
CREATE EXTENSION pg_sift;

CREATE TABLE sf_dim_category (cat_id int PRIMARY KEY, name text NOT NULL);
INSERT INTO sf_dim_category SELECT c, 'cat_' || c FROM generate_series(1, 32) c;

CREATE TABLE sf_dim_product (prod_id int PRIMARY KEY, cat_id int NOT NULL REFERENCES sf_dim_category, brand text NOT NULL, price numeric(10,2) NOT NULL);
INSERT INTO sf_dim_product SELECT p, 1 + p % 32, 'brand_' || p % 24, 1 + ((hashint4(p + :seed) & 32767) % 10000) / 100.0 FROM generate_series(1, 512) p;

CREATE TABLE sf_dim_region (reg_id int PRIMARY KEY, tier text NOT NULL, active boolean NOT NULL);
INSERT INTO sf_dim_region SELECT r, 'tier_' || chr(97 + r % 4), r % 5 <> 0 FROM generate_series(1, 64) r;

-- amount is a multiple of 0.25 and score an integer: float sums are exact in any order, so a parallel and a serial plan return equal rows
CREATE TABLE sf_events (id int PRIMARY KEY, prod_id int NOT NULL, reg_id int NOT NULL, event_ts timestamptz NOT NULL, day date NOT NULL, status text NOT NULL,
	url text NOT NULL, user_agent text NOT NULL, amount float8 NOT NULL, qty int NOT NULL, metric_num numeric(12,2) NOT NULL, doc jsonb, body text NOT NULL,
	pad text STORAGE PLAIN NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO sf_events
SELECT i, 1 + (hashint4(i * 3 + :seed) & 2147483647) % 512, 1 + (hashint4(i * 5 + :seed) & 2147483647) % 64, ts, ts::date, 'st_' || (hashint4(i * 7 + :seed) & 2147483647) % 8,
	CASE WHEN u < 0.08 THEN 'https://www.google.com/search?q=' || n ELSE 'https://site' || n % 50 || '.example.com/p/' || n END, 'ua_' || (hashint4(i * 11 + :seed) & 2147483647) % 500,
	1 + 0.25 * ((hashint4(i * 13 + :seed) & 2147483647) % 4000), 1 + (hashint4(i * 17 + :seed) & 2147483647) % 9, ((hashint4(i * 19 + :seed) & 2147483647) % 1000000) / 100.0,
	CASE WHEN i % 50 = 0 THEN NULL ELSE jsonb_build_object('channel', 'ch_' || chr(97 + i % 6), 'score', 1 + i % 10, 'flag', CASE WHEN i % 10 < 7 THEN 'ok' WHEN i % 10 < 9 THEN 'warn' ELSE 'bad' END,
		'items', jsonb_build_array(jsonb_build_object('kind', 'k_' || chr(97 + i % 4), 'score', 1 + (i * 3) % 10), jsonb_build_object('kind', 'k_' || chr(97 + (i + 1) % 4), 'score', 1 + (i * 7) % 10))) END,
	CASE WHEN i % 5 < 4 THEN 'wire ' ELSE '' END || CASE WHEN i % 8 = 0 THEN 'switch ' ELSE '' END || CASE WHEN i % 97 = 0 THEN 'quantum ' ELSE '' END || CASE WHEN i % 31 = 0 THEN 'clean cable ' ELSE '' END
		|| 'tail' || (hashint4(i * 23 + :seed) & 2147483647) % 2000 || ' tail' || (hashint4(i * 29 + :seed) & 2147483647) % 2000,
	repeat(md5(i::text), 3)
FROM generate_series(1, :rows) i,
	LATERAL (SELECT timestamptz '2024-01-01' + ((hashint4(i + :seed) & 2147483647) % 63072000) * interval '1 second' AS ts,
		(hashint4(i * 31 + :seed) & 65535) / 65536.0 AS u,
		floor(power(4000.0, ((i * 0.6180339887498949) % 1.0)))::int AS n) g;
CREATE INDEX sf_events_btree_day_reg ON sf_events (reg_id, day);
CREATE INDEX sf_events_col ON sf_events USING sift_col (id, prod_id, reg_id, event_ts, day, status, url, user_agent, amount, qty, metric_num, doc)
	WITH (key_order = 'reg_id, event_ts', paths = 'doc.channel text, doc.score float8, doc.flag text, doc.items[]{kind astext, score float8}');
CREATE INDEX sf_events_bm25 ON sf_events USING sift (body);

CREATE TABLE tbl_store (id int PRIMARY KEY, prod_id int NOT NULL, reg_id int NOT NULL, event_ts timestamptz NOT NULL, day date NOT NULL, status text NOT NULL,
	url text NOT NULL, user_agent text NOT NULL, amount float8 NOT NULL, qty int NOT NULL, metric_num numeric(12,2) NOT NULL, doc jsonb) USING sift_table;
SELECT sift.table_set_option('tbl_store', 'key_order', 'reg_id, id');
INSERT INTO tbl_store SELECT id, prod_id, reg_id, event_ts, day, status, url, user_agent, amount, qty, metric_num, doc FROM sf_events;
CREATE TABLE tbl_copy_stage (LIKE tbl_store) USING sift_table;
CREATE TABLE tbl_copy_heap (LIKE tbl_store);
CREATE TABLE tbl_mut (LIKE tbl_store INCLUDING INDEXES) USING sift_table;
INSERT INTO tbl_mut SELECT * FROM tbl_store WHERE id <= :rows / 4;

-- the pair of the change-tracking canary: the same rows and the same column index, with and without the row trigger that tracks the changes of the heap
CREATE TABLE tbl_col_track (id int PRIMARY KEY, reg_id int NOT NULL, amount float8 NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tbl_col_track SELECT id, reg_id, amount, status FROM sf_events WHERE id <= :rows / 4;
CREATE INDEX tbl_col_track_col ON tbl_col_track USING sift_col (id, reg_id, amount, status) WITH (track_changes = 'ctid');
CREATE TABLE tbl_untracked (LIKE tbl_col_track INCLUDING INDEXES) WITH (autovacuum_enabled = false);
INSERT INTO tbl_untracked SELECT * FROM tbl_col_track;
CREATE INDEX tbl_untracked_col ON tbl_untracked USING sift_col (id, reg_id, amount, status) WITH (track_changes = 'off');

-- the visible fraction of a table prices the index-only scans of the native plan: a first pass has been seen to set the all-visible bit of part of the pages only
VACUUM (FREEZE, ANALYZE) sf_dim_category, sf_dim_product, sf_dim_region, sf_events, tbl_store, tbl_copy_stage, tbl_copy_heap, tbl_mut, tbl_col_track, tbl_untracked;
VACUUM (FREEZE, ANALYZE) sf_events, tbl_col_track, tbl_untracked;
DO $$
BEGIN
	IF pg_relation_size('sf_events') < 8 * 1024 * 1024 THEN
		RAISE EXCEPTION 'fixture.sql: sf_events heap is % bytes, below the stock min_parallel_table_scan_size of 8MB: pad the rows', pg_relation_size('sf_events');
	END IF;
	IF EXISTS (SELECT 1 FROM pg_class WHERE oid IN ('sf_events'::regclass, 'tbl_col_track'::regclass, 'tbl_untracked'::regclass) AND relallvisible < relpages) THEN
		RAISE EXCEPTION 'fixture.sql: a table has pages that are not all visible after two VACUUMs (a transaction holding the horizon?)';
	END IF;
END $$;
CHECKPOINT;
