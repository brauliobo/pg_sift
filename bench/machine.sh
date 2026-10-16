#!/usr/bin/env bash
# Usage: bench/machine.sh > machine.txt
# Describes the machine and the engines of a run as `key: value` lines so results from different
# hosts and sessions can be compared. No host name or user path is printed. ab.sh, places.sh and
# reproduce.sh store it next to their results.
. "$(dirname "$0")/lib.sh"

cpu() { grep -m1 -oP 'model name\s+: \K.*' /proc/cpuinfo; }
simd() { local f; for f in avx2 avx512f avx512bw avx512vl; do grep -qw "$f" /proc/cpuinfo && printf '%s ' "$f"; done; }
os() { . /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" || uname -o; }
# default_version of an extension, looked up in the order the bench cluster searches (bench/cluster.sh)
ext_version() {
  local d IFS=:
  for d in $EXT_SHARE $SHAREDIR; do
    [[ -f $d/extension/$1.control ]] && { sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$d/extension/$1.control"; return; }
  done
  echo "not installed"
}
# what bench/fetch.sh sift built (commit, + marker when the working tree was used with local changes)
sift_commit() { cat "$SIFT_ROOT/.build-info" 2>/dev/null || echo "not built by bench/fetch.sh"; }

echo "date: $(date -u +%FT%TZ)"
echo "cpu: $(cpu)"
echo "cpu threads: $(nproc --all), cores: $(lscpu -p=core,socket 2>/dev/null | grep -v '^#' | sort -u | wc -l), simd: $(simd)"
echo "memory: $(awk '/MemTotal/ {printf "%.0f GiB", $2 / 1048576}' /proc/meminfo)"
echo "kernel: $(uname -sr)"
echo "os: $(os)"
echo "bench root: $(stat -f -c %T "$ROOT") (free: $(df -h --output=avail "$ROOT" 2>/dev/null | tail -1 | tr -d ' '))"
echo "postgres: $(pg_config --version)"
echo "engine budget: $([[ $BUDGET_ENFORCED == yes ]] && echo "cgroup scope CPUQuota=${CPU_QUOTA:-800%} MemoryMax=${MEM_MAX:-32G}" || echo "not enforced (no systemd user session)")"
echo "engines: sift $(ext_version sift) ($(sift_commit)), pg_search $(ext_version pg_search) (pinned $PARADEDB_VERSION), rum $(ext_version rum) (pinned tag $RUM_VERSION)"
echo "sift commit: $(sift_commit)"
echo "load average at start: $(cut -d' ' -f1-3 /proc/loadavg)"
