-- a page of a text key under a locale collation ordered with the id as the tie-break, from a bound, is served by the whole order of the key (`order_array_columns = 't'`: the default build of a narrow locale index leaves the column out and says so in a NOTICE)
-- label: `WHERE t >= 't00100000' ORDER BY t, id LIMIT 20` over 200,000 rows of a `text COLLATE "und-x-icu"` key unrelated to the heap order, `order_array_columns = 't'`
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_tl (id int PRIMARY KEY, g int NOT NULL, t text COLLATE "und-x-icu" NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_tl SELECT i, i % 1000, 't' || lpad(((i::bigint * 104729) % 200000)::text, 8, '0') FROM generate_series(1, 200000) i;
CREATE INDEX tp_tl_col ON tp_tl USING sift_col (id, g, t) WITH (order_array_columns = 't');
VACUUM (FREEZE) tp_tl;
VACUUM (FREEZE, ANALYZE) tp_tl;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_tl' AND attname = 't') > 0.1 THEN RAISE EXCEPTION 't follows the heap order'; END IF; END $$
--@ pg
SELECT id, t FROM tp_tl WHERE t >= 't00100000' ORDER BY t, id LIMIT 20
