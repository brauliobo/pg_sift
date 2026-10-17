# rum over the same stored tsvector column as gin ('simple' config); top-k ordered by the index via <=>.
. "$BENCH/engines/gin.sh"
REQUIRES=rum
SETUP="CREATE EXTENSION IF NOT EXISTS rum; $SETUP"
INDEX="CREATE INDEX docs_fts ON docs USING rum (tsv rum_tsvector_ops);"
RANK="'docs.tsv <=> ' || quote_literal(tsq(mode, terms)) || '::tsquery'"

# places: a stored tsvector of the all-fields text (a generated column, so the table grows) with the same index; a typed prefix
# is the prefix tsquery `'gra':* & 'ho':*`, ranked with `<=>` (a proximity ranking, not BM25). rum has no per-field boost: the boost
# workload is an emulation next to the native index (reported as the `emulation` build), a second tsvector with the name words at
# weight A (1.0) and the other fields at B (0.4), so the name counts 2.5 times, not 3.
PLACES_FLAVOR="pg"
PLACES_SETUP="CREATE EXTENSION IF NOT EXISTS rum; ALTER TABLE places ADD COLUMN ptsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', $PLACES_TEXT)) STORED"
PLACES_INDEX="CREATE INDEX places_$ENGINE ON places USING rum (ptsv rum_tsvector_ops)"
PLACES_INDEXES=places_$ENGINE
PLACES_SETUP2="ALTER TABLE places ADD COLUMN pwsv tsvector GENERATED ALWAYS AS (setweight(to_tsvector('simple', $PLACES_NAME), 'A') || setweight(to_tsvector('simple', place_search_rest(admin1, country, country_code, feature)), 'B')) STORED"
PLACES_INDEX2="CREATE INDEX places_rum_boost ON places USING rum (pwsv rum_tsvector_ops)"
PLACES_INDEXES2=places_rum_boost
P_match="SELECT id FROM places WHERE ptsv @@ :tq::tsquery"
P_top="SELECT id FROM places WHERE ptsv @@ :tq::tsquery ORDER BY ptsv <=> :tq::tsquery LIMIT 20"
P_boost="SELECT id FROM places WHERE pwsv @@ :tq::tsquery ORDER BY pwsv <=> :tq::tsquery LIMIT 20"
P_filter="SELECT id FROM places WHERE ptsv @@ :tq::tsquery AND country_code = 'US' ORDER BY ptsv <=> :tq::tsquery LIMIT 20"
P_count="SELECT count(*) FROM places WHERE ptsv @@ :tq::tsquery"
P_scan="SELECT id FROM places WHERE ptsv @@ :tq::tsquery AND population >= 100000"
