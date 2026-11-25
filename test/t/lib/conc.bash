((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Sessions that hold a lock or a transaction while a test works against them, shared by the shell TAP tests (sourced by common.bash, which releases
# every holder when the script ends). NODE is a node of common.bash.
declare -A TAP_TX_PID=() TAP_TX_DIR=()

# tap_tx_hold NODE NAME STATEMENT: a session of NODE that runs STATEMENT (the lock or the transaction to hold) and stays until tap_tx_release NAME; it has run when this returns
tap_tx_hold() {
	local node=$1 name=$2 d
	d=$(dir "$node")
	rm -f "$d/held.$name" "$d/release.$name"
	{ echo "$3;"; echo "\\! touch '$d/held.$name'"; echo "\\! while [ ! -e '$d/release.$name' ]; do sleep 0.05; done"; } | sql "$node" >/dev/null 2>&1 &
	TAP_TX_PID[$name]=$! TAP_TX_DIR[$name]=$d
	wait_for "[[ -e $d/held.$name ]]" 60 || bail "the holder $name did not start on $node"
}

# tap_tx_blocked NODE [N [LOCKTYPE]]: at least N (1) sessions of NODE wait for a lock of LOCKTYPE (advisory)
tap_tx_blocked() { [[ $(sql "$1" "SELECT count(*) FROM pg_locks WHERE NOT granted AND locktype = '${3:-advisory}'") -ge ${2:-1} ]]; }

# tap_tx_wait_blocked NODE [N [LOCKTYPE]]: waits until tap_tx_blocked holds
tap_tx_wait_blocked() { wait_for "tap_tx_blocked $1 ${2:-1} ${3:-advisory}" 60; }

# tap_tx_release NAME: lets the holder NAME go and waits for it
tap_tx_release() {
	touch "${TAP_TX_DIR[$1]}/release.$1"
	wait "${TAP_TX_PID[$1]}"
	unset "TAP_TX_PID[$1]" "TAP_TX_DIR[$1]"
}

tap_tx_release_all() { local name; for name in "${!TAP_TX_PID[@]}"; do tap_tx_release "$name"; done; }
