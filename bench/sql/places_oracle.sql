-- Places oracle (bench/steps/places.sh check): exact answers computed in SQL from the places table alone.
-- Match set: a place matches a typed query when every typed word is a prefix of a word of its all-fields text. What a word
-- is depends on the engine (the flavor): split, lowercase alphanumeric runs (pg_search's edge_ngram tokenizer, and the
-- n-gram text of the pg_textsearch emulation); fold, the same with accents removed (sift); pg, the lexemes of PostgreSQL's
-- parser with the simple configuration (the native tsvector and bm25 indexes of rum and pg_textsearch).
-- Ranking: BM25 (k1 = 1.2, b = 0.75, Lucene idf) over the edge n-grams (1-15 letters) of the words, the analysis of pg_search's
-- edge_ngram(1, 15) index: the query is the distinct n-grams of the typed words, the score of a place the sum over those
-- n-grams. top: all-fields text; boost: text + 3 x name field (name and alternate names); filter: top restricted to
-- country_code = 'US'. Ties at rank 20 count as correct (the places scoring at least as much as the 20th).
-- Needs place_ngrams / place_ngram_terms (places_schema.sql).
CREATE EXTENSION IF NOT EXISTS unaccent;
CREATE TABLE place_word AS
SELECT DISTINCT id, fl, w FROM (
  SELECT id, 'split' AS fl, w FROM places, LATERAL regexp_split_to_table(lower(place_search_text(name, alternate_names, admin1, country, country_code, feature)), '[^[:alnum:]]+') w
  UNION ALL
  SELECT id, 'fold', w FROM places, LATERAL regexp_split_to_table(lower(unaccent(place_search_text(name, alternate_names, admin1, country, country_code, feature))), '[^[:alnum:]]+') w
  UNION ALL
  SELECT id, 'pg', unnest(tsvector_to_array(to_tsvector('simple', place_search_text(name, alternate_names, admin1, country, country_code, feature)))) FROM places) x
WHERE w <> '';
CREATE INDEX ON place_word (fl, w text_pattern_ops);

-- n-gram tokens per place and field (0: all-fields text, 1: name field) with their frequency, and the field lengths
CREATE TABLE place_tok AS
SELECT id, fld, term, count(*)::int AS tf FROM (
  SELECT id, 0 AS fld, unnest(string_to_array(place_ngrams(place_search_text(name, alternate_names, admin1, country, country_code, feature)), ' ')) AS term FROM places
  UNION ALL
  SELECT id, 1, unnest(string_to_array(place_ngrams(place_search_name(name, alternate_names)), ' ')) FROM places) t
GROUP BY 1, 2, 3;
CREATE INDEX ON place_tok (fld, term);
CREATE TABLE place_len AS SELECT id, fld, sum(tf)::int AS dl FROM place_tok GROUP BY 1, 2;
CREATE UNIQUE INDEX ON place_len (id, fld);
ANALYZE place_word; ANALYZE place_tok; ANALYZE place_len;

CREATE FUNCTION place_oracle_match(q text, fl text) RETURNS SETOF int LANGUAGE sql STABLE AS $$
  WITH w AS (SELECT DISTINCT x FROM regexp_split_to_table(lower(q), '[^[:alnum:]]+') x WHERE x <> '')
  SELECT pw.id FROM place_word pw JOIN w ON pw.w LIKE w.x || '%' WHERE pw.fl = $2 GROUP BY pw.id HAVING count(DISTINCT w.x) = (SELECT count(*) FROM w)
$$;

-- BM25 score of every place with at least one of the n-grams of q in field fld
CREATE FUNCTION place_oracle_bm25(q text, fld int) RETURNS TABLE (id int, score float8) LANGUAGE sql STABLE AS $$
  WITH st AS (SELECT count(*)::float8 AS n, avg(dl) AS avgdl FROM place_len WHERE place_len.fld = $2),
       df AS (SELECT term, count(*)::float8 AS df FROM place_tok WHERE place_tok.fld = $2 AND term = ANY (string_to_array(place_ngram_terms($1), ' ')) GROUP BY term)
  SELECT t.id, sum(ln(1 + (st.n - df.df + 0.5) / (df.df + 0.5)) * t.tf * 2.2 / (t.tf + 1.2 * (0.25 + 0.75 * l.dl / st.avgdl)))
  FROM place_tok t JOIN df USING (term) JOIN place_len l ON l.id = t.id AND l.fld = t.fld, st
  WHERE t.fld = $2 GROUP BY t.id
$$;

-- The oracle's answers for the first n queries of every class: for workload wl and flavor fl of query qid the places that match
-- (`hits`; for filter those of the US) and the ones a correct top-20 may return (`ok`: scoring at least as much as the 20th best).
CREATE TABLE place_expect (qid int, wl text, fl text, hits int[], ok int[], PRIMARY KEY (qid, wl, fl));
CREATE FUNCTION place_oracle_fill(n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r record; f text;
BEGIN
  CREATE TEMP TABLE IF NOT EXISTS ps (id int, ts float8, ns float8, us boolean);
  CREATE TEMP TABLE IF NOT EXISTS sc (id int, ts float8, ns float8);
  FOR r IN SELECT p.id, p.q FROM (SELECT *, row_number() OVER (PARTITION BY class ORDER BY id) AS rn FROM pq) p WHERE p.rn <= n LOOP
    TRUNCATE sc;
    INSERT INTO sc SELECT t.id, t.score, 0 FROM place_oracle_bm25(r.q, 0) t;
    UPDATE sc SET ns = nm.score FROM place_oracle_bm25(r.q, 1) nm WHERE nm.id = sc.id;
    FOREACH f IN ARRAY ARRAY['split', 'fold', 'pg'] LOOP
      TRUNCATE ps;
      INSERT INTO ps SELECT m, coalesce(sc.ts, 0), coalesce(sc.ns, 0), country_code = 'US'
      FROM place_oracle_match(r.q, f) m LEFT JOIN sc ON sc.id = m JOIN places ON places.id = m;
      INSERT INTO place_expect
      SELECT r.id, w.wl, f, (SELECT coalesce(array_agg(id), '{}') FROM ps WHERE w.wl <> 'filter' OR us),
             (SELECT coalesce(array_agg(id), '{}') FROM (SELECT id, ts + CASE w.wl WHEN 'boost' THEN 3 * ns ELSE 0 END AS s FROM ps WHERE w.wl <> 'filter' OR us) x
              WHERE s >= coalesce((SELECT min(s) FROM (SELECT ts + CASE w.wl WHEN 'boost' THEN 3 * ns ELSE 0 END AS s FROM ps WHERE w.wl <> 'filter' OR us ORDER BY 1 DESC LIMIT 20) y), 0) - 1e-9)
      FROM (VALUES ('match'), ('top'), ('boost'), ('filter')) w(wl);
    END LOOP;
  END LOOP;
END $$;

-- The comparison of one engine's answer with the oracle's, for workload wl: a (agree) is, for match, the Jaccard index of the engine's match
-- set and the oracle's (hits), for top | boost | filter the share of the returned places the oracle accepts as top-20 (ok; ties at rank 20
-- included); wrong: returned places that are no match (match: extra and missing); short: fewer places than min(20, matches).
CREATE FUNCTION place_score(wl text, got int[], hits int[], ok int[], OUT a numeric, OUT wrong int, OUT short int) LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE WHEN wl = 'match' THEN CASE WHEN cardinality(got) + cardinality(hits) = 0 THEN 1 ELSE
                cardinality(ARRAY(SELECT unnest(got) INTERSECT SELECT unnest(hits)))::numeric / cardinality(ARRAY(SELECT unnest(got) UNION SELECT unnest(hits))) END
              WHEN cardinality(got) = 0 THEN (cardinality(hits) = 0)::int
              ELSE cardinality(ARRAY(SELECT unnest(got) INTERSECT SELECT unnest(ok)))::numeric / cardinality(got) END,
         cardinality(ARRAY(SELECT unnest(got) EXCEPT SELECT unnest(hits))) + CASE WHEN wl = 'match' THEN cardinality(ARRAY(SELECT unnest(hits) EXCEPT SELECT unnest(got))) ELSE 0 END,
         CASE wl WHEN 'match' THEN 0 ELSE (cardinality(got) < least(20, cardinality(hits)))::int END
$$;

-- Per class: queries, exact (agree = 1), agree (mean), worst, wrong, short of the rows of the temp table chk.
CREATE FUNCTION place_report() RETURNS TABLE (class text, queries int, exact int, agree numeric, worst numeric, wrong int, short int) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT chk.class, count(*)::int, count(*) FILTER (WHERE chk.a = 1)::int, round(avg(chk.a), 3), round(min(chk.a), 3), sum(chk.wrong)::int, sum(chk.short)::int FROM chk GROUP BY chk.class ORDER BY chk.class;
END $$;

-- Runs the statement template of workload wl (:q :sq :sqb :tq :ng as in steps/places.sh) for the oracle's queries and compares.
CREATE FUNCTION place_check(wl text, tmpl text, fl text) RETURNS TABLE (class text, queries int, exact int, agree numeric, worst numeric, wrong int, short int)
LANGUAGE plpgsql AS $$
DECLARE r record; got int[]; sq text; sqb text; tq text;
BEGIN
  CREATE TEMP TABLE IF NOT EXISTS chk (class text, a numeric, wrong int, short int);
  TRUNCATE chk;
  FOR r IN SELECT p.id, p.class, p.q, e.hits, e.ok FROM pq p JOIN place_expect e ON e.qid = p.id AND e.wl = place_check.wl AND e.fl = place_check.fl ORDER BY p.id LOOP
    sq := regexp_replace(r.q, '(\S+)', '\1*', 'g');
    sqb := format('(%s) AND (name:{%s}^3 OR (%s))', sq, sq, sq);
    tq := (SELECT string_agg(quote_literal(lexeme) || ':*', ' & ') FROM unnest(to_tsvector('simple', r.q)));
    EXECUTE 'SELECT coalesce(array_agg(id), ''{}'') FROM (' || replace(replace(replace(replace(replace(tmpl, ':sqb', quote_literal(sqb)), ':sq', quote_literal(sq)),
            ':tq', quote_literal(tq)), ':ng', quote_literal(place_ngram_terms(r.q))), ':q', quote_literal(r.q)) || ') s' INTO got;
    INSERT INTO chk SELECT r.class, s.a, s.wrong, s.short FROM place_score(wl, got, r.hits, r.ok) s;
  END LOOP;
  RETURN QUERY SELECT * FROM place_report();
END $$;

-- The same for an engine outside Postgres: its answers are the rows of the temp table place_got (qid, ids), the ids it returned for the
-- query qid of pq (steps/xlib.sh xp_check).
CREATE FUNCTION place_check_ids(wl text, fl text) RETURNS TABLE (class text, queries int, exact int, agree numeric, worst numeric, wrong int, short int)
LANGUAGE plpgsql AS $$
BEGIN
  CREATE TEMP TABLE IF NOT EXISTS chk (class text, a numeric, wrong int, short int);
  TRUNCATE chk;
  INSERT INTO chk SELECT p.class, s.a, s.wrong, s.short
  FROM place_got g JOIN pq p ON p.id = g.qid JOIN place_expect e ON e.qid = p.id AND e.wl = place_check_ids.wl AND e.fl = place_check_ids.fl,
       LATERAL place_score(place_check_ids.wl, g.ids, e.hits, e.ok) s;
  RETURN QUERY SELECT * FROM place_report();
END $$;
