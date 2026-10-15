-- Places directory benchmark (GeoNames): the table, the typed query set and the search
-- functions a multi-field autocomplete indexes (the name apart from the concatenation of every field).
CREATE EXTENSION IF NOT EXISTS pg_search CASCADE;
CREATE EXTENSION IF NOT EXISTS sift;

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
