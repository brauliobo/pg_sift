# sift with the adjacent-pair phrase index: two-word phrases are answered from the
# index alone, longer ones are narrowed by their pairs. Same queries as sift.
V_OPTS="phrase_index = pairs"
. "$BENCH/engines/sift.sh"
