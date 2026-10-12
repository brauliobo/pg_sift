#!/usr/bin/env bash
# Usage: bench/fetch.sh [bm25|paradedb|elasticsearch|meilisearch|typesense|xbench ...] (default: all)
# Unpacks third-party engines under $ROOT (no system install). Downloads land in their own dirs.
. "$(dirname "$0")/lib.sh"

ES_VERSION=${ES_VERSION:-9.5.5} PARADEDB_VERSION=${PARADEDB_VERSION:-0.26.1}
MEILI_VERSION=${MEILI_VERSION:-1.54.3} TYPESENSE_VERSION=${TYPESENSE_VERSION:-30.2}
ENG=$ROOT/engines

dl() { mkdir -p "$ENG/$1/dl"; [[ -s $ENG/$1/dl/${2##*/} ]] || curl -fsSL -o "$ENG/$1/dl/${2##*/}" "$2"; echo "$ENG/$1/dl/${2##*/}"; }

fetch_bm25() { mkdir -p "$BM25_ROOT"; bsdtar -xf $HOME/pkg/pgx-bm25/pgx-bm25-1.0.0-1-x86_64.pkg.tar.zst -C "$BM25_ROOT"; }

fetch_paradedb() {
  local deb; deb=$(dl paradedb "https://github.com/paradedb/paradedb/releases/download/v$PARADEDB_VERSION/postgresql-18-pg-search_$PARADEDB_VERSION-1PARADEDB-noble_amd64.deb")
  mkdir -p "$ENG/paradedb/root"
  bsdtar -xOf "$deb" data.tar.xz | bsdtar -xf - -C "$ENG/paradedb/root"
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

for t in ${*:-bm25 paradedb elasticsearch meilisearch typesense xbench}; do "fetch_$t"; log "fetched $t"; done
