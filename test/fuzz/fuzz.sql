-- SQL-level fuzzing of sift.parse_query and sift.tokenize in the server (run
-- it under ASan: see test/fuzz/README.md). Loaded by scripts/fuzz.sh after
-- test/sql/reference.sql and test/diff/diff.sql (query generator).
--
-- Findings (table sift_fuzz.findings):
--   parse_error  parse_query failed with an SQLSTATE other than syntax/limit/regex errors
--   disagree     parse_query and the SQL reference disagree on accepting a query of
--                at most 2000 characters
--                (reported, not fatal: usually a grammar question for the design notes)
--   tokenize     a sift.tokenize invariant broke (positions 0..n-1, 1..255-byte
--                tokens, offsets inside the text, every token analyzes to itself)
-- Each input is logged (LOG "sift_fuzz input") before use, so a crash can be
-- traced to its input in the server log.

\set sift_fuzz_echo :ECHO
\set ECHO none
SET client_min_messages = warning;
CREATE SCHEMA IF NOT EXISTS sift_fuzz;
CREATE TABLE IF NOT EXISTS sift_fuzz.findings (kind text, input text, detail text);

-- A generated query with up to three hostile edits
CREATE OR REPLACE FUNCTION sift_fuzz.hostile_query() RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	q text := sift_diff.gen_query();
	p int;
BEGIN
	FOR i IN 1 .. sift_diff.rand_int(0, 3) LOOP
		p := sift_diff.rand_int(1, length(q) + 1);
		q := left(CASE sift_diff.rand_int(0, 9)
			WHEN 0 THEN left(q, p - 1)
			WHEN 1 THEN overlay(q PLACING (ARRAY['(', ')', '"', '{', '}', '^', '~', '/', '\', '*', '?', ';',
				' AND ', ' OR ', ' NOT ', '^99999', '~3', '^1.', '\\'])[sift_diff.rand_int(1, 19)] FROM p FOR 0)
			WHEN 2 THEN overlay(q PLACING '' FROM p FOR sift_diff.rand_int(1, 4))
			WHEN 3 THEN q || ' ' || q
			WHEN 4 THEN repeat('(', sift_diff.rand_int(28, 40)) || q || repeat(')', 40)
			WHEN 5 THEN q || repeat(' NOT x', sift_diff.rand_int(1, 300))
			WHEN 6 THEN overlay(q PLACING chr(sift_diff.rand_int(1, 55295)) FROM p FOR 1)
			WHEN 7 THEN repeat(q || ' OR ', sift_diff.rand_int(50, 150)) || q
			WHEN 8 THEN '"' || repeat('ab ', sift_diff.rand_int(28, 40)) || '"'
			ELSE repeat('x', sift_diff.rand_int(250, 400)) || '*'
		END, 30000);
	END LOOP;
	RETURN q;
END $$;

-- Random text: vocabulary, ASCII noise, Latin/Cyrillic/CJK/emoji, combining marks, controls
CREATE OR REPLACE FUNCTION sift_fuzz.hostile_text() RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT string_agg(CASE sift_diff.rand_int(0, 6)
		WHEN 0 THEN sift_diff.pick_word(false)
		WHEN 1 THEN chr(sift_diff.rand_int(32, 126))
		WHEN 2 THEN chr(sift_diff.rand_int(160, 591))
		WHEN 3 THEN chr(sift_diff.rand_int(768, 879))
		WHEN 4 THEN chr((ARRAY[1024, 12352, 19968, 128512, 8192, 65024])[sift_diff.rand_int(1, 6)] + sift_diff.rand_int(0, 200))
		WHEN 5 THEN chr(sift_diff.rand_int(1, 31))
		ELSE ' ' END, '')
	FROM generate_series(1, sift_diff.rand_int(0, 80))
$$;

CREATE OR REPLACE FUNCTION sift_fuzz.outcome(q text, ref bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF ref THEN
		PERFORM sift_ref.analyze_query(q);
	ELSE
		PERFORM sift.parse_query(q);
	END IF;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE;
END $$;

CREATE OR REPLACE FUNCTION sift_fuzz.tokenize_bug(t text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE
		WHEN count(*) > 0 AND (min(pos) <> 0 OR max(pos) <> count(*) - 1) THEN 'positions not 0..n-1'
		WHEN bool_or(octet_length(token) NOT BETWEEN 1 AND 255) THEN 'token length outside 1..255 bytes'
		WHEN bool_or(start_offset < 0 OR end_offset <= start_offset OR end_offset > length(t)) THEN 'offset out of range'
		WHEN bool_or((SELECT array_agg(x.token) FROM sift.tokenize(token) x) IS DISTINCT FROM ARRAY[token])
			THEN 'token does not analyze to itself'
	END
	FROM sift.tokenize(t)
$$;

CREATE OR REPLACE FUNCTION sift_fuzz.run(seed float8, n int) RETURNS TABLE (kind text, cases bigint) LANGUAGE plpgsql AS $$
DECLARE
	q text;
	s text;
	r text;
BEGIN
	PERFORM setseed(seed);
	FOR i IN 1 .. n LOOP
		q := sift_fuzz.hostile_query();
		RAISE LOG 'sift_fuzz input %', quote_literal(q);
		s := sift_fuzz.outcome(q, false);
		-- the plpgsql reference parser is quadratic; long inputs only probe sift
		r := CASE WHEN length(q) <= 2000 THEN sift_fuzz.outcome(q, true) ELSE s END;
		IF s NOT IN ('ok', '42601', '54000', '2201B') THEN
			INSERT INTO sift_fuzz.findings VALUES ('parse_error', q, s);
		ELSIF (s = 'ok') <> (r = 'ok') THEN
			INSERT INTO sift_fuzz.findings VALUES ('disagree', q, format('sift %s, reference %s', s, r));
		END IF;
		q := sift_fuzz.hostile_text();
		RAISE LOG 'sift_fuzz input %', quote_literal(q);
		s := sift_fuzz.tokenize_bug(q);
		IF s IS NOT NULL THEN
			INSERT INTO sift_fuzz.findings VALUES ('tokenize', q, s);
		END IF;
	END LOOP;
	RETURN QUERY SELECT f.kind, count(*) FROM sift_fuzz.findings f GROUP BY f.kind;
END $$;

RESET client_min_messages;
\set ECHO :sift_fuzz_echo
