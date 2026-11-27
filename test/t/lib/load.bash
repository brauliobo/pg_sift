# load_suite NODE ARGS...: runs scripts/load/run.py ARGS against NODE (its server log scanned for crashes and leaks; LOAD_CRASH kills it with -9, LOAD_RESTART starts it
# again, for a case that crashed it) and turns every LOAD line into a check: PASS is ok, SKIP a skip, OPEN=ID a TODO (the case waits for the fix of finding ID), anything else
# not ok; LOAD_ONLY=RE runs the cases whose name matches, LOAD_ARGS=--determinism the other modes
load_suite() {
	local node=$1 d out rc l name verdict
	shift
	d=$(dir "$node")
	local -a args=()
	[[ -r /proc/self/smaps_rollup ]] || args+=(--no-rss)
	export -f crash real_node dir
	export TAPDIR
	out=$(PGHOST=$d/sock PGPORT=$(port "$node") PGUSER=postgres LOAD_CRASH="crash $node" \
		LOAD_RESTART="until ! pg_ctl -D '$d/data' status >/dev/null; do sleep 0.05; done; LD_PRELOAD='${SIFT_PRELOAD:-}' pg_ctl -D '$d/data' -l '$d/postgres.log' -w -t 120 start >/dev/null && '$TAP_SCRIPTS/leakcheck.sh' baseline '$d/data'" \
		python3 -I "$SIFT_REPO/scripts/load/run.py" --log "$d/postgres.log" "${args[@]}" "$@" ${LOAD_ONLY:+--only "$LOAD_ONLY"} ${LOAD_ARGS:-} 2>"$d/load.err")
	rc=$?
	[[ -s $d/load.err ]] && diag "$(cat "$d/load.err")"
	diag "$out"
	check "the runner exited 0 (it exited $rc)" test "$rc" -eq 0
	while IFS= read -r l; do
		[[ $l == "LOAD "[a-z]* ]] || continue
		name=${l#LOAD } name=${name%% *} verdict=${l##* }
		case $verdict in
		PASS) ok "$name" ;;
		SKIP) ok "$name # SKIP $(sed -n 's/^LOAD [a-z0-9_]* \[f[12]\] \(.*\) SKIP$/\1/p' <<<"$l")" ;;
		OPEN=*) NTEST=$((NTEST + 1)); echo "not ok $NTEST - $name # TODO ${verdict#OPEN=}" ;;
		*) not_ok "$name: $l" ;;
		esac
	done <<<"$out"
}
