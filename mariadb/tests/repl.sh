#!/usr/bin/env bash
# Replication and backup. The index is derived data local to a node; only the queue (a table, with its barrier rows) and
# the table travel through the binlog (ROW format). A replica builds its own index with CALL sift_rebuild and then drains
# the replicated queue with its own watermark. Three private servers (primary, replica, restore target) under
# $REPL_ROOT, ports from $REPL_PORT; they are stopped and deleted at the end.
#   mariadb/tests/repl.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRV=$HERE/server.sh
REPL_ROOT=${REPL_ROOT:-/tmp/sift-mdb-repl}
export SIFT_BUILD=${SIFT_BUILD:-/tmp/sift-mariadb-build}
fail=0 total=0
NAME=replication
check() { total=$((total + 1)); if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail + 1)); fi; }

node() { local n=$1; shift; SIFT_MDB_ROOT=$REPL_ROOT/$n "$SRV" "$@"; }
sqln() { local n=$1; shift; node "$n" sql -N -B rt -e "$*"; }
port() { sed -n 's/^port=//p' "$REPL_ROOT/$1/my.cnf"; }
cleanup() { for n in primary replica restore; do node $n destroy 2>/dev/null; done; rm -rf "$REPL_ROOT"; }
trap '[[ -n ${KEEP:-} ]] || cleanup' EXIT
mkdir -p "$REPL_ROOT"

BASE=${REPL_PORT:-33420}
boot() {   # name: starts (or restarts) the node with its options
  SIFT_MDB_PORT=$BASE BUFFER_POOL=128M SERVER_ARGS=$(cat "$REPL_ROOT/$1.args") SIFT_MDB_ROOT=$REPL_ROOT/$1 "$SRV" restart || exit 1
}
start() {   # name server-id extra-args
  local n=$1 id=$2; shift 2
  echo "--server-id=$id --log-bin=$REPL_ROOT/$n/binlog --binlog-format=ROW --relay-log=$REPL_ROOT/$n/relay $*" >"$REPL_ROOT/$n.args"
  boot "$n"
  BASE=$((BASE + 1))
  node "$n" sql -e "CREATE DATABASE IF NOT EXISTS rt DEFAULT CHARACTER SET utf8mb4"
  { echo 'SET sql_log_bin=0;'; cat "$HERE/sql/functions.sql"; } | node "$n" sql rt     # the UDFs are registered per node
}
start primary 1
start replica 2 --read-only=1
node primary sql -e "CREATE USER 'repl'@'127.0.0.1' IDENTIFIED BY 'x'; GRANT REPLICATION SLAVE ON *.* TO 'repl'@'127.0.0.1'" >/dev/null
node primary sql -e "RESET MASTER" >/dev/null
node replica sql -e "STOP SLAVE; RESET SLAVE ALL; CHANGE MASTER TO MASTER_HOST='127.0.0.1', MASTER_PORT=$(port primary), MASTER_USER='repl', MASTER_PASSWORD='x', MASTER_USE_GTID=slave_pos; START SLAVE" >/dev/null
# the procedures, tables and the data come through the binlog
node primary sql rt <"$HERE/sql/sift.sql"

catchup() { node replica sql -N -B -e "SELECT MASTER_GTID_WAIT('$(sqln primary 'SELECT @@global.gtid_binlog_pos')', 60)"; }
both() {   # query: "count top10" on both nodes, to compare
  for n in primary replica; do
    echo "$(sqln $n "SELECT sift_count('rt', '$1')") $(sqln $n "SELECT IFNULL(GROUP_CONCAT(id ORDER BY n), '') FROM (SELECT sift_topk('rt', '$1', 10) AS r) x, JSON_TABLE(x.r, '\$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '\$.id')) j")"
  done
}
same() {   # name query
  local out; out=$(both "$2"); local a b; a=$(sed -n 1p <<<"$out"); b=$(sed -n 2p <<<"$out")
  check "$1: primary and replica agree ($a)" "$a" "$b"
}

sqln primary "CREATE TABLE rt (id BIGINT PRIMARY KEY, body TEXT)"
sqln primary "INSERT INTO rt SELECT seq, CONCAT('alpha doc', seq, IF(seq % 3 = 0, ' beta', ''), IF(seq % 5 = 0, ' gamma', '')) FROM seq_1_to_2000"
sqln primary "CALL sift_attach('rt', 'rt', 'id', 't.body', 32)" >/dev/null
sqln primary "CALL sift_sync('rt')" >/dev/null
check "the replica catches up" 0 "$(catchup)"
check "the replica has no index of its own yet" 0 "$(sqln replica "SELECT sift_exists('rt')")"
sqln replica "CALL sift_rebuild('rt')" >/dev/null
check "rebuild on the replica: the replica adds no barrier of its own" 1 "$(sqln replica "SELECT COUNT(*) FROM sift_q_rt WHERE op = 'B'")"
check "rebuild on the replica: documents" 2000 "$(sqln replica "SELECT JSON_VALUE(sift_stats('rt'), '\$.live')")"
same "after the rebuild" "beta AND gamma"
same "after the rebuild, OR" "beta OR gamma"

# primary writes: visible on the replica as soon as the queue row has replicated (the replica reads its queue), then drained
sqln primary "INSERT INTO rt VALUES (3000, 'alpha beta gamma fresh'), (3001, 'beta only fresh')"
sqln primary "UPDATE rt SET body = 'changed gamma' WHERE id = 5"
sqln primary "DELETE FROM rt WHERE id IN (3, 6)"
check "the replica catches up (writes)" 0 "$(catchup)"
same "queue rows replicated, no drain on the replica" "beta"
same "queue rows replicated, gamma" "gamma"
sqln primary "CALL sift_sync('rt')" >/dev/null
catchup >/dev/null
sqln replica "CALL sift_sync('rt')" >/dev/null
check "the replica drained its replicated queue" 0 "$(sqln replica "SELECT JSON_VALUE(sift_stats('rt'), '\$.pending')")"
same "after the drains" "beta"
same "after the drains, gamma" "gamma"
same "after the drains, AND NOT" "alpha AND NOT beta"
check "each node has its own watermark" "ok" "$([[ $(sqln primary "SELECT sift_watermark('rt')") -gt 0 && $(sqln replica "SELECT sift_watermark('rt')") -gt 0 ]] && echo ok)"

# the replica never writes the queue: a barrier is the primary's
check "the replica added no barrier of its own" "$(sqln primary "SELECT COUNT(*) FROM sift_q_rt")" "$(sqln replica "SELECT COUNT(*) FROM sift_q_rt")"

# a replica restarted without its index files (a fresh node, a restored disk) rebuilds
rm -rf "$REPL_ROOT/replica/data/.sift/rt"
boot replica
check "a node that lost its index files reports it" 0 "$(sqln replica "SELECT sift_exists('rt')")"
sqln replica "CALL sift_rebuild('rt')" >/dev/null
same "after the rebuild of a lost index" "gamma"

# pruning the queue: the rebuild is what a node that fell behind does
sqln primary "CALL sift_prune('rt', 0)" >/dev/null
catchup >/dev/null
sqln primary "INSERT INTO rt VALUES (3100, 'prune check gamma')"
sqln primary "CALL sift_sync('rt')" >/dev/null
catchup >/dev/null
sqln replica "CALL sift_sync('rt')" >/dev/null
same "after a prune and more writes" "gamma"

# backup: mariadb-dump of the database (tables, triggers, procedures), restore on another node, rebuild
sqln primary "INSERT INTO rt VALUES (3200, 'last gamma before the dump')"
mariadb-dump --no-defaults -uroot -S "$REPL_ROOT/primary/mysql.sock" --single-transaction --routines --triggers rt >"$REPL_ROOT/rt.sql"
start restore 3 --skip-log-bin
node restore sql rt <"$REPL_ROOT/rt.sql"
check "restore: the index is not in the dump" 0 "$(sqln restore "SELECT sift_exists('rt')")"
check "restore: the table is" "$(sqln primary 'SELECT COUNT(*) FROM rt')" "$(sqln restore 'SELECT COUNT(*) FROM rt')"
sqln restore "CALL sift_rebuild('rt')" >/dev/null
for q in "gamma" "beta AND NOT gamma" "alpha OR beta"; do
  check "restore: '$q' equals the primary after the rebuild" "$(sqln primary "SELECT sift_count('rt', '$q')")" "$(sqln restore "SELECT sift_count('rt', '$q')")"
done
sqln restore "INSERT INTO rt VALUES (9000, 'zeta restored')"
check "restore: the triggers work on the restored node" 1 "$(sqln restore "SELECT sift_count('rt', 'zeta')")"

echo "$((total - fail))/$total $NAME checks passed"
exit $((fail != 0))
