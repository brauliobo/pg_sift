-- Ranked top-k pruning with far more scored leaves than a query may name: three phrases of 100 terms are 300
-- plan leaves (a phrase is the AND of its terms), above the 256 the score upper bounds were derived for.
-- The top k must equal an exact float4 BM25 reference computed from the documents (the formulas and the
-- order of the sum of DESIGN.md (a).4), scores and ties included.
CREATE EXTENSION pg_sift;

-- documents hold whole phrases (all 100 terms in order) of the three, some of them, and filler words
CREATE TABLE rleaf (id int PRIMARY KEY, body text);
INSERT INTO rleaf
SELECT i,
	(SELECT string_agg(
		CASE WHEN p = 0 AND hashint4(i) % 4 = 0 THEN (SELECT string_agg('a' || t, ' ' ORDER BY t) FROM generate_series(0, 99) t)
			 WHEN p = 1 AND hashint4(i + 1) % 3 = 0 THEN (SELECT string_agg('b' || t, ' ' ORDER BY t) FROM generate_series(0, 99) t)
			 WHEN p = 2 AND hashint4(i + 2) % 5 = 0 THEN (SELECT string_agg('c' || t, ' ' ORDER BY t) FROM generate_series(0, 99) t)
			 ELSE 'f' || (abs(hashint4(i * 31 + p)) % 50) END, ' ' ORDER BY p)
	 FROM generate_series(0, 2) p)
	|| (SELECT string_agg(' a' || (abs(hashint4(i * 17 + j)) % 100) || ' b' || (abs(hashint4(i * 19 + j)) % 100), '') FROM generate_series(1, i % 40) j)
FROM generate_series(1, 900) i;
CREATE INDEX rleaf_idx ON rleaf USING sift (body);
ANALYZE rleaf;

-- the reference: tf of every term of every document, df, N, avgdl, and the score of the phrases' terms in the
-- order a0..a99 b0..b99 c0..c99 (float4 sums, rounded step by step like the C code)
CREATE TEMP TABLE rl_tok AS
SELECT ctid AS tid, t AS term, count(*)::int AS tf FROM rleaf, regexp_split_to_table(body, ' ') t WHERE t <> '' GROUP BY 1, 2;
CREATE TEMP TABLE rl_doc AS SELECT tid, sum(tf)::int AS dl FROM rl_tok GROUP BY tid;
CREATE TEMP TABLE rl_df AS SELECT term, count(*) AS df FROM rl_tok GROUP BY term;
CREATE TEMP TABLE rl_stat AS SELECT count(*) AS n, (sum(dl)::float8 / count(*)::float8)::real AS avgdl FROM rl_doc;

-- boosts: of the three phrases; w = (boost * idf) * (k1 + 1), s = w * (tf / (tf + knorm)); the terms of a phrase score in the
-- documents that hold it
CREATE FUNCTION rl_want(boosts real[]) RETURNS TABLE (tid tid, score real) LANGUAGE sql STABLE AS $$
	WITH u AS (SELECT term, (boosts[ascii(term) - 96] * ln(1::float8 + (st.n::float8 - df.df::float8 + 0.5::float8) / (df.df::float8 + 0.5::float8))::real) * (1.2::real + 1::real) AS w,
				(ascii(term) - 97) * 100 + substr(term, 2)::int AS ord
			FROM rl_df df, rl_stat st WHERE term ~ '^[abc][0-9]+$'),
		hit AS (SELECT t.tid, substr(term, 1, 1) AS ph FROM rl_tok t WHERE term ~ '^[abc][0-9]+$' GROUP BY 1, 2 HAVING count(*) = 100)
	SELECT tok.tid, sum(u.w * (tok.tf::real / (tok.tf::real + ((1.2::real * (1::real - 0.75::real)) + ((1.2::real * 0.75::real) / st.avgdl) * least(d.dl, 65535)::real))) ORDER BY u.ord)::real
	FROM rl_tok tok JOIN u USING (term) JOIN rl_doc d ON d.tid = tok.tid JOIN rl_stat st ON true
	JOIN hit ON hit.tid = tok.tid AND hit.ph = substr(term, 1, 1)
	GROUP BY tok.tid
$$;

CREATE FUNCTION rl_phrase(q int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT '"' || string_agg(c || t, ' ' ORDER BY t) || '"' FROM generate_series(0, 99) t, (VALUES (CASE q WHEN 0 THEN 'a' WHEN 1 THEN 'b' ELSE 'c' END)) v(c)
$$;
CREATE TEMP TABLE rl_q (q text, boosts real[]);
INSERT INTO rl_q VALUES
	(rl_phrase(0) || ' OR ' || rl_phrase(1) || ' OR ' || rl_phrase(2), '{1,1,1}'),
	(rl_phrase(0) || ' OR ' || rl_phrase(1) || '^3 OR ' || rl_phrase(2) || '^0.5', '{1,3,0.5}');
-- the query holds 3 leaves; the plan 300 scored ones
SELECT (SELECT count(*) FROM regexp_matches(q, '[abc][0-9]+', 'g')) AS terms FROM rl_q;
SELECT count(*) > 0 AS matches FROM rleaf WHERE body @@> (SELECT q FROM rl_q LIMIT 1);

-- the documents that hold one of the phrases, ranked by the reference
CREATE FUNCTION rl_diff(q text, boosts real[], k int) RETURNS bigint LANGUAGE sql AS $$
	WITH got AS (SELECT row_number() OVER (ORDER BY score DESC, tid) AS rank, tid, score FROM
			(SELECT ctid AS tid, sift.score(ctid) AS score FROM rleaf WHERE body @@> q
			 ORDER BY sift.score(ctid) DESC LIMIT k) g),
		want AS (SELECT row_number() OVER (ORDER BY score DESC, tid) AS rank, tid, score FROM
			(SELECT * FROM rl_want(boosts) ORDER BY score DESC, tid LIMIT k) w)
	SELECT count(*) FROM got FULL JOIN want USING (rank) WHERE got.tid IS DISTINCT FROM want.tid OR got.score IS DISTINCT FROM want.score
$$;
SELECT k, sum(rl_diff(q, boosts, k)) AS mismatches
FROM unnest(ARRAY[1, 3, 10, 30, 100, 500]) k, rl_q GROUP BY k ORDER BY k;

DROP FUNCTION rl_diff(text, real[], int), rl_want(real[]), rl_phrase(int);
DROP TABLE rleaf;
DROP EXTENSION pg_sift;
