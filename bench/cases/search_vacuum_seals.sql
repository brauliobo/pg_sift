-- a ranked search after rounds of updates, each followed by a VACUUM: the VACUUM seals the open write buffer, so a search does not read every document of it
-- label: the top 20 of `body @@> 'qaaaf OR qaatg'` on a copy of the documents whose 4% of the rows are updated in each of 8 rounds with a `VACUUM` after each (the setup); PostgreSQL is a GIN index on `to_tsvector('simple', body)` of the same table. The statement returns the number of rows of the page (20)
--@ setup
DROP TABLE IF EXISTS docs_churn;
CREATE TABLE docs_churn AS SELECT id, body FROM docs;
ALTER TABLE docs_churn ADD PRIMARY KEY (id);
CREATE INDEX docs_churn_gin ON docs_churn USING gin (to_tsvector('simple', body));
CREATE INDEX docs_churn_sift ON docs_churn USING sift (body);
VACUUM (FREEZE, ANALYZE) docs_churn;
UPDATE docs_churn SET body = body || ' qaatg' WHERE id % 25 = 0;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaaaf' WHERE id % 25 = 1;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaatg' WHERE id % 25 = 2;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaaaf' WHERE id % 25 = 3;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaatg' WHERE id % 25 = 4;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaaaf' WHERE id % 25 = 5;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaatg' WHERE id % 25 = 6;
VACUUM docs_churn;
UPDATE docs_churn SET body = body || ' qaaaf' WHERE id % 25 = 7;
VACUUM docs_churn
--@ pg
SELECT count(*) FROM (SELECT id FROM docs_churn WHERE to_tsvector('simple', body) @@ to_tsquery('simple', 'qaaaf | qaatg') ORDER BY ts_rank_cd(to_tsvector('simple', body), to_tsquery('simple', 'qaaaf | qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs_churn WHERE body @@> 'qaaaf OR qaatg' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
