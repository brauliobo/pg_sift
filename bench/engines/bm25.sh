# pgx-bm25 `bm25_native`, the installed system package (pacman -S pgx-bm25; found in the system extension and library directories), or,
# on a host without it, the extracted Arch package (BM25_PKG) or a build of its source tarball (BM25_SRC) by bench/steps/fetch.sh bm25,
# via extension_control_path. Its analyzer resolves `<language>_stem` in pg_catalog, so a
# `simple_stem` dictionary (simple template) gives it the same no-stemming analysis as the other engines.
# Queries that are not answered by WAND materialize their whole match set; the default 256 MB
# budget fails most M-tier queries, so it is raised to 2 GB.
REQUIRES=bm25_native
BM25_SETUP=$(cat <<'SQL'
CREATE EXTENSION IF NOT EXISTS bm25_native;
SET allow_system_table_mods = on;
CREATE TEXT SEARCH DICTIONARY pg_catalog.simple_stem (TEMPLATE = simple);
RESET allow_system_table_mods;
DO $$ BEGIN EXECUTE format('ALTER DATABASE %I SET bm25_native.max_match_memory = %s', current_database(), 2 * 1024 * 1024); END $$;
SQL
)
SETUP=$BM25_SETUP
INDEX="CREATE INDEX docs_fts ON docs USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'simple', stopwords = 'none');"
PRED="'docs.body @@@ ' || quote_literal(bm25_q(mode, terms)) || '::jsonb'"
RANK="'docs.body &@@ ' || quote_literal(bm25_q(mode, terms)) || '::jsonb'"
HELPERS=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bm25_q(mode text, terms text[]) RETURNS jsonb LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN bm25_boolean(must => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms) t))
    WHEN 'or'     THEN bm25_boolean(should => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms) t))
    WHEN 'phrase' THEN bm25_phrase('body', array_to_string(terms, ' '))
    WHEN 'not'    THEN bm25_boolean(must => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms[:cardinality(terms) - 1]) t),
                                    must_not => ARRAY[bm25_term('body', terms[cardinality(terms)])])
    WHEN 'prefix' THEN bm25_wildcard('body', terms[1] || '*')
  END
$$;
SQL
)

# places: a multi-field index over the two sections sift has (pname: name and alternate names, prest: region, country, code, feature;
# stored generated columns, so the table grows and filling them is the setup time). A typed word is a prefix wildcard on both fields
# (`bm25_wildcard`, a must per word, a should across the two fields) and the name field is boosted x3 with `bm25_boost`, the same match
# set as the other engines' all-fields prefix match. That is native for words of 3 letters or more: the extension refuses a wildcard
# shorter than that ("wildcard prefix too short (min 3)"), so a typed word of 1-2 letters is an emulation inside the same index: two more
# stored columns (sname, srest) hold the 1-2 letter edge n-grams of the words of the two sections and the word is matched as a whole
# term there (the same match set as a prefix of that length, scored by BM25 over the n-grams). `@@@` selects the rows and `&@@` orders
# them, both anchored on the first indexed column with the same query tree; the tree is built per query by bm25_pq.
# A wildcard that expands to more than bm25_native.wildcard_max_expansions (default 1000) terms is an error: at 236,262 places `san*` is one,
# so the database raises the limit (sift's max_expansions is raised the same way).
PLACES_FLAVOR="split"
PLACES_SETUP="$BM25_SETUP
ALTER DATABASE places_$ENGINE SET bm25_native.wildcard_max_expansions = 100000;
CREATE FUNCTION bm25_short(t text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS \$\$
  SELECT string_agg(left(w, n), ' ') FROM regexp_split_to_table(lower(t), '[^[:alnum:]]+') w, generate_series(1, least(length(w), 2)) n WHERE w <> ''
\$\$;
ALTER TABLE places ADD COLUMN pname text GENERATED ALWAYS AS ($PLACES_NAME) STORED,
  ADD COLUMN prest text GENERATED ALWAYS AS (place_search_rest(admin1, country, country_code, feature)) STORED,
  ADD COLUMN sname text GENERATED ALWAYS AS (bm25_short($PLACES_NAME)) STORED,
  ADD COLUMN srest text GENERATED ALWAYS AS (bm25_short(place_search_rest(admin1, country, country_code, feature))) STORED;
CREATE FUNCTION bm25_pq(typed text, name_boost float8) RETURNS jsonb LANGUAGE sql IMMUTABLE AS \$\$
  SELECT bm25_boolean(must => array_agg(bm25_boolean(should => CASE WHEN length(w) >= 3
    THEN ARRAY[bm25_boost(name_boost, bm25_wildcard('pname', w || '*')), bm25_wildcard('prest', w || '*')]
    ELSE ARRAY[bm25_boost(name_boost, bm25_term('sname', w)), bm25_term('srest', w)] END)))
  FROM regexp_split_to_table(lower(typed), '\s+') w WHERE w <> ''
\$\$;"
PLACES_INDEX="CREATE INDEX places_$ENGINE ON places USING bm25_native (pname, prest, sname, srest) INCLUDE (id)
  WITH (key_field = 'id', language = 'simple', stopwords = 'none')"
PLACES_INDEXES=places_$ENGINE
P_match="SELECT id FROM places WHERE pname @@@ bm25_pq(:q, 1)"
P_top="SELECT id FROM places WHERE pname @@@ bm25_pq(:q, 1) ORDER BY pname &@@ bm25_pq(:q, 1) LIMIT 20"
P_boost="SELECT id FROM places WHERE pname @@@ bm25_pq(:q, 3) ORDER BY pname &@@ bm25_pq(:q, 3) LIMIT 20"
P_filter="SELECT id FROM places WHERE pname @@@ bm25_pq(:q, 1) AND country_code = 'US' ORDER BY pname &@@ bm25_pq(:q, 1) LIMIT 20"
P_count="SELECT count(*) FROM places WHERE pname @@@ bm25_pq(:q, 1)"
P_scan="SELECT id FROM places WHERE pname @@@ bm25_pq(:q, 1) AND population >= 100000"
