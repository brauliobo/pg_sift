# sift (this project): CREATE INDEX ... USING sift (body);
#   body @@> 'a AND b' | 'a OR b' | '"a b"' | 'a AND NOT b' | 'a*' | 'a~1';  ORDER BY sift.score(ctid) DESC
# Trace terms are lowercase plain words, so they never collide with the uppercase keywords or the
# SiftQL metacharacters. Skipped by the harness until the extension is installed.
# The variants (sift_pairs, sift_nopos, sift_ref: a build of an older ref under the extension name pg_sift_ref, see
# bench/steps/fetch.sh) set V_EXT (extension) and V_OPTS (index reloptions) and source this file.
SIFT_EXT=${V_EXT:-pg_sift} SIFT_OPTS=${V_OPTS:-}
REQUIRES=$SIFT_EXT
SETUP="CREATE EXTENSION IF NOT EXISTS $SIFT_EXT;"
INDEX="CREATE INDEX docs_fts ON docs USING sift (body)${SIFT_OPTS:+ WITH ($SIFT_OPTS)};"
PRED="'docs.body @@> ' || quote_literal(sift_q(mode, terms))"
RANK="CASE WHEN sift_q(mode, terms) IS NOT NULL THEN 'sift.score(docs.ctid) DESC' END"
HELPERS=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION sift_q(mode text, terms text[]) RETURNS text LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN array_to_string(terms, ' AND ')
    WHEN 'or'     THEN array_to_string(terms, ' OR ')
    WHEN 'phrase' THEN '"' || array_to_string(terms, ' ') || '"'
    WHEN 'not'    THEN array_to_string(terms[:cardinality(terms) - 1], ' AND ') || ' AND NOT ' || terms[cardinality(terms)]
    WHEN 'prefix' THEN terms[1] || '*'
    WHEN 'fuzzy'  THEN terms[1] || '~1'
  END
$$;
SQL
)

# places: a name section and a section of the other fields (the name boosted as `name:{...}^3`), every typed word a prefix; every
# workload is native. sift errors beyond sift.max_expansions (512) terms and a typed prefix of a common word expands further on 236k places.
PLACES_FLAVOR="fold"
PLACES_SETUP="CREATE EXTENSION IF NOT EXISTS $SIFT_EXT; ALTER DATABASE places_$ENGINE SET sift.max_expansions = 100000"
PLACES_INDEX="CREATE INDEX places_$ENGINE ON places USING sift (($PLACES_SECT)) WITH (fields = 'name', prefix_index = 3${SIFT_OPTS:+, $SIFT_OPTS})"
PLACES_INDEXES=places_$ENGINE
P_match="SELECT id FROM places WHERE ($PLACES_SECT) @@> :sq"
P_top="SELECT id FROM places WHERE ($PLACES_SECT) @@> :sq ORDER BY sift.score(ctid) DESC LIMIT 20"
P_boost="SELECT id FROM places WHERE ($PLACES_SECT) @@> :sqb ORDER BY sift.score(ctid) DESC LIMIT 20"
P_filter="SELECT id FROM places WHERE ($PLACES_SECT) @@> :sq AND country_code = 'US' ORDER BY sift.score(ctid) DESC LIMIT 20"
P_count="SELECT count(*) FROM places WHERE ($PLACES_SECT) @@> :sq"
P_scan="SELECT id FROM places WHERE ($PLACES_SECT) @@> :sq AND population >= 100000"
