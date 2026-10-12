#!/usr/bin/env bash
# Soak test: the 1,000 UPDATE/s mixed workload (DESIGN.md M4) on the
# scripts/pgtest.sh cluster.
#
#   scripts/soak.sh [--duration SEC] [--rate TPS] [--rows N] [--clients N] [--check-every SEC] [--am sift|gin]
#
# pgbench writers rewrite indexed text (non-HOT updates, 80% of --rate, so the
# default is 1,000 UPDATE/s), insert and delete while readers run count and
# ranked queries.
# Every --check-every seconds one REPEATABLE READ snapshot compares index
# results with the SQL reference, and the index is verified (sift.verify with
# heapallindexed, or amcheck for GIN). --am gin runs the same harness on a GIN
# control index; sift is skipped until its insert path lands. Exit 1 on any
# mismatch, verification error or failed transaction.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGDATABASE=sift_soak PGOPTIONS='-c client_min_messages=warning'
OUT=${SIFT_TMP:-/tmp/sift-arch}/soak
DURATION=600 RATE=1250 ROWS=100000 CLIENTS=8 EVERY=30 AM=sift
while (( $# )); do
	case $1 in
		--duration) DURATION=$2 ;; --rate) RATE=$2 ;; --rows) ROWS=$2 ;; --clients) CLIENTS=$2 ;;
		--check-every) EVERY=$2 ;; --am) AM=$2 ;;
		*) sed -n '2,16p' "$0"; exit 1 ;;
	esac
	shift 2
done

q() { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
log() { printf '[soak %s] %s\n' "$(date +%T)" "$*" | tee -a "$OUT/soak.log" >&2; }

case $AM in
	sift)
		INDEX="CREATE INDEX soak_idx ON soak USING sift (body)"
		VERIFY="SELECT sift.verify('soak_idx', true)"
		match() { echo "body @@> ($1)"; }
		RANKED="SELECT id FROM soak WHERE $(match "'w' || :w") ORDER BY sift.score(ctid) DESC LIMIT 10;" ;;
	gin)
		INDEX="CREATE INDEX soak_idx ON soak USING gin (to_tsvector('simple', body))"
		VERIFY="SELECT gin_index_check('soak_idx')"
		match() { echo "to_tsvector('simple', body) @@ to_tsquery('simple', $1)"; }
		RANKED="SELECT id FROM soak WHERE $(match "'w' || :w") ORDER BY id DESC LIMIT 10;" ;;
	*) echo "--am sift|gin" >&2; exit 1 ;;
esac

rm -rf "$OUT" && mkdir -p "$OUT"
psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS sift_soak WITH (FORCE)" -c "CREATE DATABASE sift_soak"
q -c "CREATE EXTENSION sift" -c "CREATE EXTENSION amcheck" -v sift_ref_lib=1 -f "$REPO/test/sql/reference.sql"
q <<-SQL
	CREATE TABLE soak (id int PRIMARY KEY, body text) WITH (autovacuum_vacuum_scale_factor = 0.02);
	CREATE SEQUENCE soak_ids START $((ROWS + 1));
	INSERT INTO soak SELECT g, concat_ws(' ', 'w' || (g % 2000), 'w' || (g * 7 % 1999), 'w' || (g % 97), 'seed')
	FROM generate_series(1, $ROWS) g;
SQL
if ! err=$(q -c "$INDEX" -c "INSERT INTO soak VALUES (0, 'probe')" -c "DELETE FROM soak WHERE id = 0" 2>&1); then
	[[ $err == *"not implemented"* ]] || { echo "$err" >&2; exit 1; }
	log "SKIP: sift is not ready for the soak yet: ${err#*ERROR:  }"
	exit 0
fi

cat >"$OUT/update.sql" <<-'SQL'
	\set id random(1, :rows)
	\set w random_zipfian(1, 2000, 1.1)
	UPDATE soak SET body = concat_ws(' ', 'w' || :w, 'w' || random(1, 2000), 'w' || (:id % 97), 'upd') WHERE id = :id;
SQL
cat >"$OUT/insert.sql" <<-'SQL'
	\set w random_zipfian(1, 2000, 1.1)
	INSERT INTO soak VALUES (nextval('soak_ids'), concat_ws(' ', 'w' || :w, 'w' || random(1, 2000), 'ins'));
SQL
cat >"$OUT/delete.sql" <<-'SQL'
	\set id random(1, :rows)
	DELETE FROM soak WHERE id = :id;
SQL
cat >"$OUT/read.sql" <<-SQL
	\\set w random_zipfian(1, 2000, 1.1)
	SELECT count(*) FROM soak WHERE $(match "'w' || :w");
	$RANKED
SQL

pgbench -n -T "$DURATION" -R "$RATE" -c "$CLIENTS" -j "$CLIENTS" -D rows="$ROWS" \
	-f "$OUT/update.sql@8" -f "$OUT/insert.sql@1" -f "$OUT/delete.sql@1" >"$OUT/writers.txt" 2>&1 &
writers=$!
pgbench -n -T "$DURATION" -c 1 -f "$OUT/read.sql" >"$OUT/readers.txt" 2>&1 &
readers=$!
trap 'kill $writers $readers 2>/dev/null || true' EXIT

checks=0 bad=0
end=$((SECONDS + DURATION))
while (( SECONDS < end )); do
	sleep "$(( EVERY < end - SECONDS ? EVERY : end - SECONDS ))"
	terms=$(q -c "SELECT string_agg('w' || (random() * 1999)::int, ' ') FROM generate_series(1, 4)")
	for t in $terms; do
		res=$(q <<-SQL
			BEGIN ISOLATION LEVEL REPEATABLE READ;
			SET LOCAL enable_seqscan = off;
			SELECT count(*) FROM soak WHERE $(match "'$t'") \gset idx_
			SET LOCAL enable_seqscan = on;
			SELECT count(*) FROM soak WHERE sift_ref.match(body, '$t') \gset ref_
			COMMIT;
			SELECT :idx_count || ' ' || :ref_count;
		SQL
		) || res="error"
		checks=$((checks + 1))
		read -r a b <<<"$res"
		[[ $a == "${b:-}" ]] || { bad=$((bad + 1)); log "MISMATCH $t: index/reference $res"; }
	done
	q -c "$VERIFY" >/dev/null 2>>"$OUT/soak.log" || { bad=$((bad + 1)); log "VERIFY FAILED"; }
	log "checks $checks, failures $bad"
done
wait $writers $readers || bad=$((bad + 1))
trap - EXIT

failed=$(awk '/^number of failed transactions/ { s += $5; n++ } END { print n ? s : "?" }' "$OUT/writers.txt" "$OUT/readers.txt")
log "writers: $(grep -E '^tps' "$OUT/writers.txt" | head -1); readers: $(grep -E '^tps' "$OUT/readers.txt" | head -1)"
log "writer latency: $(grep -m1 "latency average" "$OUT/writers.txt"); failed transactions: $failed"
[[ $bad == 0 && $failed == 0 ]] || { log "FAIL"; exit 1; }
log "PASS ($checks equivalence checks)"
