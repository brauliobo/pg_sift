CREATE INDEX document_author_idx ON document (author_id);
CREATE INDEX document_fts_idx ON document USING gin (to_tsvector('simple', body));
CREATE INDEX author_country_idx ON author (country);
