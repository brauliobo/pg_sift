-- a ranked search joined to the categories, the join being a test inside the BM25 scan (INCLUDE category_id)
-- label: the top 20 of `body @@> 'qaaaf OR qaatg'` joined to `categories` with `region = 3` on `USING sift (body) INCLUDE (category_id, created_at)`; PostgreSQL joins the GIN match. The statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT d.id FROM docs d JOIN categories c ON c.id = d.category_id WHERE d.tsv @@ to_tsquery('simple', 'qaaaf | qaatg') AND c.region = 3 ORDER BY ts_rank_cd(d.tsv, to_tsquery('simple', 'qaaaf | qaatg')) DESC, d.id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT d.id FROM docs d JOIN categories c ON c.id = d.category_id WHERE d.body @@> 'qaaaf OR qaatg' AND c.region = 3 ORDER BY sift.score(d.ctid) DESC, d.id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body) INCLUDE (category_id, created_at)
