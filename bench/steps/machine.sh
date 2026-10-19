#!/usr/bin/env bash
# Usage: bench/steps/machine.sh > machine.txt
# Describes the machine and the engines of a run as `key: value` lines so results from different
# hosts and sessions can be compared. No host name or user path is printed. articles.sh and places.sh
# store it next to their results.
. "$(dirname "$0")/lib.sh"

cpu() { grep -m1 -oP 'model name\s+: \K.*' /proc/cpuinfo; }
simd() { local f; for f in avx2 avx512f avx512bw avx512vl; do grep -qw "$f" /proc/cpuinfo && printf '%s ' "$f"; done; }
os() { if . /etc/os-release 2>/dev/null; then echo "$PRETTY_NAME"; else uname -o; fi; }
# default_version of an extension, looked up in the order the bench cluster searches (bench/steps/cluster.sh)
ext_version() {
  local d IFS=:
  for d in $EXT_SHARE $SHAREDIR; do
    [[ -f $d/extension/$1.control ]] && { sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$d/extension/$1.control"; return; }
  done
  echo "not installed"
}
# where the cluster finds the extension: the engine root of bench/steps/fetch.sh or the system
ext_origin() {
  local d IFS=:
  for d in $EXT_SHARE; do [[ -f $d/extension/$1.control ]] && { echo "bench root build"; return; }; done
  echo "system package"
}
# what bench/steps/fetch.sh sift built (commit, + marker when the working tree was used with local changes)
sift_commit() { cat "$SIFT_ROOT/.build-info" 2>/dev/null || echo "not built by bench/steps/fetch.sh"; }

# the CPU quota of the scope this runs in (systemd-run -p CPUQuota=...), the closest limit up the cgroup tree: "N CPUs", or "none" when the host's CPUs are all available
cpu_quota() {
  local d=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup) q p
  while [[ $d == /sys/fs/cgroup* ]]; do
    if [[ -r $d/cpu.max ]]; then read -r q p <"$d/cpu.max"; [[ $q == max ]] || { awk -v q="$q" -v p="$p" 'BEGIN {printf "%.4g CPUs", q / p}'; return; }; fi
    [[ $d == /sys/fs/cgroup ]] && break
    d=${d%/*}
  done
  echo none
}
echo "date: $(date -u +%FT%TZ)"
echo "cpu: $(cpu)"
echo "cpu quota: $([[ -n ${SERVER_BUDGET:-} ]] && echo "${SERVER_BUDGET} CPUs" || cpu_quota)"${SERVER_BUDGET:+" (servers only: cores $SERVER_CPUS; the clients run outside the cap)"}
echo "cpu threads: $(nproc --all), cores: $(lscpu -p=core,socket 2>/dev/null | grep -v '^#' | sort -u | wc -l), simd: $(simd)"
echo "memory: $(awk '/MemTotal/ {printf "%.0f GiB", $2 / 1048576}' /proc/meminfo)"
echo "kernel: $(uname -sr)"
echo "os: $(os)"
echo "bench root: $(stat -f -c %T "$ROOT") (free: $(df -h --output=avail "$ROOT" 2>/dev/null | tail -1 | tr -d ' '))"
echo "postgres: $(pg_config --version)"
echo "method: one engine at a time on $([[ -n ${SERVER_BUDGET:-} ]] && echo "a server capped at $SERVER_BUDGET CPUs, the clients outside the cap" || { [[ $(cpu_quota) == none ]] && echo 'every CPU of the host' || echo "the CPUs of its scope ($(cpu_quota))"; }), interleaved rounds, ${CLIENTS:-8} clients on the articles, one on the places"
echo "engines: sift $(ext_version pg_sift) ($(sift_commit)), pg_search $(ext_version pg_search) ($(ext_origin pg_search); pinned $PARADEDB_VERSION)," \
  "pg_textsearch $(ext_version pg_textsearch) ($(ext_origin pg_textsearch); pinned tag v$PG_TEXTSEARCH_VERSION, commit ${PG_TEXTSEARCH_COMMIT:0:12}), rum $(ext_version rum) ($(ext_origin rum); pinned tag $RUM_VERSION)," \
  "pgx-bm25 $(ext_version bm25_native) ($(ext_origin bm25_native))"
es_description() { # the Elasticsearch of the run, when it is installed: the system package's version, its JVM, heap, shards
  local es=${ES_HOME:-/usr/share/elasticsearch} ver jvm
  [[ -x $es/bin/elasticsearch ]] || return 0
  ver=$(pacman -Q elasticsearch 2>/dev/null | cut -d' ' -f2)
  jvm=$("$es/jdk/bin/java" -version 2>&1 | sed -n '2p' | sed 's/^OpenJDK Runtime Environment //')
  echo "elasticsearch: ${ver:-unknown} (system package at $es, bundled JDK link $jvm, heap ${ES_HEAP:-${TIER_SHBUF[$TIER]%B}}, ${ES_SHARDS:-1} shard(s), ${ES_PROCESSORS:-all} processors, no replica, security off)"
}
es_description
echo "sift commit: $(sift_commit)"
[[ -f $ENG/sift_ref/root/.build-info ]] && echo "sift reference build: $(cat "$ENG/sift_ref/root/.build-info")"
echo "$(quiet_report 2>&1) at start (the host is judged quiet below ${SIFT_QUIET_MAX:-10}%)"
