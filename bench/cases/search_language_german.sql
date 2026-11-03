-- a search on an index with a language analyzer (the stemmer of a PostgreSQL text search configuration), against the GIN index of the same configuration
-- label: `count(*)` of `body @@> 'qaatg OR qacyy'` on `USING sift (body) WITH (stemmer = 'german')`; PostgreSQL is a GIN index on `to_tsvector('german', body)`
--@ pg
SELECT count(*) FROM docs WHERE to_tsvector('german', body) @@ to_tsquery('german', 'qaatg | qacyy')
--@ sift
SELECT count(*) FROM docs WHERE body @@> 'qaatg OR qacyy'
--@ setup
DROP INDEX IF EXISTS docs_sift;
DROP INDEX IF EXISTS docs_gin_german;
CREATE INDEX docs_sift ON docs USING sift (body) WITH (stemmer = 'german');
CREATE INDEX docs_gin_german ON docs USING gin (to_tsvector('german', body))
