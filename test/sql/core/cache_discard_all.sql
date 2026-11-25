-- DISCARD ALL (a pooler's server_reset_query) gives back what the backend keeps for its next statement: the caches of the extension hold less after it, and the statements go on as before.
-- DISCARD PLANS leaves them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;

CREATE TABLE orders (id int, tenant_id int, label text) USING sift_table;
INSERT INTO orders SELECT g, g % 5, 'label' || g % 40 FROM generate_series(1, 3000) g;
CREATE TABLE events (id int, tenant_id int, kind text);
INSERT INTO events SELECT g, g % 5, 'kind' || g % 40 FROM generate_series(1, 3000) g;
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, kind);

SELECT count(*), count(DISTINCT label) FROM orders;
SELECT count(*), count(DISTINCT kind) FROM events WHERE tenant_id = 2;
SELECT count(*), count(DISTINCT kind) FROM events WHERE tenant_id = 2;
SELECT cache_total AS held FROM sift._test_col_dict_cache() \gset
DISCARD PLANS;
SELECT cache_total = :held AS kept_by_discard_plans FROM sift._test_col_dict_cache();
DISCARD ALL;
SELECT cache_total < :held AS cache_smaller FROM sift._test_col_dict_cache();
SELECT count(*), count(DISTINCT label) FROM orders;
SELECT count(*), count(DISTINCT kind) FROM events WHERE tenant_id = 2;

DROP TABLE orders, events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
