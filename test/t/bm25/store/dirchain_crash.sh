#!/usr/bin/env bash
# The flip of a chained directory under SIGKILL: a sequence of segment writes and drops on a test index whose directory is chained from its third entry on,
# killed at every directory update of the sequence at each point of the update: before it takes its locks, after it has written and logged the pages of the
# new chain and before the record that flips to it, after the record. After crash recovery the directory is the one before the operation that held the update,
# or the one after it when the update was its last and the fault came after it; the chain is sound, its pages are on no free list, the pages a killed update
# left are orphans that the sweep reclaims (the pages of a chain that was not flipped are in flight until the next update moves the version past them), every segment reads back byte-exact, and the rest of the sequence applied to it ends with the segments of an index
# that was never killed. The primary runs with wal_consistency_checking = generic, so that every record of the flip is checked against the page it changes.
. "$(dirname "$0")/lib.bash"

OPS=(w w w w w w w "d 0" "d 3" w "d 1" w "d 0")
POINTS=(before chain after)
STEP=$(knob SIFT_DIRCHAIN_STEP 1 3)
SHAPE=("SET sift.test_dir_flat_max = 2" "SET sift.test_dir_leaf_max = 3" "SET sift.test_dir_root_max = 12")

node_init dc 1 "$WAL_CONF
wal_consistency_checking = 'generic'
autovacuum = off"
node_start dc || bail "start failed"
SIFT_TMP=$(dir dc) leak_expect 400 'terminated by signal 9|terminating connection'
sql dc "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

op_sql() { # INDEX OPNO "op...": the statement of an operation
	local ix=$1 no=$2 op k
	read -r op k <<<"$3"
	if [[ $op == w ]]; then
		echo "SELECT sift._test_seg_write('$ix', $((100 + 37 * no)), $no)"
	else
		echo "SELECT sift._test_seg_drop('$ix', (SELECT gen FROM sift._test_segments('$ix') ORDER BY gen OFFSET $k LIMIT 1))"
	fi
}
state() { sql dc "SELECT coalesce(string_agg(gen || ':' || size, ',' ORDER BY gen), '') FROM sift._test_segments('$1')"; }
intact() { sql dc "SELECT count(*) || ' ' || coalesce(bool_and(sift._test_seg_read('$1', gen) = payload), true) FROM sift._test_segments('$1')"; }
newtable() { sql dc "CREATE TABLE t$1 (v int)" "CREATE INDEX $1 ON t$1 USING sift_store_test (v)" >/dev/null || bail "create $1"; }
apply() { # INDEX FROM TO: operations FROM..TO of the sequence, completely
	local i
	for ((i = $2; i <= $3; i++)); do sql dc "${SHAPE[@]}" "$(op_sql "$1" "$i" "${OPS[i - 1]}")" >/dev/null || bail "$1: operation $i failed"; done
}

# a clean run: the state after each operation and the number of directory updates it made
newtable m
S=("")
K=(0)
for i in $(seq "${#OPS[@]}"); do
	updates=$(sql dc "${SHAPE[@]}" "SET sift.test_crash_dir = 0" "$(op_sql m "$i" "${OPS[i - 1]}")" "SELECT sift._test_dir_updates()" | tail -1)
	S[i]=$(state m)
	K[i]=$((K[i - 1] + updates))
done
final=$(intact m)
total=${K[${#OPS[@]}]}
chained=$(sql dc "SELECT chained FROM sift._test_dir_info('m')")
plan $(( (total + STEP - 1) / STEP * ${#POINTS[@]} + 2 ))
ok "clean run: ${#OPS[@]} operations make $total directory updates, directory chained: $chained"
[[ $chained == t ]] || bail "the test directory is not chained"

case_no=0
for point in "${POINTS[@]}"; do
	for ((u = 1; u <= total; u += STEP)); do
		case_no=$((case_no + 1))
		a=a$case_no
		i=1
		while ((K[i] < u)); do i=$((i + 1)); done
		newtable "$a"
		apply "$a" 1 $((i - 1))
		chain="SET sift.test_crash_chain = off"
		[[ $point != chain ]] || chain="SET sift.test_crash_chain = on"
		local_u=$((u - K[i - 1]))
		alive=1
		sql dc "${SHAPE[@]}" "$chain" "SET sift.test_crash_dir = $([[ $point == after ]] && echo -$local_u || echo $local_u)" \
			"SET sift.test_crash_kind = 0" "$(op_sql "$a" "$i" "${OPS[i - 1]}")" >/dev/null 2>&1 || alive=0
		((alive == 1)) || wait_for "! kill -0 \$(head -1 $(dir dc)/data/postmaster.pid 2>/dev/null) 2>/dev/null" 60
		if ((alive == 0)); then
			node_start dc || bail "case $case_no: restart failed"
			want=${S[i - 1]}
			[[ $point == after && $u == "${K[i]}" ]] && want=${S[i]}
		else
			want=${S[i]}
		fi
		got=$(state "$a")
		checked=$(sql dc "SELECT sift._test_dir_check('$a')" 2>&1)
		swept=$(sql dc "SELECT sift._test_sweep('$a')" 2>&1)
		read_back=$(intact "$a" 2>&1)
		[[ $got == "${S[i]}" ]] || apply "$a" "$i" "$i"
		apply "$a" $((i + 1)) "${#OPS[@]}"
		sql dc "SELECT sift._test_sweep('$a')" >/dev/null 2>&1
		orphans=$(sql dc "SELECT sift._test_check('$a')" 2>&1)
		if [[ $got == "$want" && $checked =~ ^[0-9]+$ && $swept =~ ^[0-9]+$ && $orphans == 0 && $read_back == *" true" && $(intact "$a") == "$final" ]]; then
			ok "$point, update $u (operation $i): $(sql dc "SELECT nsegs FROM sift._test_dir_info('$a')") segments, $swept orphan blocks swept"
		else
			not_ok "$point, update $u (operation $i)"
			diag "want $want"$'\n'"got  $got"$'\n'"check=$checked swept=$swept orphans=$orphans read=$read_back final=$(intact "$a") want=$final"
		fi
		sql dc "DROP TABLE t$a" >/dev/null 2>&1
	done
done
ok "the sequence ends where a clean one does"
finish
