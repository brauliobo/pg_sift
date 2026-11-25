-- the limit registry (src/core/limits.h): sift._test_limits() lists it, sift.test_limits lowers a hooked limit and refuses the rest
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

SELECT id, macro, dflt, effective = dflt AS unchanged, hooked FROM sift._test_limits() ORDER BY id;

-- a hooked limit is lowered for the session and comes back with RESET
SET sift.test_limits = 'query_bytes=64, phrase_terms=3';
SELECT id, dflt, effective FROM sift._test_limits() WHERE effective <> dflt ORDER BY id;

CREATE TABLE items (id int, body text);
INSERT INTO items SELECT i, 'alpha beta gamma delta epsilon w' || i % 7 FROM generate_series(1, 300) i;
CREATE INDEX items_idx ON items USING sift (body);

SELECT count(*) FROM items WHERE body @@> 'alpha';
SELECT count(*) FROM items WHERE body @@> repeat('alpha ', 20);
SELECT count(*) FROM items WHERE body @@> '"alpha beta gamma"';
SELECT count(*) FROM items WHERE body @@> '"alpha beta gamma delta"';

RESET sift.test_limits;
SELECT count(*) FROM items WHERE body @@> repeat('alpha ', 20);
SELECT count(*) FROM items WHERE body @@> '"alpha beta gamma delta"';
SELECT count(*) FROM sift._test_limits() WHERE effective <> dflt;

-- the setting rejects what would pass a test for the wrong reason
SET sift.test_limits = 'no_such_limit=1';
SET sift.test_limits = 'arms=1';
SET sift.test_limits = 'query_bytes=-1';
SET sift.test_limits = 'query_bytes=9999999';
SET sift.test_limits = 'query_bytes';
SET sift.test_limits = 'query_bytes=12x';
SHOW sift.test_limits;

-- 0 is a value: a query of any length is too long
SET sift.test_limits = 'query_bytes=0';
SELECT count(*) FROM items WHERE body @@> 'alpha';
RESET sift.test_limits;

DROP TABLE items;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
