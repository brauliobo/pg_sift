#!/usr/bin/env bash
# Regression gate of two or more sift builds that holds up on a busy host: it compares the user-space instructions and cycles that
# the server processes spend per operation, not throughput or latency. Standalone, run directly; not part of make check or scripts/ci.sh.
#
#   bench/steps/cpu_gate.sh [--rows N] [--inserts N] [--queries N] [--reps N] [--max-reps N] [--se PCT] [--budget SECS]
#                           [--cells REGEX] [--raw FILE] [--index-opts LABEL=OPTS] [--guc LABEL=NAME=VALUE] [--env LABEL=NAME=VALUE]
#                           --build LABEL=DIR --build LABEL=DIR ...
#
# Every --build is a checkout with scripts/pgtest.sh (git worktree add --detach DIR REF); the first is the baseline and the others are
# compared with it. Each build gets its own production cluster (SIFT_TMP ${SIFT_GATE_TMP:-/tmp}/sift-gate-LABEL (SIFT_GATE_TMP moves the root), ports SIFT_PORT=56120 and up).
# What is counted: perf stat -e instructions:u,cycles:u on the postmaster of the build, which follows the processes it forks while the
# operation runs, so it sums the backend, the maintenance worker and parallel workers, and leaves out what the host does for others.
# The window of a write ends when the maintenance worker has gone. The cost of a session (connect, load sift, one query on an empty
# index: cell "session", gated as well) is subtracted from every other cell, which is then divided by its operations.
# Every cell starts from the same data: deterministic rows (setseed), the same databases made once per build and copied with
# CREATE DATABASE .. TEMPLATE, and the same statements; the md5 of the heap is compared across builds.
# Cells (write cells run on a fresh copy, read cells on states made once, queries per class from one seeded file):
#   insert1 bulk copy update delete promote merge create    one-row INSERTs, INSERT .. SELECT, COPY, UPDATE of 10%, DELETE of 1/7,
#                                                           sift.promote, 20 sift.merge steps of a 60-segment backlog, CREATE INDEX
#   vacuum_backlog vacuum_dead vacuum_clean vacuum_nofree   VACUUM of the backlog, after deleting 80%, with nothing to do, and the second VACUUM in a row (no free tail left to give back)
#   idle60_C idle1_C vac_C                                  query class C (count rank and or phrase) on 61 segments, on one, and on
#                                                           the index after VACUUM and more rows (what VACUUM leaves differs by design)
# Per cell and round the builds run one after the other, the first of the round rotating (ABAB, BABA); the ratio of a build to the
# baseline is the geometric mean of the per-round ratios of instructions per operation, with a 95% interval (t distribution). Rounds
# run --reps times (default 7), then only the cells whose standard error of the ratio is still above --se percent (default 1) get
# more, up to --max-reps (25) or until --budget seconds (default 3600) have passed. A cell regresses when its whole interval is above
# 1.02; "design" marks a cell whose end state differs between builds (segments left, docs not promoted): its ratio is work done by
# design, reported apart. Also printed: cycles ratio (secondary), WAL bytes, index size, segments.
# --index-opts and --guc give one build a reloption list for the indexes of its cells (vacuum_truncate = false) or a setting of its cluster
# (sift.maintenance_cost_delay=0); the same DIR under two labels compares two configurations of one build. --env LABEL=NAME=VALUE sets a variable of that build's
# scripts/pgtest.sh (SIFT_HARDEN=1: the same DIR under two labels compares compiler flag sets, docs/SAFETY.md).
# Never connects to a server it did not start. Clusters and scratch directories are removed at the end.
set -Eeuo pipefail
trap 'echo "cpu-gate: failed at line $LINENO: $BASH_COMMAND" >&2' ERR
export LC_ALL=C
. "$(dirname "$0")/../../scripts/quiet.sh"   # the pressure is printed for context; instruction counts do not wait for a quiet host

ROWS=20000 INS=10000 NQ=100 REPS=7 MAXREPS=25 SE=1 BUDGET=3600 CELLS=. RAW= BUILDS=()
declare -A DIR TMP PORT PM IOPTS GUCS ENVS
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--inserts) INS=$2; shift ;;
		--queries) NQ=$2; shift ;;
		--reps) REPS=$2; shift ;;
		--max-reps) MAXREPS=$2; shift ;;
		--se) SE=$2; shift ;;
		--budget) BUDGET=$2; shift ;;
		--cells) CELLS=$2; shift ;;
		--raw) RAW=$2; shift ;;
		--build) BUILDS+=("$2"); shift ;;
		--index-opts) IOPTS[${2%%=*}]=${2#*=}; shift ;;
		--guc) GUCS[${2%%=*}]+="${2#*=}"$'\n'; shift ;;
		--env) ENVS[${2%%=*}]+="${2#*=} "; shift ;;
		*) sed -n '2,28p' "$0"; exit 1 ;;
	esac
	shift
done
(( ${#BUILDS[@]} >= 2 )) || { sed -n '2,28p' "$0"; exit 1; }
BASE=$(mktemp -d ${SIFT_GATE_TMP:-/tmp}/sift-gate-run.XXXXXX)
PORT0=${SIFT_PORT:-56120}
START=$SECONDS
LABELS=()

pg() { # LABEL pgtest-args...
	local l=$1; shift
	# shellcheck disable=SC2086  # ENVS holds NAME=VALUE words
	( cd "${DIR[$l]}" && env SIFT_TMP=${TMP[$l]} SIFT_PORT=${PORT[$l]} SIFT_TESTING= ${ENVS[$l]:-} scripts/pgtest.sh "$@" )
}
Q() { # LABEL DATABASE psql-args...
	local l=$1 db=$2; shift 2
	psql -X -q -At -v ON_ERROR_STOP=1 -h "${TMP[$l]}/sock" -p "${PORT[$l]}" -U postgres -d "$db" "$@"
}
Qn() { Q "$@" >/dev/null; }
create_index() { echo "CREATE INDEX $1 ON $2 USING sift (body)${IOPTS[$3]:+ WITH (${IOPTS[$3]})}"; } # NAME TABLE LABEL
cleanup() {
	[[ -z ${PERFPID:-} ]] || kill "$PERFPID" 2>/dev/null || true
	for l in "${LABELS[@]}"; do pg "$l" destroy >/dev/null 2>&1 || true; done
	rm -rf "$BASE"
}
trap cleanup EXIT

# a maintenance worker of the cluster of LABEL is a child of its postmaster ("sift maintenance (index N)"); the check opens no session
worker_up() { ps -o args= --ppid "${PM[$1]}" | awk '/sift maintenance/ {f = 1} END {exit !f}'; }
settle() { # LABEL: until no worker has been seen for two checks
	local n=0 t=$SECONDS
	while (( n < 2 )); do
		sleep 0.25
		if worker_up "$1"; then n=0; else n=$((n + 1)); fi
		(( SECONDS - t < 600 )) || { echo "cpu-gate: the maintenance worker of $1 does not finish" >&2; exit 1; }
	done
}
ack() { local x='' i; for i in 1 2 3 4 5; do read -r -t 30 -u "$AF" x || true; [[ $x == *ack* ]] && return 0; done; echo "cpu-gate: perf did not acknowledge" >&2; exit 1; }

# window LABEL cmd...: runs cmd (and, with WSETTLE=1, waits for the worker to go) while perf counts the processes the postmaster forks;
# sets W_INS and W_CYC
window() {
	local l=$1 line
	shift
	rm -f "$BASE/ctl" "$BASE/ack"; mkfifo "$BASE/ctl" "$BASE/ack"
	exec {CF}<>"$BASE/ctl" {AF}<>"$BASE/ack"
	perf stat -D -1 --control "fifo:$BASE/ctl,$BASE/ack" -x, -e instructions:u,cycles:u -p "${PM[$l]}" -o "$BASE/perf.out" >"$BASE/perf.log" 2>&1 &
	PERFPID=$!
	until grep -q 'Events disabled' "$BASE/perf.log"; do kill -0 "$PERFPID" 2>/dev/null || { cat "$BASE/perf.log" >&2; exit 1; }; sleep 0.02; done
	echo enable >&"$CF"; ack
	"$@"
	[[ ${WSETTLE:-1} == 0 ]] || settle "$l"
	echo disable >&"$CF"; ack
	kill -INT "$PERFPID"; wait "$PERFPID" || true
	PERFPID=
	exec {CF}>&- {AF}>&-
	W_INS=$(awk -F, '/instructions/ {print $1}' "$BASE/perf.out")
	W_CYC=$(awk -F, '/cycles/ {print $1}' "$BASE/perf.out")
	[[ $W_INS =~ ^[0-9]+$ && $W_CYC =~ ^[0-9]+$ ]] || { echo "cpu-gate: perf counted nothing: $(<"$BASE/perf.out")" >&2; exit 1; }
}

ROUND=0
rec() { printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$ROUND" "$3" "$4" >>"$BASE/all.tsv"; } # CELL LABEL METRIC VALUE

# measure CELL LABEL DB INDEX cmd...: the window around cmd, then WAL, size and segments of INDEX; OPS is the divisor of the cell
measure() {
	local c=$1 l=$2 db=$3 idx=$4 lsn0 lsn1 kb segs mut
	shift 4
	lsn0=$(Q "$l" "$db" -c "SELECT pg_current_wal_lsn()")
	window "$l" "$@"
	lsn1=$(Q "$l" "$db" -c "SELECT pg_current_wal_lsn()")
	read -r kb segs mut < <(Q "$l" "$db" -F ' ' -c "SELECT pg_relation_size('$idx') / 1024, count(*) FILTER (WHERE kind = 'immutable'),
		coalesce(sum(ndocs) FILTER (WHERE kind <> 'immutable'), 0) FROM sift.segments('$idx')")
	rec "$c" "$l" instr "$W_INS"; rec "$c" "$l" cyc "$W_CYC"; rec "$c" "$l" ops "$OPS"
	rec "$c" "$l" wal_kb "$(Q "$l" "$db" -c "SELECT (pg_wal_lsn_diff('$lsn1', '$lsn0') / 1024)::bigint")"
	rec "$c" "$l" idx_kb "$kb"; rec "$c" "$l" segs "$segs"; rec "$c" "$l" mutdocs "$mut"
}

# N random words (a skewed vocabulary); a column of the outer query makes it a new value for every row
words() { echo "(SELECT string_agg('w' || floor(power(random(), 3) * 50000)::int, ' ') FROM generate_series(1, $1) w${2:+ WHERE $2 IS NOT NULL})"; }
newdb() { settle "$1"; Qn "$1" postgres -c "CREATE DATABASE $2 TEMPLATE $3"; } # LABEL NAME TEMPLATE
dropdb() { settle "$1"; Qn "$1" postgres -c "DROP DATABASE $2 WITH (FORCE)"; }

# the databases of a build, each made the same way on every build: write cells copy tpl_empty, tpl_data (one merged segment) or tpl_bk
# (60 segments)
templates() { # LABEL
	local l=$1
	Qn "$l" postgres -c "CREATE DATABASE tpl_empty"
	Qn "$l" tpl_empty -c "CREATE EXTENSION pg_sift" -c "CREATE TABLE t (id serial, body text)" -c "$(create_index t_idx t "$l")" \
		-c "SELECT setseed(0.42)" -c "CREATE TABLE stage AS SELECT i AS id, $(words 40 i) AS body FROM generate_series(1, $ROWS) i"
	newdb "$l" tpl_data tpl_empty
	Qn "$l" tpl_data -c "SET sift.async_maintenance = off" -c "INSERT INTO t (body) SELECT body FROM stage ORDER BY id" \
		-c "SELECT sift.promote('t_idx')" -c "SELECT sift.merge('t_idx', true)"
	newdb "$l" tpl_bk tpl_empty
	Qn "$l" tpl_bk -c "SELECT setseed(0.43)" -c "SET sift.async_maintenance = off" -c "DO \$\$ BEGIN FOR k IN 1..60 LOOP
		INSERT INTO t (body) SELECT $(words 6 i) FROM generate_series(1, 600) i; PERFORM sift.promote('t_idx'); COMMIT; END LOOP; END \$\$"
}

# the states of the read cells: 61 segments, one segment, and the backlog after VACUUM, more rows and another VACUUM
read_states() { # LABEL
	local l=$1 db f
	newdb "$l" r60 tpl_bk
	Qn "$l" r60 -c "SET sift.async_maintenance = off" -c "INSERT INTO t (body) SELECT body FROM stage WHERE id <= 1000" -c "SELECT sift.promote('t_idx')"
	newdb "$l" r1 r60
	Qn "$l" r1 -c "SELECT sift.merge('t_idx', true)"
	newdb "$l" rvac tpl_bk
	Qn "$l" rvac -c "VACUUM t"; settle "$l"
	Qn "$l" rvac -c "INSERT INTO t (body) SELECT body FROM stage WHERE id <= 5000" -c "VACUUM t"; settle "$l"
	for db in r60 r1 rvac; do for f in "$BASE"/q_*.sql; do Qn "$l" $db -f "$f"; done; done
}

# the statements, from the baseline's stage table (the same rows on every build)
statements() { # LABEL
	local l=$1
	Q "$l" tpl_empty -c "SELECT body FROM stage WHERE id <= $INS ORDER BY id" | awk '{gsub(/\047/, "\047\047"); print "INSERT INTO t (body) VALUES (\047" $0 "\047);"}' >"$BASE/insert.sql"
	Qn "$l" tpl_empty -c "COPY (SELECT body FROM stage ORDER BY id) TO '$BASE/stage.csv' CSV"
	local rnd="floor(power(random(), 3) * 50000)::int" cls
	declare -A gen=(
		[count]="format('SELECT count(*) FROM t WHERE body @@> %L;', 'w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd)"
		[rank]="format('SELECT id FROM t WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10;', 'w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd)"
		[and]="format('SELECT id FROM t WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10;', 'w' || $rnd || ' AND w' || $rnd)"
		[or]="format('SELECT id FROM t WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10;', 'w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd || ' OR w' || $rnd)"
	)
	for cls in count rank and or; do
		Q "$l" tpl_empty -c "SELECT setseed(0.7)" -c "SELECT ${gen[$cls]} FROM generate_series(1, $NQ)" | grep -v '^$' >"$BASE/q_$cls.sql"
	done
	Q "$l" tpl_empty -c "SELECT format('SELECT id FROM t WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT 10;',
		'\"' || (regexp_match(body, 'w[0-9]+ w[0-9]+'))[1] || '\"') FROM stage WHERE id <= $NQ ORDER BY id" >"$BASE/q_phrase.sql"
}

# one cell on one build, in the current ROUND
run_cell() { # CELL LABEL
	local c=$1 l=$2 st cls db=w_$1
	OPS=1 WSETTLE=1
	case $c in
		session) measure "$c" "$l" tpl_empty t_idx Qn "$l" tpl_empty -c "SELECT count(*) FROM t WHERE body @@> 'w1'" ;;
		idle60_* | idle1_* | vac_*)
			st=${c%%_*} cls=${c#*_} OPS=$NQ WSETTLE=0
			case $st in idle60) db=r60 ;; idle1) db=r1 ;; vac) db=rvac ;; esac
			measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -f "$BASE/q_$cls.sql" ;;
		*)
			case $c in
				insert1 | bulk | copy | promote | create) newdb "$l" "$db" tpl_empty ;;
				update | delete | vacuum_dead | vacuum_clean | vacuum_nofree) newdb "$l" "$db" tpl_data ;;
				*) newdb "$l" "$db" tpl_bk ;;
			esac
			case $c in
				insert1) OPS=$INS measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -f "$BASE/insert.sql" ;;
				bulk) OPS=$ROWS measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "INSERT INTO t (body) SELECT body FROM stage" ;;
				copy) OPS=$ROWS measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "COPY t (body) FROM '$BASE/stage.csv' CSV" ;;
				update) OPS=$((ROWS / 10)) measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "UPDATE t SET body = body || ' w1 w2' WHERE id % 10 = 0" ;;
				delete) OPS=$((ROWS / 7)) measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "DELETE FROM t WHERE id % 7 = 0" ;;
				promote)
					Qn "$l" "$db" -c "SET sift.async_maintenance = off" -c "INSERT INTO t (body) SELECT body FROM stage WHERE id <= 3000"
					measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "SELECT sift.promote('t_idx')" ;;
				merge) measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "SELECT sift.merge('t_idx') FROM generate_series(1, 20)" ;;
				create) measure "$c" "$l" "$db" s_idx Qn "$l" "$db" -c "$(create_index s_idx stage "$l")" ;;
				vacuum_nofree)
					Qn "$l" "$db" -c "VACUUM t"; settle "$l"
					measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "VACUUM t" ;;
				vacuum_backlog | vacuum_clean) measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "VACUUM t" ;;
				vacuum_dead)
					Qn "$l" "$db" -c "DELETE FROM t WHERE id % 5 <> 0"
					measure "$c" "$l" "$db" t_idx Qn "$l" "$db" -c "VACUUM t" ;;
			esac
			dropdb "$l" "$db" ;;
	esac
}

ALLCELLS=(session insert1 bulk copy update delete promote merge create vacuum_backlog vacuum_dead vacuum_clean vacuum_nofree)
for st in idle60 idle1 vac; do for cls in count rank and or phrase; do ALLCELLS+=("${st}_$cls"); done; done

# round WANTED...: the cells of the list that --cells selects, builds in an order that rotates with the round
round() {
	local c l k n=${#LABELS[@]}
	ROUND=$((ROUND + 1))
	for l in "${LABELS[@]}"; do settle "$l"; done
	for c in "${ALLCELLS[@]}"; do
		[[ $c == session || ( $c =~ $CELLS && " $* " == *" $c "* ) ]] || continue
		for ((k = 0; k < n; k++)); do run_cell "$c" "${LABELS[(k + ROUND) % n]}"; done
	done
}

# stats table|pending: the ratios of every build to the first from $BASE/all.tsv
stats() {
	awk -F'\t' -v mode="$1" -v se_target="$SE" -v maxreps="$MAXREPS" -v labels="${LABELS[*]}" '
	BEGIN {
		nb = split(labels, B, " ")
		split("12.706 4.303 3.182 2.776 2.571 2.447 2.365 2.306 2.262 2.228 2.201 2.179 2.160 2.145 2.131 2.120 2.110 2.101 2.093 2.086 2.080 2.074 2.069 2.064 2.060 2.056 2.052 2.048 2.045 2.042", T, " ")
	}
	{ if (!($1 in seen)) { seen[$1] = 1; cells[++nc] = $1 } v[$1, $2, $3, $4] = $5; has[$1, $3] = 1 }
	function human(x) { return x >= 1e9 ? sprintf("%.2fG", x / 1e9) : x >= 1e6 ? sprintf("%.2fM", x / 1e6) : x >= 1e3 ? sprintf("%.1fk", x / 1e3) : sprintf("%.0f", x) }
	function net(c, b, r, m,   x) {
		x = v[c, b, r, m]; if (x == "") return -1
		if (c != "session" && c != "vacuum_nofree") { if (v["session", b, r, m] == "") return -1; x -= v["session", b, r, m] }
		return x / v[c, b, r, "ops"]
	}
	# ratio of build b to the baseline over the rounds of cell c in metric m: sets n, ratio, se (of the ratio, in %), lo, hi
	function ratio(c, b, m,   r, a, z, s, q, tq, mu, sd) {
		n = 0; s = 0; q = 0
		for (r = 1; r <= maxround; r++) {
			if (!has[c, r]) continue
			a = net(c, B[1], r, m); z = net(c, b, r, m)
			if (a <= 0 || z <= 0) continue
			a = log(z / a); n++; s += a; q += a * a
		}
		if (n < 2) { rt = n ? exp(s) : 0; se = 100; lo = 0; hi = 1e9; return }
		mu = s / n; sd = sqrt((q - n * mu * mu) / (n - 1)); tq = (n - 1 <= 30) ? T[n - 1] : 1.96
		rt = exp(mu); se = rt * sd / sqrt(n) * 100; lo = exp(mu - tq * sd / sqrt(n)); hi = exp(mu + tq * sd / sqrt(n))
	}
	function mnet(c, b,   r, s, k, x) { s = 0; k = 0; for (r = 1; r <= maxround; r++) if (has[c, r] && (x = net(c, b, r, "instr")) >= 0) { s += x; k++ } return k ? s / k : 0 }
	function last(c, b, m,   r, x) { for (r = 1; r <= maxround; r++) if (has[c, r] && v[c, b, r, m] != "") x = v[c, b, r, m]; return x }
	function mean(c, b, m,   r, s, k) { s = 0; k = 0; for (r = 1; r <= maxround; r++) if (has[c, r] && v[c, b, r, m] != "") { s += v[c, b, r, m]; k++ } return k ? s / k : 0 }
	END {
		for (k in has) { split(k, p, SUBSEP); if (p[2] + 0 > maxround) maxround = p[2] + 0 }
		if (mode == "pending") {
			for (i = 1; i <= nc; i++) for (j = 2; j <= nb; j++) {
				ratio(cells[i], B[j], "instr")
				if (n < maxreps && se >= se_target) { printf "%s ", cells[i]; break }
			}
			exit
		}
		printf "%-16s %10s", "cell", "instr/op"
		for (j = 2; j <= nb; j++) printf " | %-7s %6s %-15s %5s %3s %6s %s", B[j], "ratio", "95% interval", "se%", "n", "cyc", "verdict"
		printf "\n"
		for (i = 1; i <= nc; i++) {
			c = cells[i]; printf "%-16s %10s", c, human(mnet(c, B[1]))
			for (j = 2; j <= nb; j++) {
				b = B[j]; ratio(c, b, "instr"); r0 = rt; s0 = se; l0 = lo; h0 = hi; n0 = n; ratio(c, b, "cyc")
				dz = (last(c, b, "segs") != last(c, B[1], "segs")) || ((last(c, b, "mutdocs") > 0) != (last(c, B[1], "mutdocs") > 0))
				verdict = l0 > 1.02 ? "REGRESS" : h0 <= 0.98 ? "faster" : h0 <= 1.02 ? "ok" : "uncertain"
				if (dz) { verdict = verdict " (design)"; nd++; dn[nd] = c " " b " " sprintf("%.3f", r0) }
				else if (verdict == "REGRESS") { nr++; rn[nr] = c " " b " " sprintf("%.3f [%.3f, %.3f]", r0, l0, h0) }
				else if (verdict == "uncertain") { nu++; un[nu] = c " " b " " sprintf("%.3f [%.3f, %.3f]", r0, l0, h0) }
				printf " | %-7s %6.3f [%5.3f,%6.3f] %5.2f %3d %6.3f %s", b, r0, l0, (h0 > 99 ? 99 : h0), s0, n0, rt, verdict
			}
			printf "\n"
		}
		printf "\nstate and cost of a run of the cell, baseline -> build (WAL and index in kB, mean of the rounds; segments and documents not promoted, last round)\n"
		printf "%-16s", "cell"
		for (j = 2; j <= nb; j++) printf " | %-7s %15s %15s %9s %11s", B[j], "wal kB", "index kB", "segs", "mutdocs"
		printf "\n"
		for (i = 1; i <= nc; i++) {
			c = cells[i]; printf "%-16s", c
			for (j = 2; j <= nb; j++) {
				b = B[j]
				printf " | %-7s %7.0f->%-7.0f %7.0f->%-7.0f %4d->%-4d %5d->%-5d", b, mean(c, B[1], "wal_kb"), mean(c, b, "wal_kb"), mean(c, B[1], "idx_kb"), mean(c, b, "idx_kb"),
					last(c, B[1], "segs"), last(c, b, "segs"), last(c, B[1], "mutdocs"), last(c, b, "mutdocs")
			}
			printf "\n"
		}
		printf "\nregressions (interval entirely above 1.02, end state equal): %d\n", nr
		for (i = 1; i <= nr; i++) print "  " rn[i]
		printf "uncertain (interval straddles 1.02): %d\n", nu
		for (i = 1; i <= nu; i++) print "  " un[i]
		printf "design (end state differs, ratio is work by design): %d\n", nd
		for (i = 1; i <= nd; i++) print "  " dn[i]
	}' "$BASE/all.tsv"
}

command -v perf >/dev/null || { echo "cpu-gate: perf is required" >&2; exit 1; }
i=0
for b in "${BUILDS[@]}"; do
	l=${b%%=*}
	LABELS+=("$l")
	DIR[$l]=$(cd "${b#*=}" && pwd) TMP[$l]=${SIFT_GATE_TMP:-/tmp}/sift-gate-$l PORT[$l]=$((PORT0 + i++))
	pg "$l" install >/dev/null
	pg "$l" start >/dev/null
	PM[$l]=$(head -1 "${TMP[$l]}/data/postmaster.pid")
	Qn "$l" postgres -c "ALTER SYSTEM SET autovacuum = off" -c "SELECT pg_reload_conf()"
	while IFS== read -r name val; do
		[[ -z $name ]] || Qn "$l" postgres -c "ALTER SYSTEM SET $name = '$val'" -c "SELECT pg_reload_conf()"
	done <<<"${GUCS[$l]:-}"
done
: >"$BASE/all.tsv"
for l in "${LABELS[@]}"; do templates "$l"; done
statements "${LABELS[0]}"
for l in "${LABELS[@]}"; do read_states "$l"; done
for db in tpl_data r60; do
	sums=$(for l in "${LABELS[@]}"; do Q "$l" $db -c "SELECT md5(string_agg(body, ',' ORDER BY id)) FROM t"; done | sort -u | wc -l)
	[[ $sums == 1 ]] || { echo "cpu-gate: the rows of $db differ between builds" >&2; exit 1; }
done
echo "$(quiet_report 2>&1), $ROWS rows, $INS inserts, $NQ queries per class, builds ${LABELS[*]}"

WANT=" ${ALLCELLS[*]} "
for ((r = 1; r <= REPS; r++)); do round $WANT; done
while (( ROUND < MAXREPS && SECONDS - START < BUDGET )); do
	pend=$(stats pending)
	[[ -n $pend ]] || break
	echo "round $((ROUND + 1)): $pend" >&2
	round $pend
done
[[ -z $RAW ]] || cp "$BASE/all.tsv" "$RAW"
stats table
left=$(stats pending)
echo
echo "rounds $ROUND (cells still above ${SE}% standard error: ${left:-none}), $((SECONDS - START)) s, $(quiet_report 2>&1) now"
