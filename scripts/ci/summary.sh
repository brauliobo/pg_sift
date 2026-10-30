# The summary of the run: one line per suite from the results file, the failures with the end of their logs, and the exit status.
# Sourced by scripts/ci.sh; not a script of its own.
# ---------- summary ----------
# SUITE: "status seconds skips where" of the jobs of one suite: FAIL when one failed or none ran, the time from the first start
# to the last end, the skip lines summed, the lane (or the number of lanes); then the failed jobs
summarize() {
	local expect=0 id
	for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$1" ]] && expect=$((expect + 1)); done
	awk -v s="$1" -v expect="$expect" '$2 == s { n++; sk += $5; if ($3 != "PASS") bad = bad " " $1
			if (!($6 in seen)) { seen[$6] = 1; nl++; lane = $6 }
			if (lo == "" || $7 < lo) lo = $7; if ($8 > hi) hi = $8 }
		END { printf "%s %d %d %s%s\n", (bad != "" || n == 0 || n < expect ? "FAIL" : "PASS"), hi - lo, sk, (nl > 1 ? nl "_lanes" : lane), bad }' "$LOG/results"
}
echo
green=$'\e[32m' red=$'\e[31m' off=$'\e[0m'
[[ -t 1 ]] || green= red= off=
failed=0
# the suites in the order of the report (a suite not listed follows the others)
ORDER=(build simd-level "${HARNESS[@]}")
for s in isa-baseline installcheck isolation-emulated isolation fuzz hardened static difftest-mutants soak soak-gin soak-col difftest difftest-nopos tap-shell tap "${SUITES[@]}"; do
	[[ " ${SUITES[*]} " != *" $s "* || " ${ORDER[*]} " == *" $s "* ]] || ORDER+=("$s")
done
for s in "${ORDER[@]}" leakcheck; do
	read -r status secs skips where bad <<<"$(summarize "$s")"
	line=$(printf '%s %-20s %5ss  %s skip lines  (%s)' "$status" "$s" "$secs" "$skips" "${where/_/ }")
	if [[ $status == PASS ]]; then echo "$green$line$off"; else failed=1; echo "$red$line$off"; fi
	# the log of a split suite: the logs of its jobs, in order
	[[ -f $LOG/$s.log ]] || for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$s" ]] && { echo "== $id"; cat "$LOG/$id.log"; }; done >"$LOG/$s.log"
	[[ -z ${bad:-} ]] || echo "$red  failed:$bad$off"
	# a batch of regression tests names the tests that failed
	for id in ${bad:-}; do [[ $id != installcheck-batch* ]] || grep -h '^not ok' "$LOG/$id.log" | sed "s/^/$red    /; s/\$/$off/"; done
done
[[ ! -e $LOG/cap_exceeded ]] || { failed=1; echo "$red$(cat "$LOG/cap_exceeded")$off"; }
# a server that crashed: the log of its job says only that psql lost the connection; the signal and the statement the backend ran are in the server log
if (( failed )); then
	find "$BASE" -path "$LOG" -prune -o -name '*.log' -print 2>/dev/null | xargs -r grep -hE -A1 'was terminated by signal|^TRAP:|PANIC:' 2>/dev/null | cut -c1-300 | head -n 30 |
		{ read -r first && { echo "${red}server crashes (signal, statement, assertion):$off"; echo "$first"; cat; }; }
	# the full tier runs on the ASan build: a backend that ASan aborts (signal 6) has no stack in the server log, its report is in the sanitizer directory of the lane
	find "$BASE" -path "$LOG" -prune -o -path '*/sanitizer/*' -type f -print 2>/dev/null | head -n 5 | while read -r f; do
		echo "${red}sanitizer report ${f#"$BASE"/}:$off"; grep -m1 -A9 -E 'ERROR: |runtime error' "$f" | cut -c1-240
	done
	# UBSan does not write to that directory: its runtime error and stack are in the server log (runs 40852955112: shards 18-3 and 18-7 had only these)
	find "$BASE" -path "$LOG" -prune -o -name postgres.log -print 2>/dev/null | head -n 20 | while read -r f; do
		grep -q 'runtime error' "$f" 2>/dev/null && { echo "${red}UBSan ${f#"$BASE"/}:$off"; grep -m1 -A9 'runtime error' "$f" | cut -c1-240; }
	done
fi
# the sampler is reaped within 20 s whatever it does: a run must not outlive its summary (arm64 shard of run 40152868204)
if [[ -n ${STATS:-} ]]; then
	kill "$STATS" 2>/dev/null
	for _ in {1..20}; do kill -0 "$STATS" 2>/dev/null || break; sleep 1; done
	if kill -0 "$STATS" 2>/dev/null; then echo "ci: the footprint sampler ignored TERM for 20 s:"; ps -o pid,stat,wchan:20,args --ppid "$STATS"; kill -9 "$STATS"; fi
	wait "$STATS" 2>/dev/null
	cat "$LOG/stats.txt"
fi
printf 'wall %ss (%s lanes), peak scratch %s MB, logs %s\n' "$((SECONDS - t0))" "$LANES" "$(cat "$LOG/peak_mb" 2>/dev/null || echo '<2')" "$LOG"
exit $failed
