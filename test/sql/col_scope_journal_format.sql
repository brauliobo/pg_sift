-- The journal of a scope is a key log of Format 1 (docs/SCOPES.md): a REINDEX keeps it there while a scope index of either format depends on it, and sift.health() does not offer it
-- an upgrade to Format 2.0, which would leave the scope without a journal it can read.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;

CREATE TABLE pairs (member int NOT NULL, tenant int NOT NULL, PRIMARY KEY (member, tenant));
INSERT INTO pairs SELECT m, 1 + m % 10 FROM generate_series(1, 200) m;
CREATE TABLE facts (id int PRIMARY KEY, member int, at timestamp, v int);
INSERT INTO facts SELECT i, 1 + i % 200, timestamp '2024-01-01' + i * interval '1 minute', i FROM generate_series(1, 5000) i;
CREATE INDEX facts_scope ON facts USING sift_col (id, member, at, v) WITH (scope = 'tenant: member -> pairs(member, tenant)');
SELECT count(*) FILTER (WHERE relation::text = 'facts_scope' AND format = '2.0') AS scope_f2, count(*) FILTER (WHERE relation::text ~ 'journal' AND format ~ '^1\.') AS journal_f1 FROM sift.indexes;
SELECT count(*) AS offered FROM sift.health() WHERE check_name = 'format_upgrade_available';

SELECT relation::regclass::text AS journal FROM sift.indexes WHERE relation::text ~ 'journal' \gset
REINDEX INDEX CONCURRENTLY :journal;
REINDEX INDEX :journal;
SELECT count(*) FILTER (WHERE relation::text = 'facts_scope' AND format = '2.0') AS scope_f2, count(*) FILTER (WHERE relation::text ~ 'journal' AND format ~ '^1\.') AS journal_f1 FROM sift.indexes;

DROP TABLE facts, pairs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
