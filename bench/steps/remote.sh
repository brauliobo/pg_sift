#!/usr/bin/env bash
# Benchmark on a remote host with a user-local PostgreSQL build, extensions, Rust toolchain and
# engines under ~/$RDIR (no sudo, nothing system-wide, never rsync --delete).
#   bench/steps/remote.sh sync          copy the repo (without .git and ignored files), Q/F dumps, pgx-bm25 source
#   bench/steps/remote.sh setup         build PG 18.6 + contrib, rum, pg_textsearch, pgvector, pgx-bm25 into
#                                 ~/$RDIR/pg18; ParadeDB package, Elasticsearch tarball; rustup + xbench (tantivy)
#   bench/steps/remote.sh sift          build sift from the synced tree and install it into ~/$RDIR/bench-root/engines/sift/root
#   bench/steps/remote.sh quick [ENGINE...]  run the standard suite there (bench/steps/quick.sh, timed sections hold the shared perf lock,
#                                 sift rebuilt first when selected) and pull the results
#   bench/steps/remote.sh run 'CMD'     run CMD detached in the remote repo with the remote env (log: bench-root/logs)
#   bench/steps/remote.sh sh 'CMD'      run CMD in the foreground
#   bench/steps/remote.sh pull          copy the remote bench/out to bench/out-$RHOST
#   bench/steps/remote.sh clean [all]   stop clusters, drop builds and bench data (keeps the dumps); `all` removes ~/$RDIR
# Example: bench/steps/remote.sh quick gin tantivy; bench/steps/remote.sh run 'bench/steps/quick.sh --full gin'
# Env: RHOST (required, an ssh host), RDIR (Projects/sift), BM25_SRC (pgx-bm25 source tarball, for sync/setup),
#      RUST_TARGET_CPU (e.g. znver2, for the xbench build).
set -euo pipefail
BENCH=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(dirname "$BENCH")
RHOST=${RHOST:?set RHOST to the ssh host} RDIR=${RDIR:-Projects/sift}
BM25_SRC=${BM25_SRC:-}
CACHE=${SIFT_BENCH_ROOT:-/tmp/sift-bench}/cache

ENV="D=\$HOME/$RDIR; export D PATH=\$D/pg18/bin:\$D/.cargo/bin:\$PATH RUSTUP_HOME=\$D/.rustup CARGO_HOME=\$D/.cargo \
SIFT_BENCH_ROOT=\$D/bench-root RUSTFLAGS='${RUST_TARGET_CPU:+-C target-cpu=$RUST_TARGET_CPU}'; cd \$D"
rsh() { ssh "$RHOST" "$ENV; $1"; }

setup_script() {
  cat <<'EOF'
set -euo pipefail
. "$D/bench/versions.env"
PG=18.6 RUM=$RUM_VERSION TEXTSEARCH=$PG_TEXTSEARCH_VERSION PGVECTOR=0.8.7
mkdir -p "$D/bench-root/build" && cd "$D/bench-root/build"
gh() { [[ -d $2 ]] || curl -fsSL "https://github.com/$1/archive/refs/tags/$3.tar.gz" | tar xz; }
if [[ ! -x $D/pg18/bin/postgres ]]; then
  [[ -d postgresql-$PG ]] || curl -fsSL "https://ftp.postgresql.org/pub/source/v$PG/postgresql-$PG.tar.bz2" | tar xj
  (cd "postgresql-$PG" && ./configure -q --prefix="$D/pg18" --with-icu --with-openssl --with-lz4 --with-zstd \
     && nice make -s -j4 world-bin && make -s install-world-bin)
fi
gh postgrespro/rum "rum-$RUM" "$RUM"
gh timescale/pg_textsearch "pg_textsearch-$TEXTSEARCH" "v$TEXTSEARCH"
gh pgvector/pgvector "pgvector-$PGVECTOR" "v$PGVECTOR"
[[ -d pgx-bm25-1.0.0 ]] || tar xzf pgx-bm25-1.0.0.tar.gz
for x in "rum-$RUM" "pg_textsearch-$TEXTSEARCH" "pgvector-$PGVECTOR" pgx-bm25-1.0.0; do
  nice make -s -C "$x" USE_PGXS=1 -j4 && make -s -C "$x" USE_PGXS=1 install
done
command -v rustup >/dev/null ||
  curl -fsSL https://sh.rustup.rs | sh -s -- -y -q --profile minimal --default-toolchain none --no-modify-path
cd "$D" && bench/steps/fetch.sh pg_search elasticsearch tantivy
EOF
}

case ${1:-} in
  sync)
    rsh "mkdir -p \$D/bench-root/cache \$D/bench-root/build"
    rsync -a --exclude .git --exclude 'bench/out*/' --filter=':- .gitignore' "$REPO/" "$RHOST:$RDIR/"
    rsync -a "$CACHE"/sift-Q-*.dump "$CACHE"/sift-F-*.dump "$RHOST:$RDIR/bench-root/cache/"
    [[ -z $BM25_SRC ]] || rsync -a "$BM25_SRC" "$RHOST:$RDIR/bench-root/build/" ;;
  setup) rsh "bash -s" < <(setup_script) ;;
  run)   rsh "mkdir -p \$SIFT_BENCH_ROOT/logs; setsid nohup bash -c $(printf %q "$2") >\$SIFT_BENCH_ROOT/logs/run-\$(date +%s).log 2>&1 </dev/null &" ;;
  sh)    rsh "$2" ;;
  sift)  rsh "nice make -s USE_PGXS=1 -j4 && make -s USE_PGXS=1 DESTDIR=\$SIFT_BENCH_ROOT/engines/sift/root install" ;;
  quick) case " ${*:2} " in *" sift "*|"  ") "$0" sift ;; esac
         rsh "bench/steps/fetch.sh tantivy && uptime && bench/steps/quick.sh ${*:2}"; "$0" pull ;;
  pull)  rsync -a "$RHOST:$RDIR/bench/out/" "$BENCH/out-$RHOST/" ;;
  clean)
    rsh "for t in Q F; do TIER=\$t bench/steps/cluster.sh destroy; done; rm -rf \$D/bench-root/pg \$D/bench-root/engines \$D/bench-root/data \$D/bench-root/target \$D/bench-root/build"
    [[ ${2:-} == all ]] && rsh "cd; rm -rf \$D" ;;
  *) sed -n '2,12p' "$0" >&2; exit 1 ;;
esac
