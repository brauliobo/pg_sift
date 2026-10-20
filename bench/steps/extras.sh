#!/usr/bin/env bash
# Usage: bench/bench extras ITEM... [--docs] [--out DIR]     ITEM: rollup tablewrites writepath bm25size leaftypes fields buildcost gpu all   (steps/extras.sh)
# The measurements behind the 3.1 and 3.2 rows of docs/BENCHMARKS.md that the search and store passes do not make. Each item measures the builds it compares on a
# scratch cluster of its own (steps/scratch.sh: systemd scope, never an existing cluster), holds /tmp/sift-perf.lock (blocking) for its timed part, writes
# bench/results/<name>.tsv (steps/extras.py: one row per number, with the load average while it ran) and, with --docs, the generated blocks.
#
#   rollup       element rollups: j02 and j17 as the normal plan, the scan of every unit and the answer from the rollups, 100k / 1M / 10M documents; index size and
#                CREATE INDEX cost with and without the rollups (bench/sql/jsonb/rollup/run.sql). 10M needs about 25 GB: SCRATCH_MEM=30G, ROLLUP_SIZES to choose
#   tablewrites  sift_table against the heap, the same orders loaded, queried, updated, deleted and vacuumed (bench/tbl.sh), a COPY of 5M rows in one
#                transaction (tbl-bigcopy.sh) and a stress run (tbl-stress.sh), on 3.0.2 and 3.1.0
#   writepath    a loop of single-row INSERTs in one transaction, through psql, with savepoints and per access method (tbl-writepath.sh), 3.1.0
#   bm25size     the BM25 index size, default and positions = off, on 1M titles and 50,000 articles, 3.0.2 against 3.1.0
#   leaftypes    numeric(30,6), numeric, money and interval leaves against the narrow path: index size, CREATE INDEX, two filters (3.1.0 narrow, main wide)
#   fields       sift.facet_counts against four GROUP BYs, and field weights (sift.field_query) against one field, on the 1M orders
#   buildcost    CREATE INDEX and write cost in user instructions per operation, 3.0.2 against 3.1.0 against main (steps/cpu_gate.sh)
#   gpu          the Vulkan aggregate path against the CPU on the store table (bench/gpu/bench-pg.sh), needs a device
#
# Builds: a label is a checkout (git worktree of a tag, or this one) and its staged install; `build_prepare` makes what is missing under $EXTRAS_SRC and
# $EXTRAS_CACHE: v302 (tag v3.0.2), v310 (v3.1.0), v310-vk (v3.1.0 with VULKAN=1), main (HEAD of this checkout). Environment: EXTRAS_SRC, EXTRAS_CACHE, SCRATCH_MEM,
# ROLLUP_SIZES, TBL_ROWS, WP_ROWS, BM25_ARTICLES (50000), BM25_TITLES (1000000), LT_ROWS (1000000), ORDERS (1000000).
set -euo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(dirname "$HERE")
REPO=$(dirname "$BENCH")
. "$HERE/scratch.sh"
STORE=${BENCH_STORE:-${BENCH_WORK:-/tmp/sift-bench}}
. "$HERE/builds.sh"

ORIG=("$@")
ITEMS=() DOCS='' OUT=''
while (( $# )); do
	case $1 in
		--docs) DOCS=1 ;;
		--out) OUT=$2; shift ;;
		-*) sed -n '2,3p' "$0"; exit 1 ;;
		*) ITEMS+=("$1") ;;
	esac
	shift
done
[[ ${ITEMS[*]:-} != all ]] || ITEMS=(rollup tablewrites writepath bm25size leaftypes fields buildcost gpu)
(( ${#ITEMS[@]} )) || { sed -n '2,3p' "$0"; exit 1; }
scratch_scope "${ORIG[@]}"
OUT=${OUT:-$BENCH/out/extras-$(date -u +%Y%m%dT%H%M%S)}
mkdir -p "$OUT"
cleanup() { [[ -z ${PG:-} ]] || scratch_down; }
trap cleanup EXIT
export PERF_LOCK=${PERF_LOCK:-/tmp/sift-perf.lock}   # PERF_LOCK=/tmp/other.lock: a smoke test of the script, never a number to keep
. "$REPO/scripts/quiet.sh"
locked() { # one timed run at a time on the host, started when the pressure of the host is low (else after SIFT_QUIET_TIMEOUT s, marked NOISY in $OUT/noisy.txt)
	( flock 9
	  quiet_batch --max "${SIFT_QUIET_MAX:-15}" --timeout "${SIFT_QUIET_TIMEOUT:-300}" >&2
	  [[ -z $QUIET_MARK ]] || echo "${ITEM:-?}: $* ($(quiet_report 2>&1))" >>"$OUT/noisy.txt"
	  "$@" ) 9>>"$PERF_LOCK"
}

# ---- builds -------------------------------------------------------------------------------------------------------------------------------
build_up() { # ITEM LABEL: a scratch cluster of the build; VERSION, BUILD_LABEL
	build_prepare "$2"
	export SIFT_BUILD=$(build_dir "$2") SIFT_ROOT=$EXTRAS_CACHE/$2/root SCRATCH_ROOT=${SCRATCH_ROOT:-$HOME/.cache/sift-bench-scratch}
	[[ -z ${PG:-} ]] || scratch_down
	unset KEEP
	rm -rf "$SCRATCH_ROOT/$1-$2"
	scratch_up "$1-$2"
	VERSION=$(git -C "$SIFT_BUILD" describe --tags --always)
	BUILD_LABEL=$2
	sq -d postgres -c "SELECT 1" >/dev/null
}
build_psql() { psql -X -q -v ON_ERROR_STOP=1 -U postgres "$@"; }

# ---- results ------------------------------------------------------------------------------------------------------------------------------
ROWS_FILE=''
begin_item() { ROWS_FILE=$OUT/$1.rows; : >"$ROWS_FILE"; LOAD0=$(python3 -I -c "import sys; sys.path.insert(0, '$HERE'); import extras; print(extras.host_load())"); }
emit() { # CASE ARM METRIC VALUE [NOTE]   (MIN, MAX, N from the environment)
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${VERSION:-?}" "$1" "$2" "$3" "$4" "${MIN:-}" "${MAX:-}" "${N:-}" "$(cut -d' ' -f1 /proc/loadavg)" "${5:-}" >>"$ROWS_FILE"
}
emit_rows() { # parse `ROW case arm metric value note` lines (psql, tab separated) from stdin into rows, ARM_PREFIX before the arm
	local tag cs arm metric value note
	while IFS=$'\t' read -r tag cs arm metric value note; do
		[[ $tag == ROW ]] || continue
		emit "$cs" "${ARM_PREFIX:-}$arm" "$metric" "$value" "${note:-}"
	done
}
finish_item() { # NAME TITLE [KEY=VALUE ...]
	local name=$1 title=$2; shift 2
	python3 -I "$HERE/extras.py" wrap --name "$name" --title "$title" --rows "$ROWS_FILE" --out "$OUT/$name.tsv" --load-before "$LOAD0" \
		--meta "date=$(date -u +%Y-%m-%d)" \
		--meta "cpu=$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); n=$2} /^CPU\(s\)/ {gsub(/ /, "", $2); t=$2} END {printf "%s (%s threads)", n, t}')" \
		--meta "noisy_starts=$(cat "$OUT/noisy.txt" 2>/dev/null | wc -l) of the timed runs started while the pressure was above ${SIFT_QUIET_MAX:-15}% for ${SIFT_QUIET_TIMEOUT:-300} s" \
		--meta "postgresql=PostgreSQL $(pg_config --version | awk '{print $2}')" --meta "scope=$(cat /proc/self/cgroup | sed 's,.*/,,' | head -1)" "$@"
	echo "extras: $OUT/$name.tsv" >&2
	[[ -z $DOCS ]] || cp "$OUT/$name.tsv" "$REPO/bench/results/$name.tsv"
}
size_label() { case $1 in 100000) echo 100k ;; 1000000) echo 1M ;; 10000000) echo 10M ;; *) echo "$1" ;; esac; }

# ---- items --------------------------------------------------------------------------------------------------------------------------------
item_rollup() {
	begin_item rollup
	build_up rollup v310
	local n variant opts builds out k line
	for n in ${ROLLUP_SIZES:-100000 1000000 10000000}; do
		k=$(size_label "$n")
		echo "rollup: $n documents" >&2
		build_psql -d postgres -c "DROP DATABASE IF EXISTS jb" -c "CREATE DATABASE jb" >/dev/null
		build_psql -d jb -c "CREATE EXTENSION pg_sift" >/dev/null
		build_psql -d jb -v n="$n" -v strs=0 -f "$BENCH/sql/jsonb/gen.sql" >/dev/null
		builds=3; (( n < 10000000 )) || builds=1
		for variant in default off; do
			opts=''; [[ $variant == default ]] || opts=", rollup = 'off'"
			out=$(locked build_psql -At -d jb -v n="$n" -v variant="$variant" -v builds="$builds" -v opts="$opts" -f "$BENCH/sql/jsonb/rollup/run.sql")
			while IFS= read -r line; do
				case $line in
					'build serial cpu ms: min '*) N=$builds MIN=$(awk '{print $6}' <<<"$line") emit "build@$k" "$variant" cpu_ms "$(awk '{print $8}' <<<"$line")" ;;
					'build serial ms: min '*) N=$builds MIN=$(awk '{print $5}' <<<"$line") emit "build@$k" "$variant" wall_ms "$(awk '{print $7}' <<<"$line")" ;;
					'build 2 workers ms: min '*) N=$builds MIN=$(awk '{print $6}' <<<"$line") emit "build@$k" "$variant" wall_ms_2_workers "$(awk '{print $8}' <<<"$line")" ;;
					'index bytes: '*) emit "index@$k" "$variant" bytes "${line#index bytes: }" ;;
					j??*' normal/scanned/pre ms: '*)
						local q=${line%% *} a b c
						read -r a b c <<<"$(sed 's/.*ms: //; s/ \/ / /g' <<<"$line")"
						emit "$q@$k" normal ms "$a" "rollup=$variant"; emit "$q@$k" scanned ms "$b" "rollup=$variant"; emit "$q@$k" rollup ms "$c" "rollup=$variant" ;;
				esac
			done <<<"$out"
		done
		build_psql -d postgres -c "DROP DATABASE jb" >/dev/null
	done
	finish_item rollup "Element rollups of jsonb arrays: j02 and j17 as the normal plan, the scan of every unit and the answer from the rollups (bench/sql/jsonb/rollup/run.sql)." \
		--meta "build=$VERSION" --meta "method=server-side median ms of 21 runs (normal plan: 7), one backend, no parallel workers, seq_page_cost 10000, rollup worker off; index and build rows with the default rollups and with rollup = 'off'"
}

item_tablewrites() {
	begin_item tablewrites
	local l out
	for l in v302 v310; do
		build_up tablewrites "$l"
		echo "tablewrites: $VERSION tbl.sh" >&2
		out=$(locked env SIFT_TMP="$SIFT_TMP" SIFT_PORT="$SIFT_PORT" "$BENCH/tbl.sh" "${TBL_ROWS:-1000000}")
		local line
		while IFS= read -r line; do
			if [[ $line =~ ^(.{34})\ heap\ +([0-9]+)\ ms\ +sift_table\ +([0-9]+)\ ms ]]; then
				local name=${BASH_REMATCH[1]}; name=${name%% }
				emit "$(sed 's/ *$//' <<<"$name")" heap ms "${BASH_REMATCH[2]}"; emit "$(sed 's/ *$//' <<<"$name")" sift_table ms "${BASH_REMATCH[3]}"
			elif [[ $line =~ ^load\ rate.*heap\ +([0-9]+)\ rows/s\ +sift_table\ +([0-9]+)\ rows/s ]]; then
				emit "load rate" heap rows_per_s "${BASH_REMATCH[1]}"; emit "load rate" sift_table rows_per_s "${BASH_REMATCH[2]}"
			elif [[ $line =~ ^(heap|sift_table)\ +([0-9]+)\ /\ ([0-9]+)$ ]]; then
				emit "size" "${BASH_REMATCH[1]}" table_mb "${BASH_REMATCH[2]}"; emit "size" "${BASH_REMATCH[1]}" indexes_mb "${BASH_REMATCH[3]}"
			fi
		done <<<"$out"
		echo "tablewrites: $VERSION bigcopy" >&2
		local rc=2
		while (( rc == 2 )); do
			out=$("$BENCH/tbl-bigcopy.sh" "${BIGCOPY_ROWS:-5000000}" 2>&1) && rc=0 || rc=$?
			(( rc != 2 )) || sleep 20
		done
		echo "$out" | sed 's/^/  /' >&2
		[[ $out =~ one\ transaction,\ ([0-9]+)\ rows:\ ([0-9.]+)\ s,\ backend\ peak\ memory\ ([0-9]+)\ MB ]] && {
			emit "COPY ${BASH_REMATCH[1]} rows, one transaction" sift_table seconds "${BASH_REMATCH[2]}"; emit "COPY ${BASH_REMATCH[1]} rows, one transaction" sift_table peak_mb "${BASH_REMATCH[3]}"; }
		[[ $out =~ VACUUM:\ ([0-9.]+)\ s ]] && emit "VACUUM after the COPY" sift_table seconds "${BASH_REMATCH[1]}"
		emit "COPY, one transaction: checks" sift_table "failed_checks" "$(grep -c '^FAIL' <<<"$out" || true)"
		if [[ $l == v310 ]]; then
			echo "tablewrites: $VERSION stress" >&2
			out=$(locked "$BENCH/tbl-stress.sh" "${STRESS_SECONDS:-120}" 2>&1) || true
			echo "$out" | tail -15 | sed 's/^/  /' >&2
			local st="stress ${STRESS_SECONDS:-120} s, ${TBL_CLIENTS:-16} clients"
			[[ $out =~ round\ of\ 60\ s:\ ([0-9\ ]+) ]] && emit "$st" sift_table tps_mean "$(awk '{s += $1} END {printf "%d", s / NF}' <<<"${BASH_REMATCH[1]}")"
			emit "$st" sift_table passed "$(grep -c '^PASS' <<<"$out" || true)"
		fi
	done
	finish_item tablewrites "sift_table against the heap on orders: load, queries, UPDATE, DELETE, VACUUM, a COPY in one transaction, a stress run (bench/tbl.sh, tbl-bigcopy.sh, tbl-stress.sh)." \
		--meta "method=bench/tbl.sh ${TBL_ROWS:-1000000} rows (100,000 rows per load transaction), best of 3 for reads, writes once; scratch cluster, shared_buffers 2GB, one client"
}

item_writepath() {
	begin_item writepath
	build_prepare v310
	local dir tmp="$EXTRAS_CACHE/v310" out
	dir=$(build_dir v310)
	[[ -z ${PG:-} ]] || scratch_down
	echo "writepath: tbl-writepath.sh ${WP_ROWS:-10000000} rows" >&2
	out=$(WP_SCOPED=1 SIFT_TMP=$tmp KEEP=1 ROWS=${WP_ROWS:-10000000} LOCK_WAIT=172800 "$BENCH/tbl-writepath.sh" 2>&1) || true
	rm -rf "$tmp/data" "$tmp/sock"
	echo "$out" | sed 's/^/  /' >&2
	VERSION=$(git -C "$dir" describe --tags --always)
	local line kv mode am key val
	while IFS= read -r line; do
		[[ $line == "RESULT "* ]] || continue
		mode=$(grep -o 'mode=[^ ]*' <<<"$line" | cut -d= -f2); am=$(grep -o ' am=[^ ]*' <<<"$line" | cut -d= -f2 || true)
		for kv in ${line#RESULT }; do
			key=${kv%%=*} val=${kv#*=}
			[[ $key != mode && $key != am ]] || continue
			emit "$mode" "${am:--}" "$key" "$val"
		done
	done <<<"$out"
	finish_item writepath "Single-row INSERT loops into a sift_table in one transaction (bench/tbl-writepath.sh)." \
		--meta "build=$VERSION" --meta "method=${WP_ROWS:-10000000} rows in the main loop, a fraction through psql, with savepoints and per access method; a cluster of its own, 2 CPUs and 12G, fsync off"
}

bm25_corpora() { # creates the tables titles(id, title) and articles(id, body) in the database bm25
	build_psql -d postgres -c "DROP DATABASE IF EXISTS bm25" -c "CREATE DATABASE bm25" >/dev/null
	build_psql -d bm25 -c "CREATE EXTENSION pg_sift" >/dev/null
	build_psql -d bm25 -v n="${BM25_TITLES:-1000000}" -f "$BENCH/sql/store/gen.sql" >/dev/null
	build_psql -d bm25 -c "CREATE TABLE titles AS SELECT id, title FROM orders_src ORDER BY id" -c "DROP TABLE orders_src" >/dev/null
	build_psql -d bm25 >/dev/null <<-EOF
		CREATE UNLOGGED TABLE src (foreign_id bigint, body text, category_id int, created date, a_count int, url text);
		\copy src FROM '$STORE/data/articles.tsv'
		CREATE TABLE articles AS SELECT row_number() OVER (ORDER BY foreign_id) AS id, body FROM (SELECT * FROM src ORDER BY hashint8(foreign_id) LIMIT ${BM25_ARTICLES:-50000}) s ORDER BY foreign_id;
		DROP TABLE src;
		VACUUM (ANALYZE) titles, articles;
	EOF
}
item_bm25size() {
	begin_item bm25size
	local l c col
	for l in v302 v310; do
		build_up bm25size "$l"
		echo "bm25size: $VERSION" >&2
		bm25_corpora
		for c in titles:title articles:body; do
			locked build_psql -At -d bm25 -v tbl="${c%%:*}" -v col="${c##*:}" -f "$BENCH/sql/extras/bm25size.sql" | emit_rows
		done
	done
	finish_item bm25size "BM25 index size and build, default and positions = off, 3.0.2 against 3.1.0 (bench/sql/extras/bm25size.sql)." \
		--meta "method=one segment built serially (max_parallel_maintenance_workers 0); titles: ${BM25_TITLES:-1000000} orders of bench/sql/store/gen.sql; articles: ${BM25_ARTICLES:-50000} Simple English Wikipedia articles of the slim tier; queries: median of 21 server-side runs"
}

item_leaftypes() {
	begin_item leaftypes
	local l wide
	for l in v310 main; do
		build_up leaftypes "$l"
		wide=0; [[ $l == main ]] && wide=1
		echo "leaftypes: $VERSION (wide=$wide)" >&2
		build_psql -d postgres -c "DROP DATABASE IF EXISTS lt" -c "CREATE DATABASE lt" >/dev/null
		build_psql -d lt -c "CREATE EXTENSION pg_sift" >/dev/null
		locked build_psql -At -d lt -v n="${LT_ROWS:-1000000}" -v wide="$wide" -f "$BENCH/sql/extras/leaftypes.sql" | emit_rows
	done
	finish_item leaftypes "Wide numeric, money and interval leaves against the narrow path of a sift_col index (bench/sql/extras/leaftypes.sql)." \
		--meta "method=${LT_ROWS:-1000000} rows, one index per side, median of 21 server-side runs, no parallel workers, sift.enable_agg off (normal plan) against on; every pair of columns holds the same values"
}

item_fields() {
	begin_item fields
	build_up fields v310
	echo "fields: $VERSION" >&2
	build_psql -d postgres -c "DROP DATABASE IF EXISTS fq" -c "CREATE DATABASE fq" >/dev/null
	build_psql -d fq -c "CREATE EXTENSION pg_sift" >/dev/null
	build_psql -d fq -v n="${ORDERS:-1000000}" -f "$BENCH/sql/store/gen.sql" >/dev/null
	locked build_psql -At -d fq -f "$BENCH/sql/extras/fields.sql" | emit_rows
	finish_item fields "Facet counts and field weights (bench/sql/extras/fields.sql)." \
		--meta "build=$VERSION" --meta "method=${ORDERS:-1000000} orders of bench/sql/store/gen.sql, median of 21 server-side runs, no parallel workers"
}

item_buildcost() {
	begin_item buildcost
	local l args=() out
	for l in v302 v310 main; do build_prepare "$l"; args+=(--build "$l=$(build_dir "$l")"); done
	[[ -z ${PG:-} ]] || scratch_down
	echo "buildcost: cpu_gate.sh ${args[*]}" >&2
	out=$("$BENCH/steps/cpu_gate.sh" "${args[@]}" --raw "$OUT/buildcost.raw" 2>&1) || true
	echo "$out" >"$OUT/buildcost.log"
	python3 -I "$HERE/extras.py" gate --log "$OUT/buildcost.log" --raw "$OUT/buildcost.raw" --rows "$ROWS_FILE" --versions "v302=$(git -C "$(build_dir v302)" describe --tags --always),v310=$(git -C "$(build_dir v310)" describe --tags --always),main=$(git -C "$(build_dir main)" describe --tags --always)"
	VERSION=gate
	finish_item buildcost "User-space instructions per operation of the server processes, 3.0.2 against 3.1.0 against main (bench/steps/cpu_gate.sh)." \
		--meta "method=perf stat instructions:u of the processes the postmaster forks for one operation, ratio to the first build with a 95% interval (geometric mean of the per-round ratios, interleaved rounds); a cell regresses when its whole interval is above 1.02"
}

item_gpu() { # bench/results/gpu.tsv keeps its own layout: rows, query, mode (cpu_1core, gpu_cold_resident), first_ms, median_ms
	local rows out=$OUT/gpu.tsv dev
	dev=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || true)
	[[ -n $dev ]] || { echo "gpu: no device, skipped" >&2; return 0; }
	build_up gpu v310-vk
	export PGOPTIONS='-c sift.gpu_min_rows=0'
	local load0; load0=$(python3 -I -c "import sys; sys.path.insert(0, '$HERE'); import extras; print(extras.host_load())")
	printf 'rows\tquery\tmode\tfirst_ms\tmedian_ms\n' >"$OUT/gpu.body"
	for rows in ${GPU_ROWS:-1000000 10000000}; do
		echo "gpu: $VERSION, $rows rows" >&2
		locked bash -c "cd '$SIFT_BUILD' && '$BENCH/gpu/bench-pg.sh' $rows ${GPU_REPS:-9}" | tee "$OUT/gpu-$rows.out" >&2
		awk -F'\t' -v rows="$rows" 'NF >= 4 && $1 != "query" {print rows "\t" $1 "\t" ($2 == "gpu" ? "gpu_cold_resident" : "cpu_1core") "\t" $3 "\t" $4}' "$OUT/gpu-$rows.out" >>"$OUT/gpu.body"
	done
	unset PGOPTIONS
	{
		echo "# Vulkan aggregate path against the CPU on the store table (bench/gpu/bench-pg.sh ROWS ${GPU_REPS:-9}), pg_sift $VERSION built with VULKAN=1, one backend, one core on the CPU side, $dev, measured $(date -u +%Y-%m-%d) under the perf lock."
		echo "# host load before: $load0; after: $(python3 -I -c "import sys; sys.path.insert(0, '$HERE'); import extras; print(extras.host_load())")."
		echo "# first_ms: the first query (cpu_1core: sift.gpu off) or the first query after the device is open, chunks uploaded (gpu_cold_resident); median_ms: median of the ${GPU_REPS:-9} repeats (chunks resident for the device)."
		cat "$OUT/gpu.body"
	} >"$out"
	echo "extras: $out" >&2
	[[ -z $DOCS ]] || cp "$out" "$REPO/bench/results/gpu.tsv"
}

for item in "${ITEMS[@]}"; do
	declare -F "item_$item" >/dev/null || { echo "extras: unknown item $item" >&2; exit 1; }
	ITEM=$item "item_$item"
done
[[ -z $DOCS ]] || python3 -I "$HERE/extras.py" render
