# sift with the adjacent-pair phrase index (DESIGN.md (g)): two-word phrases are answered from the
# index alone, longer ones are narrowed by their pairs. Same queries as sift.
. "$BENCH/engines/sift.sh"
INDEX="CREATE INDEX docs_fts ON docs USING sift (body) WITH (phrase_index = pairs);"
