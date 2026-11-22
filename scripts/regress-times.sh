#!/usr/bin/env bash
# Records the seconds each regression test took inside its batch, from the job logs of a kept run (KEEP=1 scripts/ci.sh quick; the logs are <scratch>/logs/installcheck*.log), as
# scripts/regress-times.tsv: scripts/ci/jobs.sh packs the batches by them. A test that ran in several logs keeps its longest time.
#   scripts/regress-times.sh LOGDIR...     the new table on stdout
#   scripts/regress-times.sh --selftest
set -uo pipefail

times() { cat "$@" 2>/dev/null | sed -En 's/^(not )?ok +[0-9]+ +[-+] +([^ ]+) +([0-9]+) ms.*/\2\t\3/p' |
	awk -F'\t' '{ if ($2 > ms[$1]) ms[$1] = $2 } END { for (t in ms) printf "%s\t%.2f\n", t, ms[t] / 1000 }' | sort; }

if [[ ${1:-} == --selftest ]]; then
	got=$(times <(printf 'ok 1         - alpha                            1500 ms\nnot ok 2     - beta                              20 ms\nok 3         + alpha                            900 ms\nnoise\n'))
	[[ $got == $'alpha\t1.50\nbeta\t0.02' ]] || { echo "regress-times selftest: got '$got'"; exit 1; }
	echo "regress-times selftest ok"
	exit 0
fi
(( $# )) || { echo "usage: scripts/regress-times.sh LOGDIR... | --selftest" >&2; exit 1; }
printf '# Seconds of each regression test inside its batch, recorded by scripts/regress-times.sh from the logs of a kept run; scripts/ci/jobs.sh packs the batches by them.\n'
printf '# A test that is not here weighs REGRESS_TEST_SECS. TEST<TAB>SECONDS.\n'
times $(for d in "$@"; do ls "$d"/installcheck*.log 2>/dev/null; done)
