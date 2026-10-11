-- SiftQL throughput (superuser, file readable by the server):
--   python3 tools/query/gen.py --mode valid -n 20000 -o /tmp/q.copy
--   flock /tmp/sift-perf.lock psql -v file=/tmp/q.copy -f tools/query/bench.sql
-- Reports parse+analyze+deparse cost per query and sift.match cost per
-- document, net of the same scan without the sift call.
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS sift;
DROP TABLE IF EXISTS bench_raw, bench_q, bench_docs;
CREATE UNLOGGED TABLE bench_raw (q text);
COPY bench_raw FROM :'file';
CREATE UNLOGGED TABLE bench_q (q text);
DO $$
DECLARE
	r record;
BEGIN
	FOR r IN SELECT q FROM bench_raw LOOP
		BEGIN
			PERFORM sift.parse_query(r.q);
			INSERT INTO bench_q VALUES (r.q);
		EXCEPTION WHEN syntax_error OR program_limit_exceeded THEN
		END;
	END LOOP;
END $$;
SELECT setseed(0.5);
CREATE UNLOGGED TABLE bench_docs AS
	SELECT string_agg(w, ' ') AS body
	FROM generate_series(1, 20000) d,
		 LATERAL (SELECT (ARRAY['the', 'quick', 'brown', 'fox', 'jumps', 'over', 'lazy', 'dog',
								'bear', 'honey', 'search', 'engine', 'index', 'query', 'phrase',
								'postgres', 'bitmap', 'ranking', 'score', 'token'])[1 + floor(random() * 20)::int]
					 || CASE WHEN random() < 0.1 THEN floor(random() * 1000)::text ELSE '' END AS w
				  FROM generate_series(1, 100 + d % 3)) t
	GROUP BY d;
VACUUM ANALYZE bench_q, bench_docs;

CREATE OR REPLACE FUNCTION bench_ms(sql text, reps int) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE
	best float8 := 'infinity';
	t timestamptz;
BEGIN
	FOR i IN 1..reps LOOP
		t := clock_timestamp();
		EXECUTE sql;
		best := least(best, extract(epoch FROM clock_timestamp() - t) * 1000);
	END LOOP;
	RETURN best;
END $$;

SELECT count(*) AS queries, round(avg(length(q))) AS avg_chars,
	   round(avg(octet_length(q))) AS avg_bytes FROM bench_q;

WITH t AS (
	SELECT bench_ms('SELECT sum(length(q)) FROM bench_q', 5) AS base,
		   bench_ms('SELECT sum(length(sift.parse_query(q))) FROM bench_q', 5) AS parse,
		   (SELECT count(*) FROM bench_q) AS n,
		   (SELECT sum(octet_length(q)) FROM bench_q) AS bytes)
SELECT round(((parse - base) * 1000 / n)::numeric, 2) AS parse_us_per_query,
	   round((n / ((parse - base) / 1000))::numeric) AS queries_per_s,
	   round((bytes / 1e6 / ((parse - base) / 1000))::numeric, 1) AS mb_per_s
FROM t;

SELECT q, round(((bench_ms(format('SELECT count(*) FROM bench_docs WHERE body @@> %L', q), 5)
			 - bench_ms('SELECT count(*) FROM bench_docs WHERE length(body) > 0', 5))
			* 1000 / 20000)::numeric, 2) AS match_us_per_doc,
	   (SELECT count(*) FROM bench_docs WHERE body @@> q) AS hits
FROM (VALUES ('fox'), ('fox dog bear'), ('"quick brown fox"'), ('qu* OR ran*'),
			 ('/sc.re[0-9]+/'), ('serch~1 NOT engine'), ('{fox dog} "lazy bear" NOT (honey OR token)')) v(q);

DROP FUNCTION bench_ms;
DROP TABLE bench_raw, bench_q, bench_docs;
