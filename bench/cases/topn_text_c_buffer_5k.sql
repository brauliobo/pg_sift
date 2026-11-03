-- a page of a C-collation text key keeps the whole order of the key with 5,000 rows (2.5% of the table) in the write buffer
-- label: `WHERE t >= 't00100000' ORDER BY t LIMIT 20` over 200,000 rows of a `text COLLATE "C"` key unrelated to the heap order, then 5,000 rows inserted that stay in the write buffer
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_bc (id int PRIMARY KEY, g int NOT NULL, t text COLLATE "C" NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_bc SELECT i, i % 1000, 't' || lpad(((i::bigint * 104729) % 200000)::text, 8, '0') FROM generate_series(1, 200000) i;
CREATE INDEX tp_bc_col ON tp_bc USING sift_col (id, g, t);
VACUUM (FREEZE) tp_bc;
VACUUM (FREEZE, ANALYZE) tp_bc;
INSERT INTO tp_bc SELECT 200000 + i, i % 1000, 't' || lpad((200000 + i)::text, 8, '0') FROM generate_series(1, 5000) i;
DO $$ BEGIN
  IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_bc' AND attname = 't') > 0.1 THEN RAISE EXCEPTION 't follows the heap order'; END IF;
  IF (SELECT sum(rows) FROM sift.col_segments('tp_bc_col') WHERE kind = 'buffer') <> 5000 THEN RAISE EXCEPTION 'the write buffer does not hold 5,000 rows'; END IF;
END $$
--@ pg
SELECT id, t FROM tp_bc WHERE t >= 't00100000' ORDER BY t LIMIT 20
