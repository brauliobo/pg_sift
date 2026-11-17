# The skips of a job and the retained summary of a run: one line per job with its status, seconds, scratch peak and the reasons it skipped.
# Used by step(), the summary of a run, `ci.sh summary` and the selftest. Sourced by scripts/ci.sh; not a script of its own.
NO_REASON='(no reason)'
# job_skips LOG: one line per skip in the log of a job, its reason ($NO_REASON when the line gives none). The forms: a TAP directive (`ok 3 # skip WHY`, `1..0 # SKIP WHY`),
# prove (`t/x.sh .. skipped: WHY`) and the SKIP marker of the scripts (`SKIP: WHY`, `tool: SKIP: WHY`)
job_skips() {
	awk -v none="$NO_REASON" '
		function reason(r) { gsub(/^[ :,;-]+|[ \t]+$/, "", r); if (r == "" || tolower(r) == "no reason given") r = none; print r }
		{ l = tolower($0) }
		$0 ~ /^(not )?ok / && (i = index(l, "# skip")) { r = substr($0, i + 6); sub(/^ped/, "", r); reason(r); next }
		l ~ /^1\.\.0 +# *skip/ { i = index(l, "skip"); r = substr($0, i + 4); sub(/^ped/, "", r); reason(r); next }
		$0 ~ / \.\. skipped([: ]|$)/ { i = index(l, ".. skipped"); reason(substr($0, i + 10)); next }
		$0 ~ /(^|: )SKIP([: ]|$)/ { i = match($0, /SKIP/); reason(substr($0, i + 4)) }' "$1"
}
# job_state STATUS LOG: PASS and FAIL as they are; a job that passed with skip lines and no test that ran is SKIP
job_state() {
	[[ $1 == PASS ]] && job_skips "$2" | grep -q . && ! grep -qE '^ok [0-9]+ ([^#]|$)|^ok [0-9]+$|\.\. ok$' "$2" && { echo SKIP; return; }
	echo "$1"
}
# job_row ID STATUS SECONDS PEAK_MB LOG: the line of the retained summary, tab separated: id, state, seconds, scratch peak MB, skip count, the reasons joined by " | "
job_row() {
	local reasons
	reasons=$(job_skips "$5" | awk '!seen[$0]++ { printf "%s%s", (n++ ? " | " : ""), $0 }')
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$(job_state "$2" "$5")" "$3" "$4" "$(job_skips "$5" | grep -c .)" "$reasons"
}
# summary_report FILE...: the jobs of retained summaries counted by state, every job that skipped with its reasons; fails when a skip has no reason
summary_report() {
	awk -F'\t' -v none="$NO_REASON" '
		$5 > 0 { skipped++; print "  " $2 " " $1 ": " $6; if (index($6, none)) bad++ }
		{ n[$2]++ }
		END { printf "%d jobs: %d pass, %d fail, %d skip; %d report skips\n", NR, n["PASS"], n["FAIL"], n["SKIP"], skipped; exit bad > 0 }' "$@"
}
