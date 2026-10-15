# Benchmarks

sift against ParadeDB pg_search (and sift with `phrase_index = pairs`) on two public datasets, measured with
`bench/ab.sh` and `bench/places.sh` (method, datasets, licenses and checksums: `bench/README.md`). Every engine
indexes the same rows with the same analysis (lowercase, no stemming, no stopwords) and answers the same
queries from the same cache state; cells are run one at a time with the engine order rotated every round, and
the table reports medians over the rounds together with the host load average.
