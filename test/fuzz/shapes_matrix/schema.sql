-- The small schema of the planner shape matrix (test/fuzz/shapes_matrix.py): every fact table holds the same 300 rows and differs by the indexes on the join column.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;

CREATE TABLE ps_tenants (id int PRIMARY KEY, name text);
INSERT INTO ps_tenants SELECT i, 'tenant ' || i FROM generate_series(1, 5) i;

CREATE TABLE ps_groups (grp int, prop int);
INSERT INTO ps_groups SELECT i % 12, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 7) % 40 END FROM generate_series(1, 30) i;

CREATE TABLE ps_entities (id int PRIMARY KEY, tenant_id int, grp int, prop int, name text);
INSERT INTO ps_entities SELECT i, 1 + i % 5, i % 12, CASE WHEN i % 3 = 0 THEN NULL ELSE i % 40 END, 'entity ' || i FROM generate_series(1, 60) i;
CREATE INDEX ps_entities_grp ON ps_entities (grp);
CREATE INDEX ps_entities_col ON ps_entities USING sift_col (id, tenant_id, grp, prop);

CREATE TABLE ps_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO ps_pairs SELECT DISTINCT i % 60 + 1, 1 + (i * 3) % 5 FROM generate_series(1, 150) i;
CREATE INDEX ps_pairs_col ON ps_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

CREATE VIEW ps_view AS SELECT e.id, e.tenant_id, e.grp, coalesce(e.prop, g.prop) AS prop FROM ps_entities e LEFT JOIN ps_groups g ON g.grp = e.grp;

CREATE FUNCTION ps_fill(t regclass) RETURNS void LANGUAGE plpgsql AS $$ BEGIN
	EXECUTE format($q$INSERT INTO %s SELECT i, CASE WHEN i %% 11 = 0 THEN NULL ELSE 1 + i %% 60 END, 1 + i %% 5, timestamptz '2026-01-01' + i * interval '7 hours',
		'k' || i %% 7, round((i %% 97)::numeric * 1.5, 2), i %% 13, ((i %% 29) / 4.0)::float8, ARRAY['t' || i %% 3, 't' || i %% 5], i %% 4 = 0,
		jsonb_build_object('a', i %% 9), 'word' || i %% 8 || ' common text'
		FROM generate_series(1, 300) i$q$, t);
END $$;

-- id, dim_a_id (nullable join key), tenant_id, created_at, kind, amount, qty, score, tags, flag, attrs, body
CREATE TABLE ps_events_none (id int, dim_a_id int, tenant_id int, created_at timestamptz, kind text, amount numeric(10,2), qty int, score float8, tags text[], flag bool, attrs jsonb, body text);
CREATE TABLE ps_events_btree (LIKE ps_events_none);
CREATE TABLE ps_events_partial (LIKE ps_events_none);
CREATE TABLE ps_events_expr (LIKE ps_events_none);
CREATE TABLE ps_events_cover (LIKE ps_events_none);
CREATE TABLE ps_events_scope (LIKE ps_events_none);
CREATE TABLE ps_events_connect (LIKE ps_events_none);
CREATE TABLE ps_events_proj (LIKE ps_events_none);
CREATE TABLE ps_events_bm25 (LIKE ps_events_none);
CREATE TABLE ps_events_tbl (LIKE ps_events_none) USING sift_table;
CREATE TABLE ps_events_tbl_bm25 (LIKE ps_events_none) USING sift_table;
CREATE TABLE ps_events_inh (LIKE ps_events_none);
CREATE TABLE ps_events_inh_child () INHERITS (ps_events_inh);
SELECT ps_fill(t) FROM unnest(ARRAY['ps_events_none', 'ps_events_btree', 'ps_events_partial', 'ps_events_expr', 'ps_events_cover', 'ps_events_scope',
	'ps_events_connect', 'ps_events_proj', 'ps_events_bm25', 'ps_events_tbl', 'ps_events_tbl_bm25', 'ps_events_inh']::regclass[]) t;
INSERT INTO ps_events_inh_child SELECT id + 1000, dim_a_id, tenant_id, created_at, kind, amount, qty, score, tags, flag, attrs, body FROM ps_events_none WHERE id <= 60;

CREATE TABLE ps_events_part (LIKE ps_events_none) PARTITION BY HASH (id);
CREATE TABLE ps_events_part_0 PARTITION OF ps_events_part FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE ps_events_part_1 PARTITION OF ps_events_part FOR VALUES WITH (MODULUS 2, REMAINDER 1);
SELECT ps_fill('ps_events_part');

CREATE INDEX ON ps_events_none USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_btree USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_btree (dim_a_id);
CREATE INDEX ON ps_events_partial USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_partial (dim_a_id) WHERE dim_a_id < 30;
CREATE INDEX ON ps_events_expr USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_expr ((dim_a_id + 0));
CREATE INDEX ON ps_events_expr ((coalesce(dim_a_id, 0)));
CREATE INDEX ON ps_events_cover USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_cover (dim_a_id) INCLUDE (amount, tenant_id);
CREATE INDEX ON ps_events_scope USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag)
	WITH (scope = 'pair_tenant: dim_a_id -> ps_pairs(dim_a_id, tenant_id)');
CREATE INDEX ON ps_events_scope (dim_a_id);
CREATE INDEX ON ps_events_connect (dim_a_id);
CREATE INDEX ON ps_events_proj USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag)
	WITH (key_cut = 'dim_a_id', rollup_columns = 'tenant_id, kind: amount', calendar_columns = 'created_at', track_changes = 'ctid');
CREATE INDEX ON ps_events_proj (dim_a_id);
CREATE INDEX ON ps_events_bm25 USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_bm25 USING sift (body);
CREATE INDEX ON ps_events_bm25 (dim_a_id);
CREATE INDEX ON ps_events_tbl (dim_a_id);
CREATE INDEX ON ps_events_tbl_bm25 USING sift (body);
CREATE INDEX ON ps_events_tbl_bm25 (dim_a_id);
CREATE INDEX ON ps_events_inh USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_inh (dim_a_id);
CREATE INDEX ON ps_events_inh_child USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_inh_child (dim_a_id);
CREATE INDEX ON ps_events_part USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag);
CREATE INDEX ON ps_events_part (dim_a_id);
VACUUM ANALYZE;
