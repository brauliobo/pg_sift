-- a lookup of 10 keys of a column the heap holds in no order (the correlation of id is under 0.05): the scattered heap pages the lookup reads are the cost
-- label: `count(*), sum(v) WHERE id = ANY of 10 ids` over a heap in the order of a hash of id, `point_tid_columns = 'id'`; the variant `btree` is a b-tree on id
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ps;
CREATE TABLE ps AS SELECT g::int AS id, (g % 1000)::int AS v, (g % 97)::int AS w FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
CREATE INDEX ps_col ON ps USING sift_col (id, v, w) WITH (point_tid_columns = 'id');
CREATE SCHEMA c_point_scattered_10__btree;
CREATE TABLE c_point_scattered_10__btree.ps AS TABLE ps;
CREATE INDEX ps_bt ON c_point_scattered_10__btree.ps (id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ps;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_point_scattered_10__btree.ps;
DO $$ DECLARE c float8 := (SELECT correlation FROM pg_stats WHERE schemaname = 'c_events' AND tablename = 'ps' AND attname = 'id'); BEGIN IF c IS NULL OR abs(c) >= 0.05 THEN RAISE EXCEPTION 'the key follows the heap: correlation %', c; END IF; END $$;
--@ pg
SELECT count(*), sum(v) FROM ps WHERE id = ANY (ARRAY[(7919 % {rows} + 1), (15838 % {rows} + 1), (23757 % {rows} + 1), (31676 % {rows} + 1), (39595 % {rows} + 1), (47514 % {rows} + 1), (55433 % {rows} + 1), (63352 % {rows} + 1), (71271 % {rows} + 1), (79190 % {rows} + 1)])
