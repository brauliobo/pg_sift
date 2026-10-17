# sift without word positions in its postings (reloption positions = off): the small index, a phrase is rechecked against
# the heap. The default `sift` index (positions on) answers phrases from the postings. Same queries as sift; skipped when
# the installed sift build has no such reloption (PROBE).
V_OPTS="positions = off"
. "$BENCH/engines/sift.sh"
PROBE="CREATE INDEX ON probe USING sift (t) WITH (positions = off)"
