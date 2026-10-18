#!/usr/bin/env bash
# Smoke test of the release tarball of sift for MariaDB (scripts/package-mariadb.sh): the library of the tarball, for the series of
# the stock server on this machine, is loaded into a private server (mariadb/server.sh) and answers the quick start of the README.
# No corpus, no PostgreSQL: it runs inside a container of the stock MariaDB image.
#
#   mariadb/tests/smoke.sh DIR VERSION     DIR: the unpacked tarball, VERSION: the release it must report
set -euo pipefail

DIR=${1:?unpacked tarball directory}
VER=${2:?version}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SERIES=$(mariadbd --version | sed -E 's/.*Ver ([0-9]+\.[0-9]+).*/\1/')
export SIFT_MDB_ROOT=$(mktemp -d) SIFT_BUILD=$DIR/lib/$SERIES BUFFER_POOL=128M DB=smoke NAME=smoke
[[ $(id -u) != 0 ]] || export SERVER_ARGS=--user=root   # in a container the server runs as root
. "$HERE/tests/lib.sh"
trap '"$SRV" destroy' EXIT

"$SRV" start
"$SRV" sql -e "CREATE DATABASE smoke DEFAULT CHARACTER SET utf8mb4"
"$SRV" sql smoke <"$DIR/sql/functions.sql"
"$SRV" sql smoke <"$DIR/sql/sift.sql"
check_match "sift_version names the release and the server" "^sift-mariadb ${VER//./\\.} \\(format [0-9]+, pages [0-9]+\\.[0-9]+ layout [0-9]+, built for MariaDB $SERIES\\." "$(sql 'SELECT sift_version()')"

sql "CREATE TABLE articles (id BIGINT PRIMARY KEY, title VARCHAR(200), body MEDIUMTEXT)"
sql "INSERT INTO articles VALUES
  (1, 'Solar power', 'Solar power plants turn sunlight into electricity.'),
  (2, 'Wind farms', 'A wind farm is a power plant made of many turbines.'),
  (3, 'Coal', 'A coal power plant burns coal to boil water.'),
  (4, 'Pasta', 'How to cook pasta in salted water.')"
sql "CALL sift_attach('news', 'articles', 'id', \"CONCAT_WS(' ', t.title, t.body)\", 32)" >/dev/null
check "ranked top-10, from the queue" 1,3,2 "$(top 'solar OR "power plant"' 10 news)"
check "count" 2 "$(cnt 'power AND plant' news)"
check "matches" 3,4 "$(idx water news)"
sync news
check "ranked top-10, from the segment" 1,3,2 "$(top 'solar OR "power plant"' 10 news)"
sql "INSERT INTO articles VALUES (5, 'Hydro power', 'Hydroelectric power plants use water.')"
sql "UPDATE articles SET body = 'Pasta needs no power plant.' WHERE id = 4"
sql "DELETE FROM articles WHERE id = 3"
check "DML is visible at once" 2,4 "$(idx '"power plant"' news)"
check "prefix" 1,2,4,5 "$(idx 'pow*' news)"
check "wildcard" 5 "$(idx 'h?dro' news)"
check "fuzzy" 1 "$(idx 'solar~1' news)"
check "regular expression" 2 "$(idx '/^w.nd$/' news)"
sync news
sql "SELECT sift_merge('news', 'full')" >/dev/null
check "after a sync and a merge" 2,4 "$(idx '"power plant"' news)"
"$SRV" restart
check "after a restart" 2,4 "$(idx '"power plant"' news)"
sql "CALL sift_detach('news')" >/dev/null
finish
