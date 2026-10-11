-- SiftQL fuzz driver (superuser, file readable by the server):
--   psql -v file=/abs/queries.copy -f tools/query/fuzz.sql
-- For each query: parse_query either succeeds or fails with 42601/54000;
-- the canonical form is a fixpoint and matches the same documents.
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS sift;
DROP TABLE IF EXISTS fuzz_q, fuzz_docs, fuzz_bad;
CREATE UNLOGGED TABLE fuzz_q (q text);
CREATE UNLOGGED TABLE fuzz_bad (q text, d1 text, err text);
COPY fuzz_q FROM :'file';
CREATE UNLOGGED TABLE fuzz_docs (body text);
INSERT INTO fuzz_docs VALUES
	('the quick brown fox jumps over the lazy dog'), ('Quick red Fox, lazy brown bear!'),
	('e-mail the honey bear about x86_64 builds in 2024'), ('жук and жучок under the 日本語 log'),
	('dog dog dog'), ('café naïve straße 😀'), ('aaa ab abc b bab'), (repeat('a', 300)), ('');

DO $$
DECLARE
	r record;
	d1 text;
	nok int := 0;
	nrej int := 0;
	nbad int := 0;
	t0 timestamptz := clock_timestamp();
BEGIN
	FOR r IN SELECT q FROM fuzz_q LOOP
		d1 := NULL;
		BEGIN
			d1 := sift.parse_query(r.q);
			IF d1 IS DISTINCT FROM sift.parse_query(d1) OR
			   EXISTS (SELECT FROM fuzz_docs WHERE (body @@> r.q) IS DISTINCT FROM (body @@> d1)) THEN
				INSERT INTO fuzz_bad VALUES (r.q, d1, 'round trip');
				nbad := nbad + 1;
			ELSE
				nok := nok + 1;
			END IF;
		EXCEPTION
			WHEN syntax_error OR program_limit_exceeded THEN
				nrej := nrej + 1;
			WHEN OTHERS THEN
				INSERT INTO fuzz_bad VALUES (r.q, d1, sqlstate || ' ' || sqlerrm);
				nbad := nbad + 1;
		END;
	END LOOP;
	RAISE NOTICE 'queries % ok % rejected % bad % in %', nok + nrej + nbad, nok, nrej, nbad,
		clock_timestamp() - t0;
END $$;
SELECT left(q, 120) AS q, left(d1, 120) AS d1, err FROM fuzz_bad LIMIT 20;
