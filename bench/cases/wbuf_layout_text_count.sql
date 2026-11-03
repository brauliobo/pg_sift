-- ORDER BY a text key of an index with 5,000 rows in its write buffer (written after the build, not promoted): the buffer is read in ranges of the key with the segment
-- label: `count(*) WHERE txt >= 'a' AND txt < 'b'` of a text key with 5,000 rows in the write buffer
-- ordered: yes
-- vacuum: no
--@ setup
DROP TABLE IF EXISTS olw;
CREATE TABLE olw AS SELECT g::int AS id, md5(g::text) AS txt FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
CREATE INDEX olw_col ON olw USING sift_col (id, txt);
CREATE SCHEMA c_wbuf_layout_text_count__btree;
CREATE TABLE c_wbuf_layout_text_count__btree.olw AS TABLE olw;
CREATE INDEX olw_bt ON c_wbuf_layout_text_count__btree.olw (txt);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) olw;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_wbuf_layout_text_count__btree.olw;
DO $$ DECLARE c float8 := (SELECT correlation FROM pg_stats WHERE schemaname = 'c_events' AND tablename = 'olw' AND attname = 'txt'); BEGIN IF c IS NULL OR abs(c) >= 0.05 THEN RAISE EXCEPTION 'the key follows the heap: correlation %', c; END IF; END $$;
INSERT INTO olw SELECT g::int, md5(g::text) FROM generate_series({rows} + 1, {rows} + 5000) g;
INSERT INTO c_wbuf_layout_text_count__btree.olw SELECT g::int, md5(g::text) FROM generate_series({rows} + 1, {rows} + 5000) g;
ANALYZE olw;
ANALYZE c_wbuf_layout_text_count__btree.olw;
--@ pg
SELECT count(*) FROM olw WHERE txt >= 'a' AND txt < 'b'
