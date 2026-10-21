-- The dictionary of a string leaf is one compressed blob that every scan reading the leaf inflates and indexes whole. A segment never
-- changes, so the backend keeps the parsed dictionary for the next query (col/dictcache.c): the second scan parses none, a rewritten
-- segment has its own, and the answers do not depend on it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE dc (id int, c0 text, c1 text, c2 text, c3 text, c4 text, c5 text);
INSERT INTO dc SELECT g, v, v, v, v, v, v FROM (SELECT g, 'label ' || (g % 500) AS v FROM generate_series(1, 20000) g) s;
CREATE INDEX dc_idx ON dc USING sift_col (c0, c1, c2, c3, c4, c5);
SELECT count(*) > 0 AS has_segment FROM sift.col_segments('dc_idx'::regclass) WHERE kind = 'immutable';

-- a cache that holds five of the six dictionaries (the backend starts with none): the least recently used goes, the answers do not change
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT bytes AS one FROM sift._test_col_dict_cache(true) \gset
SELECT set_config('sift.test_col_dict_cache', ((:one * 5.5 + 1023) / 1024)::int::text, false) IS NOT NULL AS sized;
SELECT sum(rows) AS rows FROM generate_series(1, 5) l, LATERAL sift._test_col_counters('dc_idx', l || ':eq:label 7');
SELECT loads = 5 AS parsed, bytes <= :one * 5.5 AS within_the_limit FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('dc_idx', '5:eq:label 7');
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT rows FROM sift._test_col_counters('dc_idx', '5:eq:label 7');
SELECT loads = 1 AS first_parsed_again, hits = 2 AS the_others_found FROM sift._test_col_dict_cache(true);
RESET sift.test_col_dict_cache;

-- the first scan of a segment parses a dictionary, the second finds it
REINDEX INDEX dc_idx;
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT loads > 0 AS parsed, hits = 0 AS none_found FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT loads = 0 AS none_parsed, hits > 0 AS found FROM sift._test_col_dict_cache(true);

-- another leaf has its own
SELECT rows FROM sift._test_col_counters('dc_idx', '1:eq:label 7');
SELECT loads > 0 AS parsed, hits = 0 AS none_found FROM sift._test_col_dict_cache(true);

-- a segment written again (REINDEX) is another segment: its dictionary is parsed, the answers are the same
REINDEX INDEX dc_idx;
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT loads > 0 AS parsed FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('dc_idx', '0:eq:label 7');
SELECT loads = 0 AS none_parsed, hits > 0 AS found FROM sift._test_col_dict_cache(true);

-- a cache too small to keep a dictionary parses it every time, with the same answers
SET sift.test_col_dict_cache = 1;
SELECT rows FROM sift._test_col_counters('dc_idx', '2:eq:label 7');
SELECT rows FROM sift._test_col_counters('dc_idx', '2:eq:label 7');
SELECT loads = 2 AS parsed_each_time, hits = 0 AS none_found FROM sift._test_col_dict_cache(true);
RESET sift.test_col_dict_cache;

DROP TABLE dc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
