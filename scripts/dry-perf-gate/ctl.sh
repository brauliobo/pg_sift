#!/bin/sh
# ctl.sh enable|disable: the measured window of one run. Writes the command to the control fifo of this run's `perf stat` ($GATE_CTL) and waits for its
# acknowledgement ($GATE_ACK), so the counters are on before the next statement starts and off before the next one. Called from psql (\!), 20 s limit.
# shellcheck disable=SC2016  # the single quotes are the point: the inner shell expands them
exec timeout 20 sh -c 'printf "%s\n" "$1" > "$GATE_CTL" && IFS= read -r ack < "$GATE_ACK"' sh "$1"
