-- a page of a list ordered by a text key with the id as the tie-break, from a bound: the order array of the text column must serve the page
-- label: `WHERE t >= 't00100000' ORDER BY t, id LIMIT 20` over 200,000 rows whose text key is a permutation unrelated to the heap order
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_text (id int PRIMARY KEY, t text NOT NULL, deleted_at timestamptz);
INSERT INTO tp_text SELECT i, 't' || lpad(((i::bigint * 104729) % 200000)::text, 8, '0'), NULL FROM generate_series(1, 200000) i;
CREATE INDEX tp_text_col ON tp_text USING sift_col (id, t, deleted_at);
VACUUM (FREEZE) tp_text;
VACUUM (FREEZE, ANALYZE) tp_text;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_text' AND attname = 't') > 0.1 THEN RAISE EXCEPTION 't follows the heap order'; END IF; END $$
--@ pg
SELECT id, t FROM tp_text WHERE deleted_at IS NULL AND t >= 't00100000' ORDER BY t, id LIMIT 20
