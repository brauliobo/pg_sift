-- The three walks of a ranked search over its units (batches by bound, seeded sweep, sweep of a conjunction) are one result: the K best rows of every query
-- shape, with ties, across segments, write buffers, dead rows, a filter and a phrase, are those of the sort of every match, whatever the mode
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- a skewed vocabulary, as rank_prune.sql: queries mix common and rare terms; the repetitions vary tf and the lengths the norms
CREATE TABLE rm (id int PRIMARY KEY, body text, grp int) WITH (autovacuum_enabled = off);
CREATE FUNCTION rm_doc(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg('w' || (1 + floor(300 * power(abs(hashint4(i * 977 + j)::float8) / 2147483648, 3)))::int ||
		repeat(' w' || (1 + floor(300 * power(abs(hashint4(i * 977 + j + 7919)::float8) / 2147483648, 3)))::int, j % 3), ' ')
	FROM generate_series(1, 5 + (hashint4(i) & 31)) j
$$;
INSERT INTO rm SELECT i, rm_doc(i), i % 4 FROM generate_series(1, 4000) i;
CREATE INDEX rm_idx ON rm USING sift (body) WITH (write_buffer_limit = 64);
INSERT INTO rm SELECT i, rm_doc(i), i % 4 FROM generate_series(4001, 7000) i;
SELECT sift.promote('rm_idx') IS NOT NULL AS promoted;
INSERT INTO rm SELECT i, rm_doc(i), i % 4 FROM generate_series(7001, 8000) i;
DELETE FROM rm WHERE id % 13 = 0;
UPDATE rm SET body = body || ' w1 w2' WHERE id % 17 = 0;
ANALYZE rm;
SELECT count(*) > 1 AS several_segments FROM sift.segments('rm_idx') WHERE kind = 'immutable';

CREATE FUNCTION rm_rows(q text, k int, filt text DEFAULT 'true', full_ranking bool DEFAULT false) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('SELECT string_agg(tid || '' '' || score, '','' ORDER BY score DESC, tid) FROM (
		SELECT * FROM (SELECT ctid AS tid, sift.score(ctid) AS score FROM rm WHERE body @@> %L AND (%s)
			ORDER BY sift.score(ctid) DESC %s) a %s) t',
		q, filt, CASE WHEN full_ranking THEN 'OFFSET 0' ELSE 'LIMIT ' || k END,
		CASE WHEN full_ranking THEN 'LIMIT ' || k ELSE '' END) INTO r;
	RETURN r;
END $$;

CREATE TEMP TABLE rm_q (q text);
INSERT INTO rm_q VALUES
	('w1 OR w2 OR w3'), ('w1 w2'), ('w5 OR w40 OR w120 OR w250'), ('w1 OR w2 OR w3 OR w4 OR w5 OR w6 OR w7 OR w8 OR w9 OR w10 OR w11 OR w12'),
	('w2 w3 w4'), ('w10 OR w20 OR w30 OR w200 OR w290'), ('w1 OR w299'), ('w3^5 OR w4'), ('w7 OR w8^0.5 OR w9^3 OR w250^2'),
	('w1* OR w29*'), ('w1 OR w2* OR w30'), ('w1 NOT w2'), ('(w3 OR w4) NOT w5'), ('(w1 w2) OR (w3 w4) OR w5'),
	('w100 OR w101 OR w102 OR w103 OR w104 OR w105'), ('"w1 w2" OR w3'), ('w6 w7 w8 w9'), ('(w1 OR w2) w3'),
	('w299 OR w298 OR w297 OR w296'), ('"w1 w2"'), ('w1 w2 w3 w4 w5');
SELECT count(*) AS queries FROM rm_q;

-- queries whose K best differ from the sort of every match, per mode (-1: chosen by the query), k and filter: 0 expected everywhere
DO $$
DECLARE
	m int;
BEGIN
	FOREACH m IN ARRAY ARRAY[-1, 0, 1, 2] LOOP
		PERFORM set_config('sift.test_rank_mode', m::text, false);
		RAISE INFO 'mode %: % mismatching', m, (SELECT count(*) FROM unnest(ARRAY[1, 3, 10, 50, 400]) k,
			unnest(ARRAY['true', 'grp = 1', 'id % 3 = 0']) filt, rm_q
			WHERE rm_rows(q, k, filt) IS DISTINCT FROM rm_rows(q, k, filt, true));
	END LOOP;
END $$;

-- without a LIMIT the threshold never rises: the whole ranking is the same whatever the mode
CREATE TEMP TABLE rm_all AS SELECT m, q, rm_rows(q, 1000000, 'true', true) AS rows
FROM unnest(ARRAY[0, 1, 2]) m, rm_q WHERE set_config('sift.test_rank_mode', m::text, false) IS NOT NULL;
SELECT count(*) AS runs, count(*) FILTER (WHERE rows IS NULL) AS empty FROM rm_all;
SELECT count(*) AS queries_with_modes_that_disagree
FROM (SELECT q FROM rm_all GROUP BY q HAVING count(DISTINCT rows) > 1) d;

RESET sift.test_rank_mode;
DROP FUNCTION rm_rows(text, int, text, bool);
DROP TABLE rm;
DROP FUNCTION rm_doc(int);
DROP EXTENSION pg_sift;
