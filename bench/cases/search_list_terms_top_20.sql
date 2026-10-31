-- a ranked search of two terms that are lists (fewer postings than sift.grouped_min_df, 8192): their blocks are decoded where the search reads them
-- label: the top 20 of `body @@> 'qaatg qacyy'` (ranks 500 and 2000: 0.8% and 0.4% of the documents, lists from 1,000,000 documents; at 100,000 every term but the first four is one) by `sift.score(ctid)` on a BM25 index; PostgreSQL ranks a GIN match with `ts_rank_cd`. The statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaatg & qacyy') ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaatg & qacyy')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaatg qacyy' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
