-- a ranked search of one term ordered by the score and then the id: the rows tied at the 20th place are read by one search of the index
-- label: the top 20 of `body @@> 'qaatg'` (rank 500: 0.8% of the documents) ordered by `sift.score(ctid) DESC, id` on a BM25 index; the score of a one-term search is a function of the frequency and the length, so many documents tie at the 20th place. PostgreSQL ranks a GIN match with `ts_rank_cd`; the statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaatg') ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaatg' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
