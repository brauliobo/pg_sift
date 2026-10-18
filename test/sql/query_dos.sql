-- What any role may ask of the server: the memory knobs of wide prefixes are superuser settings, a regex
-- cannot back-reference (the one feature that makes a match exponential), and a scan of a big dictionary
-- obeys statement_timeout and cancel.
CREATE EXTENSION pg_sift;
CREATE ROLE sift_dos_user;
GRANT USAGE ON SCHEMA sift TO sift_dos_user;

SET ROLE sift_dos_user;
SET sift.max_expansions = 1000000;
SET sift.prefix_cache_size = 1048576;
SHOW sift.max_expansions;
RESET ROLE;
SET sift.max_expansions = 1000;
SHOW sift.max_expansions;
RESET sift.max_expansions;

-- 300k distinct terms
CREATE TABLE dos (id int PRIMARY KEY, body text);
INSERT INTO dos SELECT i, 'x' || md5(i::text) || ' y' || md5((i * 7)::text) FROM generate_series(1, 150000) i;
CREATE INDEX dos_idx ON dos USING sift (body);
ANALYZE dos;

-- back-references are refused (a pattern like (a*)*\1 takes exponential time)
SELECT sift.parse_query('/(a+)\1/');
SELECT sift.parse_query('/(a|b)c\2/');
SELECT sift.parse_query('/(a+)b\\1/') IS NOT NULL AS escaped_backslash_is_not_a_backreference;
SELECT count(*) FROM dos WHERE body @@> '/(x[0-9a-f]{3}).*\1/';

-- a scan of the whole dictionary stops at the timeout, in the expansion of a pattern that matches nothing
SET statement_timeout = 1;
SELECT count(*) FROM dos WHERE body @@> '*q*';
SELECT count(*) FROM dos WHERE body @@> '/.*q.*q.*/';
SELECT count(*) FROM dos WHERE body @@> '*q*' OR id < 10;
RESET statement_timeout;
SELECT count(*) FROM dos WHERE body @@> '*q*';

DROP TABLE dos;
REVOKE USAGE ON SCHEMA sift FROM sift_dos_user;
DROP ROLE sift_dos_user;
DROP EXTENSION pg_sift;
