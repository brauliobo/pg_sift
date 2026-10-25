#!/usr/bin/env bash
# Usage: bench/steps/store_scaling.sh OUT STORE_JSON [STORE_JSON...]   (bench/bench store scaling ...)
# The results of the store questions at other sizes than 1,000,000 orders, each from its own `bench/bench store all --sizes N,N` run (bench/out/run-*/store.json), into the one file that the
# scale charts read: every size of every run is kept, a size of a later file is not mixed into one of an earlier file, and a run measured on another host is
# refused (steps/store.py merge --keep).
#   bench/bench store all --sizes 25000,100000 --cold-runs 0 --restart-runs 0 --update-sizes none
#   bench/bench store all --sizes 10000000 --runs 3 --warmups 1 --cold-runs 0 --restart-runs 0 --update-sizes none
#   bench/bench store scaling bench/results/store-scaling.json bench/out/run-A/store.json bench/out/run-B/store.json
set -euo pipefail
STEPS=$(cd "$(dirname "$0")" && pwd)
(( $# >= 2 )) || { sed -n '2,3p' "$0" | sed 's/^# //'; exit 2; }
out=$1; shift
keep=()
for f in "$@"; do
  [[ -s $f ]] || { echo "store scaling: $f is not a results file" >&2; exit 1; }
  keep+=(--keep "$f")
done
tmp=$(mktemp "$out.XXXXXX")
trap 'rm -f "$tmp"' EXIT
python3 -I "$STEPS/store.py" merge -o "$tmp" "${keep[@]}"
python3 -I - "$tmp" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
tiers = d['meta']['tiers']
if not tiers:
    sys.exit('store scaling: no size was carried over (another host, or the same size twice)')
print('store scaling: %s' % ', '.join('%s (%s orders)' % (t, format(v['rows'], ',')) for t, v in sorted(tiers.items(), key=lambda kv: kv[1]['rows'])), file=sys.stderr)
PY
mv "$tmp" "$out"
trap - EXIT
