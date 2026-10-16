#!/usr/bin/env bash
# Usage: bench/steps/fetch.sh [sift|rum|paradedb|bm25|elasticsearch|meilisearch|typesense|xbench ...]
# (default: sift rum paradedb). Versions are pinned in bench/versions.env.
# Builds or unpacks the engines under $ROOT, never system-wide: sift (this checkout) into SIFT_ROOT, rum (PGXS)
# into RUM_ROOT, the ParadeDB pg_search package under $ROOT/engines. Downloads land in their own dirs.
# SIFT_SRC=worktree (default) builds the tracked files of this checkout, SIFT_SRC=head the last commit.
# BM25_PKG: the pgx-bm25 Arch package to unpack. Builds run with nice and at most 4 jobs.
. "$(dirname "$0")/lib.sh"

ENG=$ROOT/engines BUILD=$ROOT/build

dl() { mkdir -p "$ENG/$1/dl"; [[ -s $ENG/$1/dl/${2##*/} ]] || curl -fsSL -o "$ENG/$1/dl/${2##*/}" "$2"; echo "$ENG/$1/dl/${2##*/}"; }
pgxs_install() { # srcdir destdir
  nice make -s -C "$1" USE_PGXS=1 -j4 && make -s -C "$1" USE_PGXS=1 DESTDIR="$2" install
}

# built tree differs from the last commit: only for SIFT_SRC=worktree
sift_dirty() {
  [[ ${SIFT_SRC:-worktree} == head ]] || git -C "$REPO" diff --quiet HEAD -- . ':!bench' ':!docs' ':!packaging' 2>/dev/null || echo "+uncommitted changes"
}

fetch_sift() {
  local src=$BUILD/sift-src
  rm -rf "$src" "$SIFT_ROOT"; mkdir -p "$src"
  if [[ ${SIFT_SRC:-worktree} == head ]]; then git -C "$REPO" archive HEAD | tar -x -C "$src"
  else (cd "$REPO" && git ls-files -z -- . ':!bench' ':!docs' ':!packaging' | tar --null -T - -cf -) | tar -x -C "$src"; fi
  pgxs_install "$src" "$SIFT_ROOT" >/dev/null
  echo "$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$(sift_dirty)" >"$SIFT_ROOT/.build-info"
}

fetch_rum() {
  local tgz src=$BUILD/rum-$RUM_VERSION
  tgz=$(dl rum "https://github.com/postgrespro/rum/archive/refs/tags/$RUM_VERSION.tar.gz")
  rm -rf "$src" "$RUM_ROOT"; mkdir -p "$src"
  tar -xzf "$tgz" -C "$src" --strip-components=1
  pgxs_install "$src" "$RUM_ROOT" >/dev/null
}

fetch_bm25() { mkdir -p "$BM25_ROOT"; bsdtar -xf "${BM25_PKG:-$HOME/pkg/pgx-bm25/pgx-bm25-1.0.0-1-x86_64.pkg.tar.zst}" -C "$BM25_ROOT"; }

fetch_paradedb() {
  local deb data; deb=$(dl paradedb "https://github.com/paradedb/paradedb/releases/download/v$PARADEDB_VERSION/postgresql-$PG_MAJOR-pg-search_$PARADEDB_VERSION-1PARADEDB-noble_amd64.deb")
  data=$(ar t "$deb" | grep '^data\.tar')
  mkdir -p "$ENG/paradedb/root"
  case $data in
    *.xz)  ar p "$deb" "$data" | tar -xJf - -C "$ENG/paradedb/root" ;;
    *.zst) ar p "$deb" "$data" | tar --zstd -xf - -C "$ENG/paradedb/root" ;;
    *.gz)  ar p "$deb" "$data" | tar -xzf - -C "$ENG/paradedb/root" ;;
  esac
}

fetch_elasticsearch() {
  local tgz; tgz=$(dl elasticsearch "https://artifacts.elastic.co/downloads/elasticsearch/elasticsearch-$ES_VERSION-linux-x86_64.tar.gz")
  [[ -d $ENG/elasticsearch/es ]] || { mkdir -p "$ENG/elasticsearch/es"; tar -xzf "$tgz" -C "$ENG/elasticsearch/es" --strip-components=1; }
}

fetch_meilisearch() {
  local bin; bin=$(dl meilisearch "https://github.com/meilisearch/meilisearch/releases/download/v$MEILI_VERSION/meilisearch-linux-amd64")
  chmod +x "$bin"
}

fetch_typesense() {
  local tgz; tgz=$(dl typesense "https://dl.typesense.org/releases/$TYPESENSE_VERSION/typesense-server-$TYPESENSE_VERSION-linux-amd64.tar.gz")
  tar -xzf "$tgz" -C "$ENG/typesense/dl"
}

fetch_xbench() {
  (cd "$BENCH/xbench" && CARGO_TARGET_DIR=$ROOT/target nice cargo build --release -j 4 -q)
}

mkdir -p "$BUILD"
for t in ${*:-sift rum paradedb}; do "fetch_$t"; log "fetched $t"; done
