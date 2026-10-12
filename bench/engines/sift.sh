# sift (this project), per DESIGN.md: CREATE INDEX ... USING sift (body);
#   body @@> 'a AND b' | 'a OR b' | '"a b"' | 'a AND NOT b';  ORDER BY sift.score(ctid) DESC
# Trace terms are lowercase plain words, so they never collide with the uppercase keywords or the
# SiftQL metacharacters. Skipped by the harness until the extension is installed.
REQUIRES=sift
SETUP="CREATE EXTENSION IF NOT EXISTS sift;"
INDEX="CREATE INDEX docs_fts ON docs USING sift (body);"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION sift_q(mode text, terms text[]) RETURNS text LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN array_to_string(terms, ' AND ')
    WHEN 'or'     THEN array_to_string(terms, ' OR ')
    WHEN 'phrase' THEN '"' || array_to_string(terms, ' ') || '"'
    WHEN 'not'    THEN array_to_string(terms[:cardinality(terms) - 1], ' AND ') || ' AND NOT ' || terms[cardinality(terms)]
  END
$$;
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT format('docs.body @@> %L', sift_q(mode, terms)) $$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'sift.score(docs.ctid) DESC' $$;
SQL
)
