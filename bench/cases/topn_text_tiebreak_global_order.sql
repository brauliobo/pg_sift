-- a page of a text key ordered with the id as the tie-break, from a bound, is served by the whole order of the key (C collation, no write buffer, no churn)
-- label: `WHERE t >= 't00100000' ORDER BY t, id LIMIT 20` over 200,000 rows of a `text COLLATE "C"` key unrelated to the heap order
-- ordered: yes
-- vacuum: no
-- gap: sift lab 0012 (text tie-break keys)
--@ setup
CREATE TABLE tp_tc (id int PRIMARY KEY, g int NOT NULL, t text COLLATE "C" NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_tc SELECT i, i % 1000, 't' || lpad(((i::bigint * 104729) % 200000)::text, 8, '0') FROM generate_series(1, 200000) i;
CREATE INDEX tp_tc_col ON tp_tc USING sift_col (id, g, t);
VACUUM (FREEZE) tp_tc;
VACUUM (FREEZE, ANALYZE) tp_tc;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_tc' AND attname = 't') > 0.1 THEN RAISE EXCEPTION 't follows the heap order'; END IF; END $$
--@ pg
SELECT id, t FROM tp_tc WHERE t >= 't00100000' ORDER BY t, id LIMIT 20
