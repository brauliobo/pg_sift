-- Engine-independent query rendering. Each engine adapter defines
--   bench_pred(mode, terms) -> boolean SQL over docs (NULL = mode unsupported as a filter)
--   bench_rank(mode, terms) -> ORDER BY expression incl. direction (NULL = no top-k)
-- Modes: and | or | phrase | not (all terms but the last, minus the last term) | prefix (term*) |
-- fuzzy (edit distance 1). Unsupported modes must render NULL in both functions.

CREATE OR REPLACE FUNCTION tsq(mode text, terms text[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE mode
    WHEN 'and'    THEN string_agg(quote_literal(t), ' & ')
    WHEN 'or'     THEN string_agg(quote_literal(t), ' | ')
    WHEN 'phrase' THEN string_agg(quote_literal(t), ' <-> ')
    WHEN 'not'    THEN string_agg(CASE WHEN o = cardinality(terms) THEN '!' ELSE '' END || quote_literal(t), ' & ')
    WHEN 'prefix' THEN string_agg(quote_literal(t) || ':*', ' & ')
  END
  FROM unnest(terms) WITH ORDINALITY AS u(t, o)
$$;

CREATE OR REPLACE FUNCTION bench_stmt(kind text, mode text, terms text[]) RETURNS text LANGUAGE plpgsql STABLE AS $$
DECLARE
  pred text := bench_pred(mode, terms);
  rank text := bench_rank(mode, terms);
  ranked boolean := kind IN ('top10', 'filter', 'join');
  extra text := CASE kind
    WHEN 'filter' THEN format('docs.created >= %L', (SELECT v FROM params WHERE k = 'recent_cutoff'))
    WHEN 'join'   THEN 'categories.region = 3'
  END;
  conds text := concat_ws(' AND ', pred, extra);
BEGIN
  IF (ranked AND rank IS NULL) OR (NOT ranked AND pred IS NULL) THEN
    RETURN NULL;
  END IF;
  RETURN CASE kind
    WHEN 'count' THEN 'SELECT count(*) FROM docs WHERE ' || pred
    WHEN 'match' THEN 'SELECT docs.id FROM docs WHERE ' || pred
    ELSE 'SELECT docs.id FROM docs'
      || CASE kind WHEN 'join' THEN ' JOIN categories ON categories.id = docs.category_id' ELSE '' END
      || CASE WHEN conds <> '' THEN ' WHERE ' || conds ELSE '' END
      || ' ORDER BY ' || rank || ' LIMIT 10'
  END;
END $$;

-- Workload -> statement kind and query modes. A workload runs only if every mode renders.
CREATE TABLE IF NOT EXISTS workloads (wl text PRIMARY KEY, kind text, modes text[]);
INSERT INTO workloads VALUES
  ('and', 'top10', '{and}'), ('or', 'top10', '{or}'), ('phrase', 'top10', '{phrase}'),
  ('mixed', 'top10', '{and,or,phrase}'), ('count', 'count', '{and,or,phrase}'),
  ('filter', 'filter', '{and,or,phrase}'), ('join', 'join', '{and,or,phrase}')
ON CONFLICT DO NOTHING;

CREATE TABLE IF NOT EXISTS stmts (wl text, n int, qid int, stmt text, PRIMARY KEY (wl, n));

CREATE OR REPLACE FUNCTION bench_build_stmts() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE w workloads;
BEGIN
  TRUNCATE stmts;
  FOR w IN SELECT * FROM workloads LOOP
    CONTINUE WHEN EXISTS (
      SELECT FROM unnest(w.modes) m WHERE bench_stmt(w.kind, m, '{x,y}') IS NULL);
    INSERT INTO stmts
    SELECT w.wl, row_number() OVER (ORDER BY q.id), q.id, bench_stmt(w.kind, q.mode, q.terms)
    FROM queries q WHERE q.mode = ANY (w.modes);
    RETURN NEXT w.wl;
  END LOOP;
END $$;

-- Runs a statement returning ids and gives them back as a sorted array.
CREATE OR REPLACE FUNCTION bench_ids(stmt text) RETURNS bigint[] LANGUAGE plpgsql AS $$
DECLARE r bigint[];
BEGIN
  EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (%s) s', stmt) INTO r;
  RETURN r;
END $$;

-- Quick suite (sql/quick.sql view, same session): one stmts workload `q_<class>` per class whose
-- queries all render.
CREATE OR REPLACE FUNCTION quick_build_stmts() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE c text;
BEGIN
  DELETE FROM stmts WHERE wl LIKE 'q\_%';
  FOR c IN SELECT DISTINCT class FROM quick ORDER BY 1 LOOP
    CONTINUE WHEN EXISTS (SELECT FROM quick q WHERE q.class = c AND bench_stmt(q.kind, q.mode, q.terms) IS NULL);
    INSERT INTO stmts SELECT 'q_' || c, row_number() OVER (ORDER BY q.sub, q.mode), q.sub, bench_stmt(q.kind, q.mode, q.terms)
    FROM quick q WHERE q.class = c;
    RETURN NEXT c;
  END LOOP;
END $$;
