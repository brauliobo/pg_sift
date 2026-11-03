-- the write path of an ingest job: single-row INSERT .. ON CONFLICT (unique expression index) DO UPDATE .. RETURNING, half of the rows new and half already there
-- label: 4,000 single-row `INSERT .. ON CONFLICT (parent_id, COALESCE(kind_id, 0), ref) DO UPDATE SET .. RETURNING id` into a heap table of 100,000 rows with its primary key, a unique expression index and a `sift_col` index (pg_sift lane only)
--@ prepare
CREATE TABLE w (id int PRIMARY KEY, parent_id int NOT NULL, kind_id int, ref text NOT NULL, ts date NOT NULL, amount float8 NOT NULL, doc jsonb NOT NULL, updated_at timestamptz NOT NULL DEFAULT now(), deleted_at timestamptz);
INSERT INTO w SELECT i, 1 + i % 400, CASE WHEN i % 5 = 0 THEN i % 3 END, 'r' || i, date '2025-01-01' + i % 300, i % 17, '{}', now(), NULL FROM generate_series(1, 100000) i;
CREATE UNIQUE INDEX w_uq ON w (parent_id, COALESCE(kind_id, 0), ref);
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_col ON w USING sift_col (parent_id, kind_id, ts, amount, deleted_at, updated_at); END IF; END $$;
VACUUM ANALYZE w
--@ load
DO $$ DECLARE r int; BEGIN FOR i IN 1..4000 LOOP INSERT INTO w (id, parent_id, kind_id, ref, ts, amount, doc) VALUES (100000 + i, 1 + i % 400, NULL, 'r' || CASE WHEN i % 2 = 0 THEN (i % 100000) + 1 ELSE 200000 + i END, date '2026-01-01', i % 9, '{"lang": "en"}') ON CONFLICT (parent_id, COALESCE(kind_id, 0), ref) DO UPDATE SET ts = EXCLUDED.ts, amount = EXCLUDED.amount, doc = EXCLUDED.doc, updated_at = now(), deleted_at = NULL RETURNING id INTO r; END LOOP; END $$
--@ check
SELECT count(*) FROM w
