-- the best 20 documents of a query, ranked by BM25 against the best 20 of ts_rank_cd
-- label: the top 20 of `body @@> 'qaaaf OR qaatg'` by `sift.score(ctid)` on a BM25 index; PostgreSQL ranks a GIN match with `ts_rank_cd`. The two rankings differ, so the statement returns the number of rows of the page (20); the ranking itself is checked by `bench check oracle`
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaaf | qaatg') ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaaaf | qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaaaf OR qaatg' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
