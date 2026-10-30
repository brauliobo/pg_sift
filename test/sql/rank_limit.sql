-- Ranked scans with a huge LIMIT or OFFSET: the top-k heap grows with the hits found, not with the
-- limit, so LIMIT 2147483647 ("all") and an OFFSET of tens of millions work (they raised
-- "invalid memory alloc request size" above 67 million rows)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE TABLE rl (id int PRIMARY KEY, body text);
INSERT INTO rl SELECT i, 'common w' || i % 100 || ' x' || i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX rl_idx ON rl USING sift (body);
ANALYZE rl;

CREATE FUNCTION rl_plan(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_text(q) $$;

-- the plan is the ranked scan with the limit as a hint
SELECT rl_plan('SELECT id FROM rl WHERE body @@> ''common'' ORDER BY sift.score(ctid) DESC LIMIT 100000000') ~ 'SiftScan' AS ranked_scan;

-- the same rows as the small limit's, in the same order
CREATE TABLE rl_ref AS
SELECT row_number() OVER () AS pos, id FROM
	(SELECT id FROM rl WHERE body @@> 'common w5 OR x3' ORDER BY sift.score(ctid) DESC, id LIMIT 20000) t;
SELECT count(*) FROM rl_ref;

SELECT count(*) = (SELECT count(*) FROM rl_ref) AS limit_1e8,
	   md5(string_agg(id::text, ',' ORDER BY pos)) = (SELECT md5(string_agg(id::text, ',' ORDER BY pos)) FROM rl_ref) AS same_order
FROM (SELECT row_number() OVER () AS pos, id FROM
	(SELECT id FROM rl WHERE body @@> 'common w5 OR x3' ORDER BY sift.score(ctid) DESC, id LIMIT 100000000) t) t;

SELECT count(*) = 20000 AS limit_int_max
FROM (SELECT id FROM rl WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC, id LIMIT 2147483647) t;

SELECT count(*) = 20000 AS limit_bigint_max
FROM (SELECT id FROM rl WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC, id LIMIT 9223372036854775807) t;

-- an OFFSET past the matches returns nothing; the limit added to it is above 67 million
SELECT count(*) AS offset_7e7 FROM
	(SELECT id FROM rl WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC, id OFFSET 70000000 LIMIT 10) t;
SELECT count(*) AS offset_big_limit FROM
	(SELECT id FROM rl WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC, id OFFSET 19990 LIMIT 100000000) t;

-- a ranked scan drained without a LIMIT ranks every match in one search
SELECT count(*) = (SELECT count(*) FROM rl_ref) AS drained, md5(string_agg(id::text, ',' ORDER BY pos)) = (SELECT md5(string_agg(id::text, ',' ORDER BY pos)) FROM rl_ref) AS same_order
FROM (SELECT row_number() OVER () AS pos, id FROM
	(SELECT id FROM rl WHERE body @@> 'common w5 OR x3' ORDER BY sift.score(ctid) DESC, id OFFSET 0) t) t;

DROP FUNCTION rl_plan(text);
DROP TABLE rl, rl_ref;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
