-- Exact-match oracle: PostgreSQL's tsvector parser with configurable folding.
-- psql vars: accents (fold|keep), stem (off|<snowball language>, e.g. english).
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS unaccent;
SELECT format('%s|%s', :'accents', :'stem') AS cfg_key \gset

SELECT coalesce(obj_description(to_regclass('oracle_tsv'), 'pg_class'), '') <> :'cfg_key' AS rebuild \gset

\if :rebuild
DROP TABLE IF EXISTS oracle_tsv;
DROP TEXT SEARCH CONFIGURATION IF EXISTS oracle_cfg;
CREATE TEXT SEARCH CONFIGURATION oracle_cfg (COPY = simple);
SELECT format('ALTER TEXT SEARCH CONFIGURATION oracle_cfg ALTER MAPPING FOR
  asciiword, word, numword, asciihword, hword, numhword, hword_asciipart, hword_part, hword_numpart WITH %s',
  concat_ws(', ', CASE :'accents' WHEN 'fold' THEN 'unaccent' END,
            CASE :'stem' WHEN 'off' THEN 'simple' ELSE :'stem' || '_stem' END)) \gexec
CREATE TABLE oracle_tsv AS SELECT id, to_tsvector('oracle_cfg', body) AS tsv FROM docs;
CREATE INDEX ON oracle_tsv USING gin (tsv);
ANALYZE oracle_tsv;
COMMENT ON TABLE oracle_tsv IS :'cfg_key';
\endif

-- Compares the engine's match set and count(*) with the oracle for one logical query.
CREATE OR REPLACE FUNCTION oracle_check(mode text, terms text[],
  OUT n_oracle bigint, OUT n_engine bigint, OUT n_count bigint, OUT missing bigint, OUT extra bigint,
  OUT missing_ids bigint[], OUT extra_ids bigint[]) LANGUAGE plpgsql AS $$
DECLARE m text := bench_stmt('match', mode, terms);
BEGIN
  IF m IS NULL THEN RETURN; END IF;
  DROP TABLE IF EXISTS pg_temp.e, pg_temp.o;
  EXECUTE 'CREATE TEMP TABLE e AS ' || m;
  CREATE TEMP TABLE o AS SELECT id FROM oracle_tsv WHERE tsv @@ to_tsquery('oracle_cfg', tsq(mode, terms));
  EXECUTE bench_stmt('count', mode, terms) INTO n_count;
  SELECT count(*) INTO n_engine FROM e;
  SELECT count(*) INTO n_oracle FROM o;
  SELECT count(*), (array_agg(id ORDER BY id))[:5] INTO missing, missing_ids FROM (SELECT id FROM o EXCEPT ALL SELECT id FROM e) d;
  -- EXCEPT ALL also reports rows the engine returned twice.
  SELECT count(*), (array_agg(id ORDER BY id))[:5] INTO extra, extra_ids FROM (SELECT id FROM e EXCEPT ALL SELECT id FROM o) d;
END $$;

-- Checked queries: the trace's and/or/phrase queries plus a NOT variant of each conjunction.
CREATE OR REPLACE VIEW oracle_queries AS
SELECT id, sub, mode, terms FROM queries
UNION ALL
SELECT id + 1000000, sub, 'not', terms FROM queries WHERE mode = 'and';
