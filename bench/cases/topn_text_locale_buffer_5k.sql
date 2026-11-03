-- a page of a text key under a locale collation (`order_array_columns = 't'`) keeps the whole order of the key with 5,000 rows (2.5% of the table) in the write buffer
-- label: `WHERE t >= 't00100000' ORDER BY t LIMIT 20` over 200,000 rows of a `text COLLATE "und-x-icu"` key unrelated to the heap order, then 5,000 rows inserted that stay in the write buffer, `order_array_columns = 't'`
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_bl (id int PRIMARY KEY, g int NOT NULL, t text COLLATE "und-x-icu" NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_bl SELECT i, i % 1000, 't' || lpad(((i::bigint * 104729) % 200000)::text, 8, '0') FROM generate_series(1, 200000) i;
CREATE INDEX tp_bl_col ON tp_bl USING sift_col (id, g, t) WITH (order_array_columns = 't');
VACUUM (FREEZE) tp_bl;
VACUUM (FREEZE, ANALYZE) tp_bl;
INSERT INTO tp_bl SELECT 200000 + i, i % 1000, 't' || lpad((200000 + i)::text, 8, '0') FROM generate_series(1, 5000) i;
DO $$ BEGIN
  IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_bl' AND attname = 't') > 0.1 THEN RAISE EXCEPTION 't follows the heap order'; END IF;
  IF (SELECT sum(rows) FROM sift.col_segments('tp_bl_col') WHERE kind = 'buffer') <> 5000 THEN RAISE EXCEPTION 'the write buffer does not hold 5,000 rows'; END IF;
END $$
--@ pg
SELECT id, t FROM tp_bl WHERE t >= 't00100000' ORDER BY t LIMIT 20
