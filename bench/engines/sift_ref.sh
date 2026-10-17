# The reference of `bench regress`: sift built from a git ref (SIFT_REF, `bench --ref`, default the tag in versions.env) by
# bench/steps/fetch.sh sift_ref. Its extension is renamed pg_sift_ref (control file, library, the library names inside the
# C code) so that it lives next to the current build in one cluster; the SQL it exposes (access method, schema `sift`, the
# queries) is the same, and it runs in a database of its own like every engine. Same queries as sift.
V_EXT=pg_sift_ref
. "$BENCH/engines/sift.sh"
