# sift without word positions in its postings (reloption positions = off): the small index, a phrase is rechecked against
# the heap. The default `sift` index (positions on) answers phrases from the postings. Same queries as sift; skipped when
# the installed sift build has no such reloption (PROBE).
. "$BENCH/engines/sift.sh"
INDEX="CREATE INDEX docs_fts ON docs USING sift (body) WITH (positions = off);"
PROBE="CREATE INDEX ON probe USING sift (t) WITH (positions = off)"
