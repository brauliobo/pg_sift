-- Places directory benchmark (GeoNames): the table, the typed query set and the search
-- functions a multi-field autocomplete indexes (the name apart from the concatenation of every field).
CREATE TABLE places (
  id int PRIMARY KEY, name text, alternate_names text, admin1 text, country text, country_code text,
  feature text, population int
);
\set f :dir /places.tsv
COPY places FROM :'f';

CREATE TABLE pq (id int PRIMARY KEY, class text, q text);
\set f :dir /queries.tsv
COPY pq FROM :'f';

CREATE FUNCTION place_search_name(name text, alternate_names text) RETURNS text
  LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT concat_ws(' ', name, alternate_names)$$;

CREATE FUNCTION place_search_text(name text, alternate_names text, admin1 text, country text, country_code text,
  feature text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT concat_ws(' ', name, alternate_names, admin1, country, country_code, feature)$$;

-- sift: the name section first, then every other field; unqualified queries search both, name:word the first
CREATE FUNCTION place_search_sections(name text, alternate_names text, admin1 text, country text, country_code text,
  feature text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT public.place_search_name(name, alternate_names) || chr(29) ||
         concat_ws(' ', admin1, country, country_code, feature)$$;

-- Edge n-grams (1-15 letters) of every word, the analysis of pg_search's edge_ngram(1, 15): a typed prefix becomes a
-- whole term. pg_textsearch has no prefix scoring, so its ranked workloads index this text (steps/places.sh).
CREATE FUNCTION place_ngrams(t text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT string_agg(left(w, n), ' ') FROM regexp_split_to_table(lower(t), '[^[:alnum:]]+') w, generate_series(1, least(length(w), 15)) n WHERE w <> ''$$;

-- the distinct n-grams of a typed query: pg_search scores each once, a BM25 query of pg_textsearch counts repeats
CREATE FUNCTION place_ngram_terms(t text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT string_agg(DISTINCT w, ' ') FROM unnest(string_to_array(place_ngrams(t), ' ')) w$$;

-- the fields that are not the name: weight B next to the name at A in rum's boost emulation (steps/places.sh)
CREATE FUNCTION place_search_rest(admin1 text, country text, country_code text, feature text) RETURNS text
  LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$SELECT concat_ws(' ', admin1, country, country_code, feature)$$;
