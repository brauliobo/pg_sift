-- mutable_limit, the reloption of 1.x and 2.0, stays accepted as an alias of write_buffer_limit
CREATE EXTENSION pg_sift;
CREATE TABLE wa (id serial PRIMARY KEY, body text);
CREATE INDEX wa_old ON wa USING sift (body) WITH (mutable_limit = 64);
CREATE INDEX wa_new ON wa USING sift (body) WITH (write_buffer_limit = 64);
CREATE INDEX wa_def ON wa USING sift (body);
SELECT relname, reloptions FROM pg_class WHERE relname IN ('wa_old', 'wa_new', 'wa_def') ORDER BY relname;

-- both names check their range
CREATE INDEX wa_bad1 ON wa USING sift (body) WITH (mutable_limit = 0);
CREATE INDEX wa_bad2 ON wa USING sift (body) WITH (write_buffer_limit = 0);

-- ALTER INDEX .. SET / RESET take the old name too
ALTER INDEX wa_def SET (mutable_limit = 128);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';
ALTER INDEX wa_def RESET (mutable_limit);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';

-- the limit applies under either name: 3000 documents fill a 64 kB write buffer many times, never the default 4096 kB one
SET sift.async_maintenance = off;
INSERT INTO wa (body) SELECT 'alpha beta word' || (i % 50) || ' gamma' FROM generate_series(1, 3000) i;
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS old_sealed FROM sift.segments('wa_old');
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS new_sealed FROM sift.segments('wa_new');
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) = 0 AS def_sealed FROM sift.segments('wa_def');
DROP TABLE wa;
DROP EXTENSION pg_sift;
