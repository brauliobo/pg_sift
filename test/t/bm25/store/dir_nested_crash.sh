#!/usr/bin/env bash
# The flip of a chained directory of nested partitions under SIGKILL: splits, merges, rewrites, insertions and deletions of partitions whose bounds are of one to three key components
# and an address, on a test index whose chain is two levels deep (leaves of 3 entries, interior pages of 4 references), killed at every directory update of the sequence at each point of
# the update (before its locks, after the pages of the new chain and before the record that flips to it, after the record). After crash recovery the directory is the one before the
# operation that held the update, or the one after it when the update was its last and the fault came after it, its chain is sound, a read by a prefix of 0 to 3 components, or of
# 3 and an address, returns the entries of no partition and those of the partitions that may hold a key with the prefix (as the bounds of the directory say), and the rest of the
# sequence applied to it ends where a clean run does. The primary runs with wal_consistency_checking = generic.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

OPS=("i 0 3" "n 3 0 0,70000,140000,3000000" "n 4 1 70000,70100" "n 3 2 0" "n 7 0 5000000" "n 5 1 140000,140001,140002" "n 4 1 70100,70101,70102,70103"
	"n 3 4 0,200" "n 5 2 70103,70104,100000" "n 10 1 3000000,3000064" "n 12 1 5000000" "d 4 1" "n 3 1 0,1,2")
POINTS=(before chain after)
STEP=$(knob SIFT_DIRNEST_STEP 1 2)
SHAPE=("SET sift.test_dir_flat_max = 2" "SET sift.test_dir_leaf_max = 3" "SET sift.test_dir_root_max = 4")

node_init dn 1 "$WAL_CONF
wal_consistency_checking = 'generic'
autovacuum = off"
node_start dn || bail "start failed"
SIFT_TMP=$(dir dn) leak_expect 400 'terminated by signal 9|terminating connection'
sql dn "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
sql dn "CREATE FUNCTION nd_expected(ix regclass, a int8, b int8) RETURNS SETOF int8 LANGUAGE sql AS \$\$
	WITH l AS (SELECT ord, gen FROM sift._test_dir_list(ix)), n AS (SELECT first, ord AS o, lead(ord) OVER (ORDER BY first) AS nxt FROM sift._test_dir_nested(ix))
	SELECT gen FROM l WHERE ord < (SELECT COALESCE(min(first), 1000000) FROM n)
	UNION ALL SELECT l.gen FROM l JOIN n ON l.ord = n.first WHERE n.o < b AND COALESCE(n.nxt, b) > a \$\$" \
	"CREATE FUNCTION nd_prefix_ok(ix regclass) RETURNS boolean LANGUAGE sql AS \$\$
	SELECT bool_and(NOT EXISTS (SELECT gen FROM sift._test_dir_prefix(ix, p.d, p.t) EXCEPT SELECT * FROM nd_expected(ix, p.a, p.b))
		AND NOT EXISTS (SELECT * FROM nd_expected(ix, p.a, p.b) EXCEPT SELECT gen FROM sift._test_dir_prefix(ix, p.d, p.t)))
	FROM (VALUES (ARRAY[]::int[], -1::int8, 0::int8, 16777216::int8), (ARRAY[0], -1, 0, 262144), (ARRAY[17], -1, 17::int8 << 18, 18::int8 << 18),
		(ARRAY[17, 0], -1, 17::int8 << 18, (17::int8 << 18) + 4096), (ARRAY[17, 0, 19], -1, (17::int8 << 18) + 19 * 64, (17::int8 << 18) + 20 * 64),
		(ARRAY[17, 0, 19], 20, (17::int8 << 18) + 19 * 64 + 20, (17::int8 << 18) + 19 * 64 + 21), (ARRAY[53], -1, 53::int8 << 18, 54::int8 << 18)) p(d, t, a, b) \$\$" >/dev/null || bail "functions failed"

op_sql() { # INDEX "op...": the statement of an operation
	local ix=$1 op a b c
	read -r op a b c <<<"$2"
	case $op in
		i) echo "SELECT sift._test_dir_edit('$ix', 'ins', $a, $b)" ;;
		d) echo "SELECT sift._test_dir_edit('$ix', 'del', $a, $b)" ;;
		n) echo "SELECT sift._test_dir_nest('$ix', $a, $b, '{$c}'::int8[])" ;;
	esac
}
state() { sql dn "SELECT coalesce(string_agg(ord || ':' || gen, ',' ORDER BY ord), '') || '|' || coalesce((SELECT string_agg(first || ':' || ord, ',' ORDER BY first) FROM sift._test_dir_nested('$1')), '') FROM sift._test_dir_list('$1')"; }
newtable() { sql dn "CREATE TABLE t$1 (v int)" "CREATE INDEX $1 ON t$1 USING sift_store_test (v)" >/dev/null || bail "create $1"; }
apply() { # INDEX FROM TO: operations FROM..TO of the sequence, completely
	local i
	for ((i = $2; i <= $3; i++)); do out=$(sql dn "${SHAPE[@]}" "$(op_sql "$1" "${OPS[i - 1]}")" 2>&1) || bail "$1: operation $i (${OPS[i - 1]}) failed: $out from $(state "$1")"; done
}

newtable m
S=("|")
K=(0)
for i in $(seq "${#OPS[@]}"); do
	updates=$(sql dn "${SHAPE[@]}" "SET sift.test_crash_dir = 0" "$(op_sql m "${OPS[i - 1]}")" "SELECT sift._test_dir_updates()" | tail -1)
	S[i]=$(state m)
	K[i]=$((K[i - 1] + updates))
done
final=${S[${#OPS[@]}]}
total=${K[${#OPS[@]}]}
chained=$(sql dn "SELECT chained FROM sift._test_dir_info('m')")
plan $(( (total + STEP - 1) / STEP * ${#POINTS[@]} + 2 ))
ok "clean run: ${#OPS[@]} operations make $total directory updates, directory chained: $chained, prefixes read: $(sql dn "SELECT nd_prefix_ok('m')")"
[[ $chained == t && $(sql dn "SELECT nd_prefix_ok('m')") == t ]] || bail "the nested test directory is not chained or a prefix read is wrong"

case_no=0
for point in "${POINTS[@]}"; do
	for ((u = 1; u <= total; u += STEP)); do
		case_no=$((case_no + 1))
		a=a$case_no
		i=1
		while ((K[i] < u)); do i=$((i + 1)); done
		newtable "$a"
		apply "$a" 1 $((i - 1))
		sql dn "CHECKPOINT" >/dev/null
		chain="SET sift.test_crash_chain = off"
		[[ $point != chain ]] || chain="SET sift.test_crash_chain = on"
		local_u=$((u - K[i - 1]))
		alive=1
		sql dn "${SHAPE[@]}" "$chain" "SET sift.test_crash_dir = $([[ $point == after ]] && echo -$local_u || echo $local_u)" \
			"SET sift.test_crash_kind = 0" "$(op_sql "$a" "${OPS[i - 1]}")" >/dev/null 2>&1 || alive=0
		((alive == 1)) || wait_for "! kill -0 \$(head -1 $(dir dn)/data/postmaster.pid 2>/dev/null) 2>/dev/null" 60
		if ((alive == 0)); then
			node_start dn || bail "case $case_no: restart failed"
			want=${S[i - 1]}
			[[ $point == after && $u == "${K[i]}" ]] && want=${S[i]}
		else
			want=${S[i]}
		fi
		got=$(state "$a")
		checked=$(sql dn "SELECT sift._test_dir_check('$a')" 2>&1)
		prefixes=$(sql dn "SELECT nd_prefix_ok('$a')" 2>&1)
		[[ $got == "${S[i]}" ]] || apply "$a" "$i" "$i"
		apply "$a" $((i + 1)) "${#OPS[@]}"
		if [[ $got == "$want" && $checked =~ ^[0-9]+$ && $prefixes == t && $(state "$a") == "$final" ]]; then
			ok "$point, update $u (operation $i): directory of $(sql dn "SELECT nsegs FROM sift._test_dir_info('$a')") entries, prefix reads hold"
		else
			not_ok "$point, update $u (operation $i)"
			diag "want $want"$'\n'"got  $got"$'\n'"check=$checked prefixes=$prefixes final=$(state "$a") want=$final"
		fi
		sql dn "DROP TABLE t$a" >/dev/null 2>&1
	done
done
ok "the sequence ends where a clean one does"
finish
