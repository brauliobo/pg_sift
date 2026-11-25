# Checks that compare answers, shared by the shell TAP tests (sourced by common.bash). NODE is a node of common.bash, QUERY a SELECT.

# tap_answer NODE on|off QUERY: the md5 of the sorted rows of QUERY with sift.enable_agg on or off and no parallel workers
tap_answer() {
	sql "$1" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $2" \
		"SELECT md5(coalesce(string_agg(x::text, ',' ORDER BY x::text), '')) FROM ($3) x" | tail -n 1
}

# tap_same_answers NODE QUERY: QUERY answers the same with the pushdown and without; a difference is a comment with both answers
tap_same_answers() {
	local on off
	on=$(tap_answer "$1" on "$2") off=$(tap_answer "$1" off "$2")
	[[ $on == "$off" ]] || diag "$2: pushdown $on, normal plan $off"
	[[ $on == "$off" ]]
}

# tap_sift_vs_plain DESCRIPTION NODE QUERY: a check that QUERY answers the same with the pushdown and without
tap_sift_vs_plain() { check "$1" tap_same_answers "$2" "$3"; }

# tap_digest NODE RELATION [ORDER [EXPRESSION]]: the row count of RELATION and the md5 of EXPRESSION (the row as text) of its rows in ORDER, as count:md5
tap_digest() { sql "$1" "SELECT count(*) || ':' || md5(coalesce(string_agg(${4:-t::text}, ',' ORDER BY ${3:-t::text}), '')) FROM $2 t" | tail -n 1; }

# tap_verified NODE INDEX: sift.col_verify of INDEX finds nothing wrong
tap_verified() { [[ -z $(sql "$1" "SELECT sift.col_verify('$2', true)" 2>&1) ]]; }
