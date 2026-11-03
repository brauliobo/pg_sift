-- the forced merge of the segments of a sift_col index cut by two keys: the rows sorted by heap address in a dense array, the range of an integer column from the zone map, and from 12 MB of segments a slice of the key range for each of two workers
-- label: `sift.col_merge(index, true)` of an index of 5 columns cut by two keys (`key_cut = 'cust, d'`) built from four batches of the rows of the run, with 0, 2 and 4 parallel maintenance workers as variants; the other lane rebuilds a b-tree over the same columns
--@ prepare
CREATE TABLE w (id int, cust int, d date, v bigint, s text) WITH (autovacuum_enabled = false);
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (id, cust, d, v, s) WITH (key_cut = 'cust, d'); ELSE CREATE INDEX w_ix ON w (cust, d, id, v); END IF; END $$;
SET sift.col_auto_merge = off;
INSERT INTO w SELECT g, (hashint8extended(g, 1) & 2147483647) % ({rows} / 50), date '2024-01-01' + (g % 365), g * 7, md5(g::text) FROM generate_series(0 * ({rows} / 4) + 1, 1 * ({rows} / 4)) g;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN PERFORM sift.col_promote('w_ix'); END IF; END $$;
INSERT INTO w SELECT g, (hashint8extended(g, 1) & 2147483647) % ({rows} / 50), date '2024-01-01' + (g % 365), g * 7, md5(g::text) FROM generate_series(1 * ({rows} / 4) + 1, 2 * ({rows} / 4)) g;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN PERFORM sift.col_promote('w_ix'); END IF; END $$;
INSERT INTO w SELECT g, (hashint8extended(g, 1) & 2147483647) % ({rows} / 50), date '2024-01-01' + (g % 365), g * 7, md5(g::text) FROM generate_series(2 * ({rows} / 4) + 1, 3 * ({rows} / 4)) g;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN PERFORM sift.col_promote('w_ix'); END IF; END $$;
INSERT INTO w SELECT g, (hashint8extended(g, 1) & 2147483647) % ({rows} / 50), date '2024-01-01' + (g % 365), g * 7, md5(g::text) FROM generate_series(3 * ({rows} / 4) + 1, 4 * ({rows} / 4)) g;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN PERFORM sift.col_promote('w_ix'); END IF; END $$;
ANALYZE w
--@ load
REINDEX INDEX w_ix
--@ load_sift
SELECT sift.col_merge('w_ix', true)
--@ check
SELECT count(*) FROM w
